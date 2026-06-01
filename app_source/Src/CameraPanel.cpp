#include "CameraPanel.h"

#include "TcpJpegReceiver.h"

#include <android/log.h>
#include <chrono>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace OVRFW {

namespace {
constexpr const char* LOG_TAG = "QuestCameraPanel";

constexpr const char* VERTEX_SHADER = R"glsl(
attribute vec4 Position;
attribute vec2 TexCoord;
varying highp vec2 oTexCoord;
void main()
{
    gl_Position = TransformVertex(Position);
    oTexCoord = TexCoord;
}
)glsl";

constexpr const char* FRAGMENT_SHADER = R"glsl(
uniform sampler2D Texture0;
varying highp vec2 oTexCoord;
void main()
{
    gl_FragColor = texture2D(Texture0, oTexCoord);
}
)glsl";

constexpr float DEFAULT_PANEL_HEIGHT = 1.125f;

} // namespace

CameraPanel::CameraPanel()
    : Receiver(nullptr)
    , PanelTexture()
    , UploadedSequence(0)
    , TextureWidth(0)
    , TextureHeight(0)
    , Initialized(false)
    , DecodedWidth(0)
    , DecodedHeight(0)
    , DecodedSequence(0)
    , DecoderRunning(false) {
    SurfaceDef.surfaceName = "CameraPanel";
}

CameraPanel::~CameraPanel() {
    Shutdown();
}

bool CameraPanel::Init(TcpJpegReceiver* receiver) {
    Receiver = receiver;

    ovrProgramParm parms[] = {
        {"ModelMatrix", ovrProgramParmType::FLOAT_MATRIX4},
        {"Texture0", ovrProgramParmType::TEXTURE_SAMPLED},
    };
    Program = GlProgram::Build(
        VERTEX_SHADER, FRAGMENT_SHADER, parms, sizeof(parms) / sizeof(parms[0]));
    if (!Program.IsValid()) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "Failed to build shader program");
        return false;
    }

    Geometry = BuildTesselatedQuad(1, 1);

    glGenTextures(1, &PanelTexture.texture);
    PanelTexture.target = GL_TEXTURE_2D;
    glBindTexture(GL_TEXTURE_2D, PanelTexture.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    uint8_t defaultPixel[4] = {0, 0, 0, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, defaultPixel);
    PanelTexture.Width = 1;
    PanelTexture.Height = 1;
    TextureWidth = 1;
    TextureHeight = 1;

    SurfaceDef.geo = Geometry;
    SurfaceDef.graphicsCommand.Program = Program;
    SurfaceDef.graphicsCommand.Textures[0] = PanelTexture;
    SurfaceDef.graphicsCommand.GpuState.depthEnable = false;
    SurfaceDef.graphicsCommand.GpuState.depthMaskEnable = false;
    SurfaceDef.graphicsCommand.GpuState.blendEnable = ovrGpuState::BLEND_ENABLE;
    SurfaceDef.graphicsCommand.GpuState.blendSrc = GL_SRC_ALPHA;
    SurfaceDef.graphicsCommand.GpuState.blendDst = GL_ONE_MINUS_SRC_ALPHA;
    SurfaceDef.graphicsCommand.GpuState.cullEnable = false;
    SurfaceDef.graphicsCommand.BindUniformTextures();

    ModelMatrix = OVR::Matrix4f::Translation(OVR::Vector3f(0.0f, 1.2f, -2.0f)) *
                  OVR::Matrix4f::Scaling(OVR::Vector3f(0.75f, DEFAULT_PANEL_HEIGHT * 0.5f, 1.0f));

    Initialized = true;
    DecoderRunning = true;
    DecoderThread = std::thread(&CameraPanel::DecoderThreadFunc, this);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Camera panel 3D initialized");
    return true;
}

void CameraPanel::Shutdown() {
    DecoderRunning = false;
    if (DecoderThread.joinable()) {
        DecoderThread.join();
    }
    if (Initialized) {
        DeleteTexture(PanelTexture);
        Geometry.Free();
        GlProgram::Free(Program);
        Initialized = false;
    }
}

void CameraPanel::RenderFrame(std::vector<ovrDrawSurface>& surfaceList) {
    if (!Initialized || !Program.IsValid()) {
        return;
    }
    UploadLatestFrame();
    surfaceList.push_back(ovrDrawSurface(ModelMatrix, &SurfaceDef));
}

bool CameraPanel::UploadLatestFrame() {
    std::lock_guard<std::mutex> lock(DecodedMutex);
    if (UploadedSequence >= DecodedSequence) {
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, PanelTexture.texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (DecodedWidth != TextureWidth || DecodedHeight != TextureHeight) {
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA, DecodedWidth, DecodedHeight, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, DecodedPixels.data());
        PanelTexture.Width = DecodedWidth;
        PanelTexture.Height = DecodedHeight;
        TextureWidth = DecodedWidth;
        TextureHeight = DecodedHeight;

        float aspect = static_cast<float>(DecodedWidth) / static_cast<float>(DecodedHeight);
        float panelH = DEFAULT_PANEL_HEIGHT;
        float panelW = panelH * aspect;
        if (panelW > 2.0f) {
            panelW = 2.0f;
            panelH = panelW / aspect;
        }
        ModelMatrix = OVR::Matrix4f::Translation(OVR::Vector3f(0.0f, 1.2f, -2.0f)) *
                      OVR::Matrix4f::Scaling(OVR::Vector3f(panelW * 0.5f, panelH * 0.5f, 1.0f));

        SurfaceDef.graphicsCommand.Textures[0] = PanelTexture;
        SurfaceDef.graphicsCommand.BindUniformTextures();
    } else {
        glTexSubImage2D(
            GL_TEXTURE_2D, 0, 0, 0, DecodedWidth, DecodedHeight,
            GL_RGBA, GL_UNSIGNED_BYTE, DecodedPixels.data());
    }

    UploadedSequence = DecodedSequence;
    return true;
}

void CameraPanel::DecoderThreadFunc() {
    uint64_t lastSequence = 0;

    while (DecoderRunning) {
        uint64_t sequence = 0;
        std::vector<uint8_t> jpeg;
        if (!Receiver->CopyLatestJpeg(jpeg, sequence)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            continue;
        }
        if (sequence == lastSequence) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            continue;
        }

        int width = 0;
        int height = 0;
        int channels = 0;
        unsigned char* pixels = stbi_load_from_memory(
            jpeg.data(), static_cast<int>(jpeg.size()), &width, &height, &channels, 4);
        if (pixels == nullptr || width <= 0 || height <= 0) {
            if (pixels != nullptr) {
                stbi_image_free(pixels);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(DecodedMutex);
            size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
            DecodedPixels.assign(pixels, pixels + pixelCount);
            DecodedWidth = width;
            DecodedHeight = height;
            DecodedSequence = sequence;
        }

        stbi_image_free(pixels);
        lastSequence = sequence;
    }
}

} // namespace OVRFW
