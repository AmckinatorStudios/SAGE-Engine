#include "rhi/opengl/OpenGLDevice.h"
#include "sage/core/Log.h"
#include <glad/glad.h>
#include <stdexcept>

namespace sage::rhi {

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

void OpenGLDevice::SetCullFace(bool enabled) {
    if (enabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
}

void OpenGLDevice::BindTexture2D(int unit, unsigned int nativeHandle) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, nativeHandle);
}

} // namespace sage::rhi
