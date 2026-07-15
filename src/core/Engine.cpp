#include "Engine.h"
#include "Game.h"
#include "Log.h"
#include "../render/Screenshot.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstdlib>

namespace {
    // Размеры окна можно переопределить через env — общий отладочный приём
    // движка (не редактировать код ради другого разрешения при разработке/
    // тестировании). Значения по умолчанию приходят из EngineConfig игры.
    int ResolveWidth(const EngineConfig& config) {
        if (const char* w = std::getenv("SAGE_WINDOW_WIDTH")) return std::atoi(w);
        return config.Width;
    }
    int ResolveHeight(const EngineConfig& config) {
        if (const char* h = std::getenv("SAGE_WINDOW_HEIGHT")) return std::atoi(h);
        return config.Height;
    }
}

Engine::Engine(const EngineConfig& config)
    : m_window(ResolveWidth(config), ResolveHeight(config), config.Title) {
    // Ввод: подключаем колбэки мыши/колеса к окну. Раскладку действий
    // регистрирует уже сама игра (это её знание о клавишах), движок лишь
    // владеет системой ввода и опрашивает её каждый кадр.
    m_input.Attach(m_window.Handle());

    if (const char* pathEnv = std::getenv("SAGE_SCREENSHOT_PATH")) m_screenshotPath = pathEnv;
    if (const char* frameEnv = std::getenv("SAGE_SCREENSHOT_AT_FRAME")) m_autoScreenshotFrame = std::atoi(frameEnv);
}

Engine::~Engine() = default;

float Engine::Time() const {
    return (float)glfwGetTime();
}

void Engine::RequestClose() {
    glfwSetWindowShouldClose(m_window.Handle(), true);
}

void Engine::TakeScreenshot() {
    SaveScreenshot(m_screenshotPath, m_window.Width(), m_window.Height());
}

void Engine::Run(Game& game) {
    game.OnStart(*this);

    float lastFrame = (float)glfwGetTime();
    float fpsTimer = 0.0f;
    int fpsFrames = 0;

    while (!m_window.ShouldClose()) {
        float currentFrame = (float)glfwGetTime();
        m_deltaTime = std::min(currentFrame - lastFrame, 0.05f); // защита от рывка после паузы/лага
        lastFrame = currentFrame;

        fpsTimer += m_deltaTime;
        ++fpsFrames;
        if (fpsTimer >= 0.5f) { m_fps = fpsFrames / fpsTimer; fpsTimer = 0.0f; fpsFrames = 0; }

        // Опрос ввода: один вызов прогоняет ВСЕ зарегистрированные игрой
        // действия через их привязки. Дальше игра читает именованные действия.
        m_input.Update(m_window.Handle());

        game.OnUpdate(*this, m_deltaTime);
        game.OnRender(*this);

        // Авто-скриншот на заданном кадре — читает уже отрисованный кадр из
        // back-буфера ДО swap. Общая тестовая функция движка.
        ++m_frameCount;
        if (m_autoScreenshotFrame >= 0 && m_frameCount == m_autoScreenshotFrame) {
            TakeScreenshot();
            RequestClose();
        }

        m_window.SwapBuffers();
        m_window.PollEvents();
    }

    game.OnShutdown(*this);
}
