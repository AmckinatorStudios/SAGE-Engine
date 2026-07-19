#pragma once
#include <memory>
#include <string>
#include "sage/rhi/Resources.h"

// ---------------------------------------------------------------------------
// RHI (Render Hardware Interface) — абстракция графического «устройства».
//
// Смысл: движок и игры НЕ обращаются к OpenGL напрямую на уровне драйвера —
// они работают с интерфейсом GraphicsDevice. Конкретная реализация (сейчас
// OpenGL, в будущем Vulkan/D3D/Metal) живёт за этим интерфейсом в
// engine/src/rhi/<backend>/ и выбирается фабрикой Create(Backend). Чтобы
// добавить новый бэкенд, реализуют этот интерфейс — код движка и игр не
// переписывается. Это и есть требование «уметь переключаться на другие
// графические библиотеки».
//
// Интерфейс покрывает два уровня:
//   1) УСТРОЙСТВО: инициализация драйвера/контекста и состояние конвейера
//      (viewport, очистка, blend/depth/cull, экранный буфер, чтение пикселей).
//   2) РЕСУРСЫ: фабрики GPU-объектов (шейдеры, геометрия, текстуры,
//      рендер-таргеты) — см. sage/rhi/Resources.h.
// ---------------------------------------------------------------------------
namespace sage::rhi {

enum class Backend {
    OpenGL,
    // Vulkan, D3D12, Metal — later
};

// Резолвер адресов функций графического API (даёт оконная система, напр. GLFW
// через glfwGetProcAddress). Бэкенд использует его в Init() для загрузки драйвера.
using ProcLoader = void* (*)(const char*);

class GraphicsDevice {
public:
    virtual ~GraphicsDevice() = default;

    // Загружает драйвер (через loader) и выставляет дефолтное состояние
    // конвейера, общее для всего движка (тест глубины, отсечение задних граней,
    // бесшовные cubemap). Вызывается один раз после создания окна/контекста.
    virtual void Init(ProcLoader loader) = 0;

    virtual const char* BackendName() const = 0;
    virtual std::string ApiVersion() const = 0;

    // --- Состояние кадра / конвейера ---
    virtual void SetViewport(int x, int y, int width, int height) = 0;
    virtual void SetClearColor(float r, float g, float b, float a) = 0;
    virtual void Clear(bool color = true, bool depth = true) = 0;

    // Привязать экранный (default) кадровый буфер как цель отрисовки.
    virtual void BindDefaultFramebuffer() = 0;

    // Стандартный alpha-blending (src_alpha, 1 - src_alpha) вкл/выкл.
    virtual void SetBlend(bool enabled) = 0;
    virtual void SetDepthTest(bool enabled) = 0;
    virtual void SetDepthWrite(bool enabled) = 0;
    virtual void SetDepthFunc(DepthFunc func) = 0;      // LessEqual нужен skybox'у
    virtual void SetCullMode(CullMode mode) = 0;        // Front — depth-проход теней
    virtual void SetPolygonMode(PolygonMode mode) = 0;  // Line — каркасный рендер (wireframe)

    // Ножницы (scissor): пиксели вне прямоугольника не пишутся. Координаты —
    // как у glScissor: (x, y) — ЛЕВЫЙ НИЖНИЙ угол в пикселях фреймбуфера.
    // Используется UI для клип-масок (обрезка детей контейнера).
    virtual void SetScissor(bool enabled, int x = 0, int y = 0, int w = 0, int h = 0) = 0;

    // Аппаратное sRGB-кодирование при записи в текущий фреймбуфер (гамма-
    // коррекция «бесплатно» на выводе). Включается на время рендера СЦЕНЫ
    // напрямую в экран, когда HDR-пост-процесса нет (он делал гамму сам):
    // без этого линейный цвет шейдеров уходит на монитор сырым и картинка
    // выглядит неоправданно тёмной. Выключать перед UI/текстом — их цвета
    // уже в sRGB и повторное кодирование их пересветит.
    virtual void SetSRGBWrite(bool enabled) = 0;

    // Привязать 2D-текстуру по нативному хендлу (хендлы отдают Texture2D::
    // NativeHandle и RenderTarget::*TextureHandle) — для сэмплирования
    // вложений рендер-таргетов (карта теней, HDR-сцена) обычными шейдерами.
    virtual void BindTexture2D(int unit, unsigned int nativeHandle) = 0;

    // Дождаться завершения всех команд GPU и прочитать прямоугольник пикселей
    // текущего буфера как плотный RGB8 (для скриншотов).
    virtual void ReadPixelsRGB(int x, int y, int width, int height, unsigned char* out) = 0;

    // Максимальная поддерживаемая анизотропия фильтрации (>= 1.0).
    virtual float MaxAnisotropy() = 0;

    // --- Фабрики GPU-ресурсов (см. sage/rhi/Resources.h) ---
    virtual std::unique_ptr<ShaderProgram> CreateShaderProgram(const std::string& vertexSrc,
                                                               const std::string& fragmentSrc) = 0;
    virtual std::unique_ptr<Geometry> CreateGeometry(const VertexLayout& layout) = 0;
    virtual std::unique_ptr<Texture2D> CreateTexture2D(const Texture2DDesc& desc, const void* pixels) = 0;
    virtual std::unique_ptr<TextureCube> CreateTextureCube(const CubeFacePixels faces[6]) = 0;
    virtual std::unique_ptr<RenderTarget> CreateRenderTarget(const RenderTargetDesc& desc) = 0;

    // Фабрика бэкенда. Возвращает готовый (но ещё не Init) девайс.
    static std::unique_ptr<GraphicsDevice> Create(Backend backend);

    // Текущий девайс процесса — ставится Application после создания. Даёт
    // подсистемам доступ к устройству без протаскивания указателя повсюду.
    static GraphicsDevice& Get();
    static void SetCurrent(GraphicsDevice* device);
};

} // namespace sage::rhi
