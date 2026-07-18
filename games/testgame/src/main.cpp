// ============================================================================
// SAGE TestGame — «боевая» тестовая игра движка (см. TestGameLayer.h).
//
// Назначение — стресс-тест: полный игровой цикл (игрок/коллизии/комнаты/
// подборы/враги/HUD/аудио/модели/тени/пост-процесс) на подсистемах движка,
// у которых после удаления The Boat не осталось реальных потребителей.
// Найденные здесь баги — это и есть ценность игры (README, «games/testgame»).
// ============================================================================
#include <cstdlib>
#include <memory>
#include <string>

#include "sage/core/GameModule.h"
#include "sage/core/Log.h"
#include "sage/core/Config.h"
#include "sage/core/Version.h"
#include "TestGameLayer.h"

sage::Application* sage::CreateApplication(int /*argc*/, char** /*argv*/) {
    Log::Init("sage_engine.log");
    LOG_INFO("TestGame") << "SAGE TestGame v" << kSageEngineVersion << " запускается...";

    // Гибкие настройки: файл sage.cfg рядом с игрой + env-оверрайды.
    sage::EngineConfig cfg;
    cfg.Title = std::string("SAGE TestGame v") + kSageEngineVersion;
    cfg.LoadFile("sage.cfg");
    cfg.ApplyEnvOverrides();
    sage::EngineConfig::Set(cfg);

    sage::AppConfig config = sage::AppConfig::FromEngineConfig(cfg);

    auto* app = new sage::Application(config);
    app->PushLayer(std::make_unique<TestGameLayer>());
    return app;
}

SAGE_MAIN()
