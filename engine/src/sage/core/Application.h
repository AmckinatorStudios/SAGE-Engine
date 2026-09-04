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

    // Секунды с запуска. Не «удобство»: без него игре, которой нужно время
    // (качание воды, мигание, любой цикл), приходилось звать glfwGetTime() —
    // то есть включать GLFW и знать, что движок стоит на нём. Ровно та же
    // протечка реализации, что и с кодами клавиш (см. sage/core/Keys.h).
    //
    // Считается сложением кадровых dt, а не спрашивается у окна: это ТО ЖЕ
    // время, которым живёт игровая логика. Время окна идёт мимо ограничения
    // dt сверху и мимо паузы, и совпадать они перестают при первой же
    // просадке кадра — а игра, у которой анимация и логика идут по разным
    // часам, расходится незаметно и необъяснимо.
    double Time() const { return m_time; }

    static Application& Get() { return *s_instance; }

private:
    AppConfig m_config;
    std::unique_ptr<Window> m_window;
    std::unique_ptr<rhi::GraphicsDevice> m_device;
    std::vector<std::unique_ptr<Layer>> m_layers;

    bool m_running = true;
    float m_deltaTime = 0.0f;
    double m_time = 0.0;   // секунды с запуска (сумма кадровых dt)
    unsigned long long m_frameIndex = 0;  // номер кадра — для диагностики графики
    float m_fps = 0.0f;

    static Application* s_instance;
};

} // namespace sage
