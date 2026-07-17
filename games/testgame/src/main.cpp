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
#include "sage/core/Version.h"
#include "TestGameLayer.h"

sage::Application* sage::CreateApplication(int /*argc*/, char** /*argv*/) {
    Log::Init("sage_engine.log");
    LOG_INFO("TestGame") << "SAGE TestGame v" << kSageEngineVersion << " запускается...";

    sage::AppConfig config;
    config.Width = 1280;
    config.Height = 720;
    if (const char* w = std::getenv("SAGE_WINDOW_WIDTH")) config.Width = std::atoi(w);
    if (const char* h = std::getenv("SAGE_WINDOW_HEIGHT")) config.Height = std::atoi(h);
    config.Title = std::string("SAGE TestGame v") + kSageEngineVersion;

    auto* app = new sage::Application(config);
    app->PushLayer(std::make_unique<TestGameLayer>());
    return app;
}

SAGE_MAIN()
