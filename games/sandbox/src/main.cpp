// ============================================================================
// SAGE Sandbox — минимальный showcase движка (не игра с геймплеем).
//
// Служит двум целям (см. README, раздел "games/sandbox"):
//   1. Референс «как сделать игру на SAGE»: тонкий main.cpp + один Layer,
//      создающий ECS-сцену, рендерящий её через один шейдер и привязывающий
//      Lua-скрипт к сущности — без движковых подсистем, которые новая игра
//      подключает сама по мере необходимости (тени, пост-процесс, аудио, UI).
//   2. Единственная запускаемая fixture для headless smoke-тестов в CI
//      (см. scripts/ci_smoke_test.sh) — сама библиотека sage_engine не exe.
//
// Главный цикл, окно и тайминг принадлежат движку (sage::Application), точку
// входа даёт макрос SAGE_MAIN — игра не дублирует цикл и обработку ошибок.
// ============================================================================
#include <cstdlib>
#include <memory>
#include <string>

#include "sage/core/GameModule.h"
#include "sage/core/Log.h"
#include "sage/core/Version.h"
#include "SandboxLayer.h"

sage::Application* sage::CreateApplication(int /*argc*/, char** /*argv*/) {
    Log::Init("sage_engine.log");
    LOG_INFO("Sandbox") << "SAGE Sandbox v" << kSageEngineVersion << " запускается...";

    // Размер окна настраивается через переменные окружения — паттерн,
    // общий для всех приложений на движке (см. также EditorLayer).
    sage::AppConfig config;
    config.Width = 1280;
    config.Height = 720;
    if (const char* w = std::getenv("SAGE_WINDOW_WIDTH")) config.Width = std::atoi(w);
    if (const char* h = std::getenv("SAGE_WINDOW_HEIGHT")) config.Height = std::atoi(h);
    config.Title = std::string("SAGE Sandbox v") + kSageEngineVersion;

    auto* app = new sage::Application(config);
    app->PushLayer(std::make_unique<SandboxLayer>());
    return app;
}

SAGE_MAIN()
