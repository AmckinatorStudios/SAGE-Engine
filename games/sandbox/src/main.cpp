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
#include "sage/core/Config.h"
#include "sage/core/Version.h"
#include "SandboxLayer.h"

sage::Application* sage::CreateApplication(int /*argc*/, char** /*argv*/) {
    Log::Init("sage_engine.log");
    LOG_INFO("Sandbox") << "SAGE Sandbox v" << kSageEngineVersion << " запускается...";

    // Гибкие настройки движка: файл sage.cfg + env-оверрайды — единый механизм
    // для всех приложений на движке (окно/vsync/тени/пост-процесс и т.д.).
    sage::EngineConfig cfg;
    cfg.Title = std::string("SAGE Sandbox v") + kSageEngineVersion;
    cfg.LoadFile("sage.cfg");
    cfg.ApplyEnvOverrides();
    sage::EngineConfig::Set(cfg);

    sage::AppConfig config = sage::AppConfig::FromEngineConfig(cfg);

    auto* app = new sage::Application(config);
    app->PushLayer(std::make_unique<SandboxLayer>());
    return app;
}

SAGE_MAIN()
