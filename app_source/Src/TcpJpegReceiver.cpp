#include "TcpJpegReceiver.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <mutex>
#include <sys/socket.h>
#include <utility>
#include <unistd.h>

namespace OVRFW {

namespace {
constexpr const char* LOG_TAG = "QuestCameraReceiver";
constexpr uint32_t MAX_HEADER_BYTES = 64 * 1024;
constexpr uint32_t MAX_JPEG_BYTES = 8 * 1024 * 1024;
#ifndef MSG_NOSIGNAL
constexpr int MSG_NOSIGNAL = 0;
#endif
} // namespace

TcpJpegReceiver::TcpJpegReceiver(std::string host, int port)
    : Host(std::move(host)),
      Port(port),
      SocketFd(-1),
      Running(false),
      FrameSequence(0) {}

TcpJpegReceiver::~TcpJpegReceiver() {
    Stop();
}

void TcpJpegReceiver::Start() {
    if (Running.exchange(true)) {
        return;
    }
    Worker = std::thread(&TcpJpegReceiver::Run, this);
}

void TcpJpegReceiver::Stop() {
    if (!Running.exchange(false)) {
        return;
    }
    CloseSocket();
    if (Worker.joinable()) {
        Worker.join();
    }
}

void TcpJpegReceiver::SendCommand(const std::string& command) {
    const int fd = SocketFd;
    if (fd < 0) {
        return;
    }
    std::string line = command;
    if (line.empty() || line.back() != '\n') {
        line.push_back('\n');
    }
    (void)::send(fd, line.data(), line.size(), MSG_NOSIGNAL);
}

uint64_t TcpJpegReceiver::GetFrameSequence() const {
    std::lock_guard<std::mutex> lock(FrameMutex);
    return FrameSequence;
}

bool TcpJpegReceiver::CopyLatestJpeg(std::vector<uint8_t>& out, uint64_t& sequence) const {
    std::lock_guard<std::mutex> lock(FrameMutex);
    if (LatestJpeg.empty()) {
        return false;
    }
    out = LatestJpeg;
    sequence = FrameSequence;
    return true;
}

void TcpJpegReceiver::Run() {
    while (Running) {
        if (!Connect()) {
            usleep(500 * 1000);
            continue;
        }
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Connected to %s:%d", Host.c_str(), Port);
        while (Running && ReadFrame()) {}
        CloseSocket();
        if (Running) {
            __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "Disconnected; retrying...");
            usleep(500 * 1000);
        }
    }
}

bool TcpJpegReceiver::Connect() {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    const std::string portText = std::to_string(Port);
    const int gai = getaddrinfo(Host.c_str(), portText.c_str(), &hints, &result);
    if (gai != 0) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "getaddrinfo failed: %s", gai_strerror(gai));
        return false;
    }

    bool ok = false;
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        const int fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Socket connected");
            SocketFd = fd;
            ok = true;
            break;
        }
        const int err = errno;
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "connect() failed: %s (errno=%d)",
                            std::strerror(err), err);
        close(fd);
    }

    freeaddrinfo(result);
    return ok;
}

bool TcpJpegReceiver::ReadExact(void* dst, size_t size) {
    uint8_t* cursor = static_cast<uint8_t*>(dst);
    size_t remaining = size;
    while (Running && remaining > 0) {
        const ssize_t readBytes = recv(SocketFd, cursor, remaining, 0);
        if (readBytes <= 0) {
            return false;
        }
        cursor += readBytes;
        remaining -= static_cast<size_t>(readBytes);
    }
    return remaining == 0;
}

bool TcpJpegReceiver::ReadFrame() {
    uint32_t headerLenNet = 0;
    if (!ReadExact(&headerLenNet, sizeof(headerLenNet))) {
        return false;
    }
    const uint32_t headerLen = ntohl(headerLenNet);
    if (headerLen == 0 || headerLen > MAX_HEADER_BYTES) {
        return false;
    }
    std::vector<uint8_t> header(headerLen);
    if (!ReadExact(header.data(), header.size())) {
        return false;
    }

    uint32_t jpegLenNet = 0;
    if (!ReadExact(&jpegLenNet, sizeof(jpegLenNet))) {
        return false;
    }
    const uint32_t jpegLen = ntohl(jpegLenNet);
    if (jpegLen == 0 || jpegLen > MAX_JPEG_BYTES) {
        return false;
    }
    std::vector<uint8_t> jpeg(jpegLen);
    if (!ReadExact(jpeg.data(), jpeg.size())) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(FrameMutex);
        LatestJpeg.swap(jpeg);
        ++FrameSequence;
    }
    return true;
}

void TcpJpegReceiver::CloseSocket() {
    if (SocketFd >= 0) {
        shutdown(SocketFd, SHUT_RDWR);
        close(SocketFd);
        SocketFd = -1;
    }
}

} // namespace OVRFW
