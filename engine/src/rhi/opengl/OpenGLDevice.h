#pragma once
#include "sage/rhi/GraphicsDevice.h"

namespace sage::rhi {

// OpenGL-реализация GraphicsDevice. Это — ЕДИНСТВЕННОЕ место уровня устройства,
// где движок включает glad и трогает GL-состояние напрямую. Остальной движок
// работает через интерфейс GraphicsDevice, поэтому смена бэкенда не затрагивает
// его код. Живёт в engine/src/rhi/opengl/ (реализация, не публичный заголовок).
class OpenGLDevice : public GraphicsDevice {
public:
    void Init(ProcLoader loader) override;

    const char* BackendName() const override { return "OpenGL"; }
    std::string ApiVersion() const override;

    void SetViewport(int x, int y, int width, int height) override;
    void SetClearColor(float r, float g, float b, float a) override;
    void Clear(bool color = true, bool depth = true) override;
    void BindDefaultFramebuffer() override;

    void SetBlend(bool enabled) override;
    void SetDepthTest(bool enabled) override;
    void SetDepthWrite(bool enabled) override;
    void SetDepthFunc(DepthFunc func) override;
    void SetCullMode(CullMode mode) override;
    void SetPolygonMode(PolygonMode mode) override;
    void SetSRGBWrite(bool enabled) override;
    void SetScissor(bool enabled, int x, int y, int w, int h) override;

    void BindTexture2D(int unit, unsigned int nativeHandle) override;
    void ReadPixelsRGB(int x, int y, int width, int height, unsigned char* out) override;
    float MaxAnisotropy() override;

    // Метки времени GPU (ARB_timer_query, ядро с OpenGL 3.3).
    unsigned int CreateTimestampQuery() override;
    void DestroyTimestampQuery(unsigned int query) override;
    void WriteTimestamp(unsigned int query) override;
    bool TimestampReady(unsigned int query) override;
    unsigned long long TimestampNs(unsigned int query) override;
    bool SupportsGpuTimers() const override { return true; }

    std::unique_ptr<ShaderProgram> CreateShaderProgram(const std::string& vertexSrc,
                                                       const std::string& fragmentSrc) override;
    std::unique_ptr<Geometry> CreateGeometry(const VertexLayout& layout) override;
    std::unique_ptr<Texture2D> CreateTexture2D(const Texture2DDesc& desc, const void* pixels) override;
    std::unique_ptr<Texture3D> CreateTexture3D(const Texture3DDesc& desc, const float* pixels) override;
    std::unique_ptr<TextureCube> CreateTextureCube(const CubeFacePixels faces[6]) override;
    std::unique_ptr<RenderTarget> CreateRenderTarget(const RenderTargetDesc& desc) override;
};

} // namespace sage::rhi
