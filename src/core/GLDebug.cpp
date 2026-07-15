#include "GLDebug.h"
#include "Log.h"

namespace {
    const char* GlErrorName(GLenum err) {
        switch (err) {
            case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
            case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
            case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
            case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
            case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
            default: return "GL_UNKNOWN_ERROR";
        }
    }
}

void CheckGlError(const char* expr, const char* file, int line) {
    // GL может копить несколько ошибок подряд — вычитываем все, чтобы
    // следующий вызов не унаследовал чужую ошибку из прошлого кадра.
    while (GLenum err = glGetError()) {
        LOG_ERROR("OpenGL") << GlErrorName(err) << " после " << expr
                             << " (" << file << ":" << line << ")";
    }
}
