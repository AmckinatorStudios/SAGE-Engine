// ============================================================================
// The Boat (альфа) на SAGE Engine — точка входа.
//
// Вся игровая логика и проходы отрисовки живут в TheBoatLayer; здесь остаётся
// только то, что игра сообщает движку до старта: конфигурация окна и какой слой
// добавить. Главный цикл, окно и тайминг принадлежат движку (sage::Application),
// точку входа даёт макрос SAGE_MAIN — игра не дублирует цикл и обработку ошибок.
//
// Концепт игры: спокойный бесконечный океан, корабль целиком из игровых блоков
// (ломается/строится), рыбалка, крафт из выловленного мусора, четыре стата и
// смена суток. Управление и механики — см. TheBoatLayer / src/game/.
// ============================================================================
#include <cstdlib>
#include <memory>
#include <string>

#include "sage/core/GameModule.h"
#include "sage/core/Log.h"
#include "sage/core/Version.h"
#include "TheBoatLayer.h"

sage::Application* sage::CreateApplication(int /*argc*/, char** /*argv*/) {
    Log::Init("sage_engine.log");
    LOG_INFO("Game") << "The Boat (alpha) v" << kSageEngineVersion << " запускается...";

    // Размер окна настраивается через переменные окружения — тот же паттерн,
    // что и остальные SAGE_* debug-переопределения (см. TheBoatLayer).
    sage::AppConfig config;
    config.Width = 1280;
    config.Height = 720;
    if (const char* w = std::getenv("SAGE_WINDOW_WIDTH")) config.Width = std::atoi(w);
    if (const char* h = std::getenv("SAGE_WINDOW_HEIGHT")) config.Height = std::atoi(h);
    config.Title = std::string("The Boat (alpha) v") + kSageEngineVersion + " - SAGE Engine";

    auto* app = new sage::Application(config);
    app->PushLayer(std::make_unique<TheBoatLayer>());
    return app;
}

SAGE_MAIN()
