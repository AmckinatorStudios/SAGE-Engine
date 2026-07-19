#include "sage/core/Application.h"
#include "sage/core/Log.h"
#include "sage/core/Systems.h"
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

    // Окно создаёт графический контекст (но НЕ трогает GL). Затем поднимаем
    // графическое устройство выбранного бэкенда: оно грузит драйвер и выставляет
    // дефолтное состояние конвейера. После этого конструкции слоёв (и их
    // OnAttach) уже могут создавать GPU-ресурсы.
    Window::Params wp;
    wp.Mode = config.Mode;
    wp.Resizable = config.Resizable;
    wp.VSync = config.VSync;
    wp.Msaa = config.Msaa;
    m_window = std::make_unique<Window>(config.Width, config.Height, config.Title, wp);

    m_device = rhi::GraphicsDevice::Create(rhi::Backend::OpenGL);
    m_device->Init(reinterpret_cast<rhi::ProcLoader>(glfwGetProcAddress));
    // Реальный размер окна может отличаться от запрошенного (fullscreen/borderless
    // берут разрешение монитора) — берём фактический.
    m_device->SetViewport(0, 0, m_window->Width(), m_window->Height());
    rhi::GraphicsDevice::SetCurrent(m_device.get());

    // Состав и версии подсистем — в лог любой сборки (игра/редактор/рантайм).
    LogEngineSystems();
}

Application::~Application() {
    // Слои отсоединяются в конструкторно-обратном порядке, пока окно (и, значит,
    // GL-контекст) ещё живо — иначе GPU-ресурсы слоёв удалялись бы уже без
    // контекста. Само окно разрушится ниже, после того как стек слоёв пуст.
    for (auto it = m_layers.rbegin(); it != m_layers.rend(); ++it) {
        (*it)->OnDetach();
    }
    m_layers.clear();
    rhi::GraphicsDevice::SetCurrent(nullptr);
    m_device.reset();
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

        // Viewport экранного буфера держим в размер окна каждый кадр (раньше
        // это делал Window::OnResize напрямую через glViewport; теперь окно к
        // GL не обращается — за viewport отвечает графический слой).
        m_device->SetViewport(0, 0, m_window->Width(), m_window->Height());
        for (auto& layer : m_layers) layer->OnRender();

        m_window->SwapBuffers();
        m_window->PollEvents();

        // Ограничитель кадров (если задан и без VSync): досыпаем до 1/cap секунды.
        if (m_config.FrameCap > 0) {
            float target = 1.0f / (float)m_config.FrameCap;
            float frameTime = (float)glfwGetTime() - now;
            if (frameTime < target) {
                glfwWaitEventsTimeout(target - frameTime); // спит, но не игнорирует события
            }
        }
    }

    LOG_INFO("Engine") << "Завершение работы";
}

} // namespace sage
