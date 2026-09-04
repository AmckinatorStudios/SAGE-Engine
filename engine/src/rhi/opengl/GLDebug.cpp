#include "rhi/opengl/GLDebug.h"
#include "sage/core/Log.h"

#include <map>
#include <string>

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

// ===========================================================================
//  Отчёт о видеокарте и канал сообщений драйвера (объявления — в GLDebug.h)
// ===========================================================================
namespace {

int GlLimit(GLenum what) {
    GLint v = 0;
    glGetIntegerv(what, &v);
    return (int)v;
}

const char* Str(GLenum name) {
    const GLubyte* s = glGetString(name);
    return s ? reinterpret_cast<const char*>(s) : "?";
}

// Расширения, от которых зависит картинка. Список короткий НАМЕРЕННО: полный
// перечень (их бывает под три сотни) в логе не читает никто, а эти шесть
// объясняют конкретные различия между машинами.
struct ExtCheck { const char* Name; int* Flag; const char* Why; };

const char* SeverityName(GLenum s) {
    switch (s) {
        case GL_DEBUG_SEVERITY_HIGH: return "критично";
        case GL_DEBUG_SEVERITY_MEDIUM: return "важно";
        case GL_DEBUG_SEVERITY_LOW: return "мелочь";
        default: return "уведомление";
    }
}

const char* SourceName(GLenum s) {
    switch (s) {
        case GL_DEBUG_SOURCE_API: return "API";
        case GL_DEBUG_SOURCE_SHADER_COMPILER: return "компилятор шейдеров";
        case GL_DEBUG_SOURCE_WINDOW_SYSTEM: return "оконная система";
        case GL_DEBUG_SOURCE_THIRD_PARTY: return "сторонний слой";
        case GL_DEBUG_SOURCE_APPLICATION: return "движок";
        default: return "прочее";
    }
}

const char* TypeName(GLenum t) {
    switch (t) {
        case GL_DEBUG_TYPE_ERROR: return "ошибка";
        case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: return "устаревшее";
        case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR: return "неопределённое поведение";
        case GL_DEBUG_TYPE_PORTABILITY: return "переносимость";
        case GL_DEBUG_TYPE_PERFORMANCE: return "производительность";
        default: return "прочее";
    }
}

// Сколько одинаковых сообщений драйвера пропускаем, прежде чем замолчать.
// Драйвер повторяет своё предупреждение КАЖДЫЙ кадр, и без глушителя лог
// перестаёт быть читаемым уже через секунду — а вместе с ним теряется всё
// остальное, ради чего его и открыли.
constexpr int kSameMessageLimit = 3;

struct DebugState {
    bool Verbose = false;
    // Ключ — идентификатор сообщения драйвера: он у одного и того же
    // предупреждения не меняется, в отличие от текста.
    std::map<GLuint, int> Seen;
};

DebugState& DbgState() {
    static DebugState s;
    return s;
}

void APIENTRY GlDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity,
                              GLsizei /*length*/, const GLchar* message,
                              const void* /*user*/) {
    DebugState& st = DbgState();
    if (!st.Verbose && severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;

    int& count = st.Seen[id];
    if (count >= kSameMessageLimit) return;
    ++count;

    const bool last = (count == kSameMessageLimit);
    std::string tail = message ? message : "";
    if (last) tail += "  [дальше это сообщение подавлено]";

    // Ошибки и неопределённое поведение — уровнем ERROR: это то, ради чего
    // канал и включён, и терять их среди отладочных строк нельзя.
    if (type == GL_DEBUG_TYPE_ERROR || type == GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR ||
        severity == GL_DEBUG_SEVERITY_HIGH) {
        LOG_ERROR("GPU") << "драйвер: " << TypeName(type) << " (" << SourceName(source)
                         << ", " << SeverityName(severity) << ", id " << id << "): " << tail;
    } else {
        LOG_WARN("GPU") << "драйвер: " << TypeName(type) << " (" << SourceName(source)
                        << ", " << SeverityName(severity) << ", id " << id << "): " << tail;
    }
}

} // namespace

void LogGpuReport() {
    LOG_INFO("GPU") << "--- отчёт о видеокарте ---";
    LOG_INFO("GPU") << "  производитель: " << Str(GL_VENDOR);
    LOG_INFO("GPU") << "  модель:        " << Str(GL_RENDERER);
    LOG_INFO("GPU") << "  OpenGL:        " << Str(GL_VERSION);
    LOG_INFO("GPU") << "  GLSL:          " << Str(GL_SHADING_LANGUAGE_VERSION);

    // Лимиты, в которые движок упирается на деле. Каждый здесь потому, что
    // его нехватка даёт СВОЙ вид поломки, а не «вообще не работает».
    LOG_INFO("GPU") << "  текстурные юниты: фрагмент " << GlLimit(GL_MAX_TEXTURE_IMAGE_UNITS)
                    << ", вершинный " << GlLimit(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS)
                    << ", всего " << GlLimit(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS)
                    << "  (движку нужно 16 фрагментных)";
    LOG_INFO("GPU") << "  uniform-компонент: фрагмент "
                    << GlLimit(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS) << ", вершинный "
                    << GlLimit(GL_MAX_VERTEX_UNIFORM_COMPONENTS);
    LOG_INFO("GPU") << "  атрибутов вершины " << GlLimit(GL_MAX_VERTEX_ATTRIBS)
                    << " (движку нужно 15: 0..14 — см. Mesh.cpp), varying-компонент "
                    << GlLimit(GL_MAX_VARYING_COMPONENTS);
    LOG_INFO("GPU") << "  текстуры: 2D до " << GlLimit(GL_MAX_TEXTURE_SIZE)
                    << ", 3D до " << GlLimit(GL_MAX_3D_TEXTURE_SIZE)
                    << ", cubemap до " << GlLimit(GL_MAX_CUBE_MAP_TEXTURE_SIZE)
                    << ", буферов цвета " << GlLimit(GL_MAX_COLOR_ATTACHMENTS)
                    << ", сэмплов " << GlLimit(GL_MAX_SAMPLES);

    // Формат ЭКРАННОГО буфера: sRGB и сглаживание задаются при создании окна и
    // молча не выполняются, если карта или драйвер их не дали. Без этой строки
    // «почему у него картинка светлее» выясняется вслепую.
    GLint srgb = 0, samples = 0, depth = 0;
    glGetIntegerv(GL_SAMPLES, &samples);
    glGetIntegerv(GL_DEPTH_BITS, &depth);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK_LEFT,
                                          GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &srgb);
    // Ошибку запроса гасим: на части драйверов запрос про BACK_LEFT в
    // core-профиле отдаёт GL_INVALID_OPERATION, и это не повод шуметь в логе.
    while (glGetError() != GL_NO_ERROR) {}
    LOG_INFO("GPU") << "  экранный буфер: sRGB " << (srgb == GL_SRGB ? "да" : "нет")
                    << ", сэмплов " << samples << ", бит глубины " << depth;

    LOG_INFO("GPU") << "  расширения: KHR_debug " << (GLAD_GL_KHR_debug ? "есть" : "НЕТ")
                    << " (без него драйвер не сможет назвать причину отказа)";
    LOG_INFO("GPU") << "--- конец отчёта ---";
}

void EnableGlDebugOutput(bool verbose) {
    if (!GLAD_GL_KHR_debug || !glDebugMessageCallback) {
        LOG_WARN("GPU") << "канал сообщений драйвера недоступен (нет GL_KHR_debug): "
                           "причину отказа рендера драйвер назвать не сможет";
        return;
    }
    DbgState().Verbose = verbose;

    glEnable(GL_DEBUG_OUTPUT);
    // Синхронный режим: сообщение приходит В МОМЕНТ вызова, а не когда-нибудь
    // потом. Без него стек и лог указывают не на тот вызов, то есть отчёт
    // становится наполовину бесполезным — а именно за точностью его и включают.
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(GlDebugCallback, nullptr);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
    LOG_INFO("GPU") << "канал сообщений драйвера включён"
                    << (verbose ? " (подробный: и уведомления тоже)" : "");
}

void SweepGlErrorsOncePerFrame(unsigned long long frame) {
    static std::map<GLenum, int> seen;
    while (GLenum err = glGetError()) {
        int& n = seen[err];
        if (n >= kSameMessageLimit) continue;
        ++n;
        LOG_ERROR("GPU") << GlErrorName(err) << " на кадре " << frame
                         << " — рендер отдал ошибку, картинка может быть неверной"
                         << (n == kSameMessageLimit ? "  [дальше подавлено]" : "");
    }
}
