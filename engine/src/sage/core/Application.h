#pragma once
#include <memory>
#include <string>
#include <vector>
#include "sage/core/Window.h"
#include "sage/core/Layer.h"
#include "sage/core/Config.h"
#include "sage/rhi/GraphicsDevice.h"

namespace sage {

// Начальная конфигурация приложения — то, что нужно движку до создания окна.
// Оконные параметры удобно заполнять из EngineConfig (см. FromEngineConfig).
struct AppConfig {
    // Желаемый графический бэкенд ("opengl"/"vulkan"/"null"). Строка, а не
    // enum: сюда она приходит из настроек, а сверка со списком бэкендов —
    // забота RHI. Недоступный бэкенд не мешает запуску, движок берёт OpenGL.
    std::string Backend = "opengl";

    int Width = 1280;
    int Height = 720;
    std::string Title = "SAGE Engine";
    float MaxDeltaTime = 0.05f; // ограничитель dt после паузы/лага (см. Run)

    WindowMode Mode = WindowMode::Windowed;
    bool Resizable = true;
    bool VSync = true;
    int FrameCap = 0; // 0 — без ограничения
    int Msaa = 0;     // сглаживание экранного буфера (GLFW_SAMPLES)

    // --- Многопоточность (JobSystem) ---
    int WorkerThreads = 0;           // 0 — авто (аппаратные потоки − 1)
    bool MultithreadedRender = true; // параллельные отсечение/подготовка кадра

    // Заполняет оконные поля из глобального/переданного EngineConfig.
    static AppConfig FromEngineConfig(const EngineConfig& cfg) {
        AppConfig a;
        a.Width = cfg.Width; a.Height = cfg.Height; a.Title = cfg.Title;
        a.Mode = cfg.Mode; a.Resizable = cfg.Resizable; a.VSync = cfg.VSync;
        a.FrameCap = cfg.FrameCap; a.Msaa = cfg.Msaa;
        a.Backend = cfg.Backend;
        a.WorkerThreads = cfg.WorkerThreads; a.MultithreadedRender = cfg.MultithreadedRender;
        return a;
    }
};

// ---------------------------------------------------------------------------
// Application — универсальный «драйвер» движка: владеет окном и контекстом,
// главным циклом и стеком слоёв. Игра и редактор — это просто наборы слоёв
// поверх одного и того же Application. Точку входа даёт движок (см.
// GameModule.h / SAGE_MAIN); игра лишь возвращает сконфигурированный
// Application с добавленными слоями из sage::CreateApplication.
// ---------------------------------------------------------------------------
class Application {
public:
    explicit Application(const AppConfig& config = {});
    virtual ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Передаёт владение слоем приложению и сразу вызывает его OnAttach
    // (GL-контекст уже создан к этому моменту — окно живёт в конструкторе).
    Layer* PushLayer(std::unique_ptr<Layer> layer);

    // Главный цикл: dt -> OnUpdate(все слои) -> OnRender(все слои) -> swap/poll.
    void Run();

    // Попросить цикл завершиться после текущего кадра.
    void Close();

    Window& GetWindow() { return *m_window; }
    rhi::GraphicsDevice& Device() { return *m_device; }
    float DeltaTime() const { return m_deltaTime; }
    float Fps() const { return m_fps; }

    static Application& Get() { return *s_instance; }

private:
    AppConfig m_config;
    std::unique_ptr<Window> m_window;
    std::unique_ptr<rhi::GraphicsDevice> m_device;
    std::vector<std::unique_ptr<Layer>> m_layers;

    bool m_running = true;
    float m_deltaTime = 0.0f;
    float m_fps = 0.0f;

    static Application* s_instance;
};

} // namespace sage
