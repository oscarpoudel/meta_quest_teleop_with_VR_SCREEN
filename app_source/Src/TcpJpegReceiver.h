#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace OVRFW {

class TcpJpegReceiver {
public:
    TcpJpegReceiver(std::string host, int port);
    ~TcpJpegReceiver();

    void Start();
    void Stop();
    void SendCommand(const std::string& command);

    uint64_t GetFrameSequence() const;
    bool CopyLatestJpeg(std::vector<uint8_t>& out, uint64_t& sequence) const;

private:
    void Run();
    bool Connect();
    bool ReadExact(void* dst, size_t size);
    bool ReadFrame();
    void CloseSocket();

    std::string Host;
    int Port;
    int SocketFd;
    std::atomic<bool> Running;
    std::thread Worker;

    mutable std::mutex FrameMutex;
    std::vector<uint8_t> LatestJpeg;
    uint64_t FrameSequence;
};

} // namespace OVRFW
