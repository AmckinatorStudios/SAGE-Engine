#pragma once
#include <memory>
#include <string>
#include <vector>
#include "sage/core/Window.h"
#include "sage/core/Layer.h"

namespace sage {

// Начальная конфигурация приложения — то, что нужно движку до создания окна.
struct AppConfig {
    int Width = 1280;
    int Height = 720;
    std::string Title = "SAGE Engine";
    float MaxDeltaTime = 0.05f; // ограничитель dt после паузы/лага (см. Run)
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
    float DeltaTime() const { return m_deltaTime; }
    float Fps() const { return m_fps; }

    static Application& Get() { return *s_instance; }

private:
    AppConfig m_config;
    std::unique_ptr<Window> m_window;
    std::vector<std::unique_ptr<Layer>> m_layers;

    bool m_running = true;
    float m_deltaTime = 0.0f;
    float m_fps = 0.0f;

    static Application* s_instance;
};

} // namespace sage
