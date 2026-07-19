#include "rhi/opengl/OpenGLDevice.h"
#include "rhi/opengl/OpenGLResources.h"
#include "sage/core/Log.h"
#include <glad/glad.h>
#include <algorithm>
#include <stdexcept>

namespace sage::rhi {

// Реализация в OpenGLResources.cpp (кэшируемый запрос лимита анизотропии).
float QueryMaxAnisotropy();

void OpenGLDevice::Init(ProcLoader loader) {
    if (!gladLoadGLLoader((GLADloadproc)loader)) {
        LOG_ERROR("RHI") << "Не удалось инициализировать glad (OpenGL)";
        throw std::runtime_error("Не удалось инициализировать glad (OpenGL)");
    }

    glEnable(GL_DEPTH_TEST);

    // Backface culling: не рисуем грани, обращённые от камеры. Все меши движка
    // используют CCW-порядок вершин при взгляде снаружи.
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    // Бесшовные cubemap — без этого на стыках граней skybox видны швы.
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    LOG_INFO("RHI") << "Графический бэкенд: OpenGL " << glGetString(GL_VERSION);
}

std::string OpenGLDevice::ApiVersion() const {
    const GLubyte* v = glGetString(GL_VERSION);
    return v ? reinterpret_cast<const char*>(v) : "unknown";
}

void OpenGLDevice::SetViewport(int x, int y, int width, int height) {
    glViewport(x, y, width, height);
}

void OpenGLDevice::SetClearColor(float r, float g, float b, float a) {
    glClearColor(r, g, b, a);
}

void OpenGLDevice::Clear(bool color, bool depth) {
    GLbitfield mask = 0;
    if (color) mask |= GL_COLOR_BUFFER_BIT;
    if (depth) mask |= GL_DEPTH_BUFFER_BIT;
    glClear(mask);
}

void OpenGLDevice::BindDefaultFramebuffer() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void OpenGLDevice::SetBlend(bool enabled) {
    if (enabled) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
}

void OpenGLDevice::SetDepthTest(bool enabled) {
    if (enabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
}

void OpenGLDevice::SetDepthWrite(bool enabled) {
    glDepthMask(enabled ? GL_TRUE : GL_FALSE);
}

void OpenGLDevice::SetDepthFunc(DepthFunc func) {
    glDepthFunc(func == DepthFunc::LessEqual ? GL_LEQUAL : GL_LESS);
}

void OpenGLDevice::SetCullMode(CullMode mode) {
    if (mode == CullMode::Off) {
        glDisable(GL_CULL_FACE);
        return;
    }
    glEnable(GL_CULL_FACE);
    glCullFace(mode == CullMode::Front ? GL_FRONT : GL_BACK);
}

void OpenGLDevice::SetPolygonMode(PolygonMode mode) {
    // GL_LINE рисует только рёбра треугольников — каркасный (wireframe) режим.
    // FRONT_AND_BACK: каркас виден с обеих сторон вне зависимости от отсечения.
    glPolygonMode(GL_FRONT_AND_BACK, mode == PolygonMode::Line ? GL_LINE : GL_FILL);
}

void OpenGLDevice::SetScissor(bool enabled, int x, int y, int w, int h) {
    if (enabled) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(x, y, std::max(w, 0), std::max(h, 0));
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

void OpenGLDevice::SetSRGBWrite(bool enabled) {
    // Кодирование линейного цвета в sRGB на записи (ядро GL с 3.0). Действует
    // только на sRGB-способные фреймбуферы (окно создаётся с GLFW_SRGB_CAPABLE);
    // для обычных RGBA8-вложений FBO — no-op, что нам и нужно (HDR-цепочка
    // пост-процесса делает гамму сама в композите).
    if (enabled) glEnable(GL_FRAMEBUFFER_SRGB);
    else glDisable(GL_FRAMEBUFFER_SRGB);
}

void OpenGLDevice::BindTexture2D(int unit, unsigned int nativeHandle) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, nativeHandle);
}

void OpenGLDevice::ReadPixelsRGB(int x, int y, int width, int height, unsigned char* out) {
    // glReadPixels обязан дождаться завершения команд по спецификации, но
    // явный glFinish — дешёвая (раз на скриншот) защита от чтения кадра
    // раньше, чем его дорисовал асинхронный/программный рендерер.
    glFinish();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, width, height, GL_RGB, GL_UNSIGNED_BYTE, out);
}

float OpenGLDevice::MaxAnisotropy() {
    return QueryMaxAnisotropy();
}

std::unique_ptr<ShaderProgram> OpenGLDevice::CreateShaderProgram(const std::string& vertexSrc,
                                                                 const std::string& fragmentSrc) {
    return std::make_unique<GLShaderProgram>(vertexSrc, fragmentSrc);
}

std::unique_ptr<Geometry> OpenGLDevice::CreateGeometry(const VertexLayout& layout) {
    return std::make_unique<GLGeometry>(layout);
}

std::unique_ptr<Texture2D> OpenGLDevice::CreateTexture2D(const Texture2DDesc& desc, const void* pixels) {
    return std::make_unique<GLTexture2D>(desc, pixels);
}

std::unique_ptr<TextureCube> OpenGLDevice::CreateTextureCube(const CubeFacePixels faces[6]) {
    return std::make_unique<GLTextureCube>(faces);
}

std::unique_ptr<RenderTarget> OpenGLDevice::CreateRenderTarget(const RenderTargetDesc& desc) {
    return std::make_unique<GLRenderTarget>(desc);
}

} // namespace sage::rhi
