// ============================================================================
// SAGE Editor — редактор сцен/ECS на ImGui. Это ОТДЕЛЬНОЕ приложение поверх
// движка: линкует ту же библиотеку sage::engine, что и игры, но добавляет свой
// слой с ImGui-панелями. Игры собираются и работают без редактора, редактор
// правит движковые сцены, не зная о конкретной игре — то самое чёткое
// разделение движок / инструменты / игры.
// ============================================================================
#include <memory>

#include "sage/core/GameModule.h"
#include "sage/core/Log.h"
#include "EditorLayer.h"

sage::Application* sage::CreateApplication(int /*argc*/, char** /*argv*/) {
    Log::Init("sage_editor.log");
    LOG_INFO("Editor") << "SAGE Editor запускается...";

    sage::AppConfig config;
    config.Width = 1600;
    config.Height = 900;
    config.Title = "SAGE Editor";

    auto* app = new sage::Application(config);
    app->PushLayer(std::make_unique<EditorLayer>());
    return app;
}

SAGE_MAIN()
