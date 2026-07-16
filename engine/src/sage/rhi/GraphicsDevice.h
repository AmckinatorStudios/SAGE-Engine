#pragma once
#include <memory>
#include <string>

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
// Данная (первая) итоговая версия покрывает УРОВЕНЬ УСТРОЙСТВА: инициализацию
// драйвера/контекста и глобальное состояние конвейера (viewport, очистка,
// blend/depth/cull, привязка экранного буфера). Ресурсы (буферы/текстуры/
// шейдеры/кадровые буферы) переезжают за RHI следующим шагом — см. rhi/*.
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
    virtual void SetCullFace(bool enabled) = 0;

    // Привязать текстуру (нативный хендл бэкенда) к текстурному юниту. Временный
    // мост, пока ресурсы-текстуры не переехали за RHI: принимает сырой хендл,
    // который отдают ещё-не-мигрированные Texture/Framebuffer/ShadowMap.
    virtual void BindTexture2D(int unit, unsigned int nativeHandle) = 0;

    // Фабрика бэкенда. Возвращает готовый (но ещё не Init) девайс.
    static std::unique_ptr<GraphicsDevice> Create(Backend backend);

    // Текущий девайс процесса — ставится Application после создания. Даёт
    // подсистемам доступ к устройству без протаскивания указателя повсюду.
    static GraphicsDevice& Get();
    static void SetCurrent(GraphicsDevice* device);
};

} // namespace sage::rhi
