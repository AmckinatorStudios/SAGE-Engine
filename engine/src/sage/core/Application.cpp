#include "sage/core/Application.h"
#include "sage/core/Log.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <stdexcept>

namespace sage {

Application* Application::s_instance = nullptr;

Application::Application(const AppConfig& config) : m_config(config) {
    if (s_instance) {
        throw std::runtime_error("Application: уже существует экземпляр (Application — синглтон)");
    }
    s_instance = this;

    // Создание окна создаёт и OpenGL-контекст, и загружает glad — после этого
    // конструкции слоёв (и их OnAttach) уже могут работать с GPU-ресурсами.
    m_window = std::make_unique<Window>(config.Width, config.Height, config.Title);
}

Application::~Application() {
    // Слои отсоединяются в конструкторно-обратном порядке, пока окно (и, значит,
    // GL-контекст) ещё живо — иначе GPU-ресурсы слоёв удалялись бы уже без
    // контекста. Само окно разрушится ниже, после того как стек слоёв пуст.
    for (auto it = m_layers.rbegin(); it != m_layers.rend(); ++it) {
        (*it)->OnDetach();
    }
    m_layers.clear();
    m_window.reset();
    s_instance = nullptr;
}

Layer* Application::PushLayer(std::unique_ptr<Layer> layer) {
    Layer* raw = layer.get();
    m_layers.push_back(std::move(layer));
    raw->OnAttach();
    return raw;
}

void Application::Close() {
    m_running = false;
}

void Application::Run() {
    float lastFrame = (float)glfwGetTime();
    float fpsTimer = 0.0f;
    int fpsFrames = 0;

    while (m_running && !m_window->ShouldClose()) {
        float now = (float)glfwGetTime();
        // Ограничитель dt: защита от «рывка» симуляции после паузы/лага/точки
        // останова (тот же приём, что был в исходном игровом цикле).
        m_deltaTime = std::min(now - lastFrame, m_config.MaxDeltaTime);
        lastFrame = now;

        // Простой счётчик FPS (обновляется дважды в секунду) — движок отдаёт
        // его через Fps(), чтобы отладочные оверлеи не считали его сами.
        fpsTimer += m_deltaTime;
        ++fpsFrames;
        if (fpsTimer >= 0.5f) { m_fps = fpsFrames / fpsTimer; fpsTimer = 0.0f; fpsFrames = 0; }

        for (auto& layer : m_layers) layer->OnUpdate(m_deltaTime);
        for (auto& layer : m_layers) layer->OnRender();

        m_window->SwapBuffers();
        m_window->PollEvents();
    }

    LOG_INFO("Engine") << "Завершение работы";
}

} // namespace sage
