#pragma once
#include "Window.h"
#include "InputSystem.h"
#include <string>

class Game;

// Конфигурация окна/приложения, которую игра сообщает движку ДО создания
// окна (размеры и заголовок). Игра отдаёт её статически (см.
// TheBoat::DefaultConfig), потому что окно должно существовать раньше, чем
// движок начнёт крутить игру, а игра — грузить GPU-ресурсы.
struct EngineConfig {
    int Width = 1280;
    int Height = 720;
    std::string Title = "SAGE Engine";
};

// ---------------------------------------------------------------------
// Engine — «раннер» приложения: владеет окном, системой ввода, главным
// циклом и таймингом и «крутит» переданную ему Game через её хуки
// (OnStart/OnUpdate/OnRender/OnShutdown). Движок НЕ знает о конкретной игре —
// только об интерфейсе Game (см. core/Game.h). Это и есть шов между движком и
// игрой: другая игра = другая реализация Game, запущенная тем же Engine::Run,
// без изменений в коде движка.
//
// Часть ЯДРА движка. Порядок использования (см. main.cpp):
//   Engine engine(TheBoat::DefaultConfig()); // окно/GL-контекст созданы
//   TheBoat game;                            // игра грузит ресурсы (контекст уже есть)
//   engine.Run(game);                        // OnStart -> цикл -> OnShutdown
//
// ВАЖНО про порядок разрушения: объявляй Engine РАНЬШЕ игры (как выше) —
// тогда игра разрушится ПЕРВОЙ, освободив свои GL-ресурсы, пока окно и
// GL-контекст (в Engine) ещё живы. Обратный порядок = удаление GL-объектов
// после уничтожения контекста (краш при выходе).
// ---------------------------------------------------------------------
class Engine {
public:
    explicit Engine(const EngineConfig& config);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Главный цикл: OnStart один раз, затем каждый кадр
    // (тайминг -> опрос ввода -> OnUpdate -> OnRender -> авто-скриншот ->
    // swap/poll), и OnShutdown при выходе из цикла (окно/GL-контекст ещё живы).
    void Run(Game& game);

    Window& GetWindow() { return m_window; }
    InputSystem& GetInput() { return m_input; }

    float DeltaTime() const { return m_deltaTime; } // время последнего кадра, сек
    float Fps() const { return m_fps; }
    int FrameCount() const { return m_frameCount; }
    float Time() const; // секунды с запуска (glfwGetTime) — для анимаций во времени

    // Просит движок закрыть окно после текущего кадра (например, по Esc из игры).
    void RequestClose();

    // Сохраняет скриншот в путь из env-переменной SAGE_SCREENSHOT_PATH
    // (по умолчанию "screenshot.png"). Общая отладочно-тестовая функция движка,
    // не зависящая от конкретной игры (используется и хоткеем игры, и
    // авто-скриншотом по кадру).
    void TakeScreenshot();

private:
    Window m_window;
    InputSystem m_input;

    float m_deltaTime = 0.0f;
    float m_fps = 0.0f;
    int m_frameCount = 0;

    // Авто-скриншот на заданном кадре (SAGE_SCREENSHOT_AT_FRAME) для
    // headless-проверки в CI/агентом; -1 — выключено.
    int m_autoScreenshotFrame = -1;
    std::string m_screenshotPath = "screenshot.png";
};
