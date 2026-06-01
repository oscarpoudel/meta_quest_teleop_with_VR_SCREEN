#pragma once

#include <cstdint>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

#include "Render/SurfaceRender.h"
#include "Render/GlProgram.h"
#include "Render/GlTexture.h"

#include "OVR_Math.h"

namespace OVRFW {

class TcpJpegReceiver;

class CameraPanel {
public:
    CameraPanel();
    ~CameraPanel();

    bool Init(TcpJpegReceiver* receiver);
    void Shutdown();
    void RenderFrame(std::vector<ovrDrawSurface>& surfaceList);

private:
    bool UploadLatestFrame();
    void DecoderThreadFunc();

    TcpJpegReceiver* Receiver;
    GlProgram Program;
    GlTexture PanelTexture;
    GlGeometry Geometry;
    OVR::Matrix4f ModelMatrix;
    uint64_t UploadedSequence;
    int TextureWidth;
    int TextureHeight;
    bool Initialized;

    std::thread DecoderThread;
    std::mutex DecodedMutex;
    std::vector<uint8_t> DecodedPixels;
    int DecodedWidth;
    int DecodedHeight;
    uint64_t DecodedSequence;
    std::atomic<bool> DecoderRunning;

    ovrSurfaceDef SurfaceDef;
};

} // namespace OVRFW
