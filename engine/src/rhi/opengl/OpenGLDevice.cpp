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

    // ЧТО ЗА ВИДЕОКАРТА И ЧТО ОНА УМЕЕТ — В ЛОГ, СРАЗУ.
    //
    // Без этих строк разбор чужой машины начинается с переписки «а какая у вас
    // карта?». Хуже того, часть отказов рендера — прямое следствие ЛИМИТОВ, и
    // без них в логе они выглядят загадкой: движку нужно шестнадцать текстурных
    // юнитов фрагментного шейдера (полная раскладка — в ShadowMap.h), и на
    // карте, дающей меньше, обычный вид может уйти в чёрное при исправных
    // отладочных режимах — те столько текстур не трогают.
    auto limit = [](GLenum what) {
        GLint v = 0;
        glGetIntegerv(what, &v);
        return (int)v;
    };
    const GLubyte* vendor = glGetString(GL_VENDOR);
    const GLubyte* renderer = glGetString(GL_RENDERER);
    LOG_INFO("RHI") << "Графический бэкенд: OpenGL " << glGetString(GL_VERSION);
    LOG_INFO("RHI") << "Видеокарта: " << (renderer ? (const char*)renderer : "?") << " ("
                    << (vendor ? (const char*)vendor : "?") << ")";
    const int texUnits = limit(GL_MAX_TEXTURE_IMAGE_UNITS);
    LOG_INFO("RHI") << "Лимиты: текстурных юнитов фрагмента " << texUnits << ", всего "
                    << limit(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS) << ", uniform-компонент "
                    << limit(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS) << ", размер текстуры "
                    << limit(GL_MAX_TEXTURE_SIZE);
    // Шестнадцать — не пожелание, а то, из чего собрана раскладка юнитов.
    // Сказать об этом надо ГРОМКО и на месте, а не оставлять человека гадать,
    // почему у него чёрные объекты.
    if (texUnits > 0 && texUnits < 16) {
        LOG_ERROR("RHI") << "Видеокарта даёт только " << texUnits
                         << " текстурных юнитов фрагментного шейдера, движку нужно 16 — "
                            "обычный вид может выйти чёрным при исправных отладочных режимах";
    }
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

void OpenGLDevice::SetBlendMode(BlendMode mode) {
    switch (mode) {
        case BlendMode::Premultiplied: glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
        case BlendMode::Additive:      glBlendFunc(GL_ONE, GL_ONE); break;
        default:                       glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
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

void OpenGLDevice::SetFrontFace(FrontFace face) {
    glFrontFace(face == FrontFace::Clockwise ? GL_CW : GL_CCW);
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

void OpenGLDevice::BindTexture2D(int unit, sage::rhi::TextureHandle texture) {
    glActiveTexture(GL_TEXTURE0 + unit);
    // Невалидный хендл — это «отвязать», а не ошибка: вызывающему сплошь и рядом
    // нужно снять текстуру с юнита (выключенный эффект, отсутствующая карта), и
    // требовать для этого отдельного вызова значило бы плодить его копии.
    glBindTexture(GL_TEXTURE_2D, (GLuint)texture.Value);
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

std::unique_ptr<Texture3D> OpenGLDevice::CreateTexture3D(const Texture3DDesc& desc, const float* pixels) {
    return std::make_unique<GLTexture3D>(desc, pixels);
}

std::unique_ptr<TextureCube> OpenGLDevice::CreateTextureCube(const CubeFacePixels faces[6]) {
    return std::make_unique<GLTextureCube>(faces);
}

std::unique_ptr<RenderTarget> OpenGLDevice::CreateRenderTarget(const RenderTargetDesc& desc) {
    return std::make_unique<GLRenderTarget>(desc);
}

std::unique_ptr<CubeRenderTarget> OpenGLDevice::CreateCubeRenderTarget(
    const CubeRenderTargetDesc& desc) {
    return std::make_unique<GLCubeRenderTarget>(desc);
}

// --- Запись цвета -----------------------------------------------------------

void OpenGLDevice::SetColorWrite(bool enabled) {
    const GLboolean v = enabled ? GL_TRUE : GL_FALSE;
    glColorMask(v, v, v, v);
}

// --- Запросы перекрытия -----------------------------------------------------

sage::rhi::QueryHandle OpenGLDevice::CreateOcclusionQuery() {
    GLuint query = 0;
    glGenQueries(1, &query);
    return {query};
}

void OpenGLDevice::DestroyOcclusionQuery(sage::rhi::QueryHandle query) {
    if (query.Valid()) {
        GLuint q = (GLuint)query.Value;
        glDeleteQueries(1, &q);
    }
}

void OpenGLDevice::BeginOcclusionQuery(sage::rhi::QueryHandle query) {
    // GL_ANY_SAMPLES_PASSED, а не GL_SAMPLES_PASSED: точное число прошедших
    // пикселей нам не нужно, нужен факт. Приблизительный вариант драйвер вправе
    // посчитать дешевле, и на плитчатых видеокартах разница заметна.
    if (query.Valid()) glBeginQuery(GL_ANY_SAMPLES_PASSED, (GLuint)query.Value);
}

void OpenGLDevice::EndOcclusionQuery() { glEndQuery(GL_ANY_SAMPLES_PASSED); }

bool OpenGLDevice::OcclusionResultReady(sage::rhi::QueryHandle query) {
    if (!query.Valid()) return false;
    GLint available = GL_FALSE;
    glGetQueryObjectiv((GLuint)query.Value, GL_QUERY_RESULT_AVAILABLE, &available);
    return available == GL_TRUE;
}

bool OpenGLDevice::OcclusionVisible(sage::rhi::QueryHandle query) {
    if (!query.Valid()) return true; // нет запроса — считаем видимым, это безопасная сторона
    GLuint result = 0;
    glGetQueryObjectuiv((GLuint)query.Value, GL_QUERY_RESULT, &result);
    return result != 0;
}

// --- Метки времени GPU ------------------------------------------------------

sage::rhi::QueryHandle OpenGLDevice::CreateTimestampQuery() {
    GLuint query = 0;
    glGenQueries(1, &query);
    return {query};
}

void OpenGLDevice::DestroyTimestampQuery(sage::rhi::QueryHandle query) {
    if (query.Valid()) {
        GLuint q = (GLuint)query.Value;
        glDeleteQueries(1, &q);
    }
}

void OpenGLDevice::WriteTimestamp(sage::rhi::QueryHandle query) {
    // glQueryCounter, а не glBeginQuery(GL_TIME_ELAPSED): интервальный запрос
    // может быть активен только один за раз, и вложенные проходы им не
    // измерить. Метка просто просит видеокарту записать «когда сюда дошло».
    if (query.Valid()) glQueryCounter((GLuint)query.Value, GL_TIMESTAMP);
}

bool OpenGLDevice::TimestampReady(sage::rhi::QueryHandle query) {
    if (!query.Valid()) return false;
    GLint available = GL_FALSE;
    glGetQueryObjectiv((GLuint)query.Value, GL_QUERY_RESULT_AVAILABLE, &available);
    return available == GL_TRUE;
}

unsigned long long OpenGLDevice::TimestampNs(sage::rhi::QueryHandle query) {
    if (!query.Valid()) return 0;
    GLuint64 value = 0;
    // Спрашиваем результат ТОЛЬКО когда он готов (см. TimestampReady): иначе
    // этот вызов блокирует поток до конца работы видеокарты, то есть само
    // измерение и создаёт задержку, которую мы измеряем.
    glGetQueryObjectui64v((GLuint)query.Value, GL_QUERY_RESULT, &value);
    return (unsigned long long)value;
}

} // namespace sage::rhi
