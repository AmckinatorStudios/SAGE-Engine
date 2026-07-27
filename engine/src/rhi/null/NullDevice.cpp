#include "rhi/null/NullDevice.h"

#include <cstring>

#include "sage/core/Log.h"

namespace sage::rhi {
namespace {

NullCounters g_counters;

// Хендлы раздаём последовательно и никогда не повторяем: ноль означает «нет
// ресурса» во всём движке, а совпадение хендлов у разных объектов маскировало
// бы ошибки привязки — с настоящим драйвером они бы вылезли, с пустышкой нет.
unsigned int NextHandle() {
    static unsigned int next = 1;
    return next++;
}

class NullShader : public ShaderProgram {
public:
    NullShader() { ++g_counters.Shaders; }
    void Use() const override {}
    void SetMat4(const std::string&, const glm::mat4&) const override {}
    void SetMat4Array(const std::string&, const glm::mat4*, int) const override {}
    void SetIntArray(const std::string&, const int*, int) const override {}
    void SetFloatArray(const std::string&, const float*, int) const override {}
    void SetVec4(const std::string&, const glm::vec4&) const override {}
    void SetVec3(const std::string&, const glm::vec3&) const override {}
    void SetVec2(const std::string&, const glm::vec2&) const override {}
    void SetFloat(const std::string&, float) const override {}
    void SetInt(const std::string&, int) const override {}
};

class NullGeometry : public Geometry {
public:
    NullGeometry() { ++g_counters.Geometries; }
    void SetVertexData(const void*, size_t, bool) override {}
    void SetIndexData(const unsigned int*, size_t, bool) override {}
    void SetInstanceData(const void*, size_t) override {}
    void DrawIndexed(size_t) const override { ++g_counters.DrawCalls; }
    void DrawIndexedRange(size_t, size_t) const override { ++g_counters.DrawCalls; }
    void DrawArrays(size_t) const override { ++g_counters.DrawCalls; }
    void DrawInstanced(size_t, size_t) const override { ++g_counters.DrawCalls; }
    void DrawIndexedInstanced(size_t, size_t) const override { ++g_counters.DrawCalls; }
    void DrawLines(size_t) const override { ++g_counters.DrawCalls; }
};

class NullTexture2D : public Texture2D {
public:
    NullTexture2D() : m_handle(NextHandle()) { ++g_counters.Textures; }
    void Bind(int) const override {}
    unsigned int NativeHandle() const override { return m_handle; }

private:
    unsigned int m_handle;
};

class NullTexture3D : public Texture3D {
public:
    NullTexture3D() { ++g_counters.Textures; }
    void Bind(int) const override {}
};

class NullTextureCube : public TextureCube {
public:
    NullTextureCube() { ++g_counters.Textures; }
    void Bind(int) const override {}
};

class NullRenderTarget : public RenderTarget {
public:
    explicit NullRenderTarget(const RenderTargetDesc& desc)
        : m_width(desc.Width), m_height(desc.Height), m_samples(desc.Samples),
          m_color(NextHandle()), m_depth(NextHandle()) {
        ++g_counters.RenderTargets;
    }
    void Bind() const override {}
    void Resize(int width, int height) override { m_width = width; m_height = height; }
    int Width() const override { return m_width; }
    int Height() const override { return m_height; }
    int Samples() const override { return m_samples; }
    unsigned int ColorTextureHandle() const override { return m_color; }
    unsigned int DepthTextureHandle() const override { return m_depth; }

private:
    int m_width, m_height, m_samples;
    unsigned int m_color, m_depth;
};

} // namespace

void NullDevice::Init(ProcLoader) {
    LOG_INFO("RHI") << "Null-бэкенд: графического API нет, отрисовка не выполняется";
}

void NullDevice::SetViewport(int, int, int, int) { ++g_counters.StateChanges; }
void NullDevice::SetClearColor(float, float, float, float) { ++g_counters.StateChanges; }
void NullDevice::Clear(bool, bool) { ++g_counters.StateChanges; }
void NullDevice::BindDefaultFramebuffer() { ++g_counters.StateChanges; }
void NullDevice::SetBlend(bool) { ++g_counters.StateChanges; }
void NullDevice::SetDepthTest(bool) { ++g_counters.StateChanges; }
void NullDevice::SetDepthWrite(bool) { ++g_counters.StateChanges; }
void NullDevice::SetDepthFunc(DepthFunc) { ++g_counters.StateChanges; }
void NullDevice::SetCullMode(CullMode) { ++g_counters.StateChanges; }
void NullDevice::SetPolygonMode(PolygonMode) { ++g_counters.StateChanges; }
void NullDevice::SetScissor(bool, int, int, int, int) { ++g_counters.StateChanges; }
void NullDevice::SetSRGBWrite(bool) { ++g_counters.StateChanges; }
void NullDevice::BindTexture2D(int, unsigned int) { ++g_counters.StateChanges; }

void NullDevice::ReadPixelsRGB(int, int, int width, int height, unsigned char* out) {
    // Чёрный кадр, а не мусор: вызывающий имеет право читать всё, что мы
    // «вернули», и неинициализированная память дала бы недетерминированные
    // проверки там, где их причина совсем не очевидна.
    if (out && width > 0 && height > 0) std::memset(out, 0, (size_t)width * height * 3u);
}

std::unique_ptr<ShaderProgram> NullDevice::CreateShaderProgram(const std::string&,
                                                               const std::string&) {
    return std::make_unique<NullShader>();
}

std::unique_ptr<Geometry> NullDevice::CreateGeometry(const VertexLayout&) {
    return std::make_unique<NullGeometry>();
}

std::unique_ptr<Texture2D> NullDevice::CreateTexture2D(const Texture2DDesc&, const void*) {
    return std::make_unique<NullTexture2D>();
}

std::unique_ptr<Texture3D> NullDevice::CreateTexture3D(const Texture3DDesc&, const float*) {
    return std::make_unique<NullTexture3D>();
}

std::unique_ptr<TextureCube> NullDevice::CreateTextureCube(const CubeFacePixels[6]) {
    return std::make_unique<NullTextureCube>();
}

std::unique_ptr<RenderTarget> NullDevice::CreateRenderTarget(const RenderTargetDesc& desc) {
    return std::make_unique<NullRenderTarget>(desc);
}

NullCounters& NullDevice::Counters() { return g_counters; }
void NullDevice::ResetCounters() { g_counters = NullCounters{}; }

} // namespace sage::rhi
