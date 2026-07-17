// ============================================================================
// SagePlayer — универсальный рантайм игр, собранных в редакторе SAGE.
//
// Использование:
//   SagePlayer <путь к проекту>       — явный путь (папка с project.sageproj)
//   SAGE_PROJECT=<путь> SagePlayer    — через окружение
//   SagePlayer                        — ./project (упакованная игра) либо
//                                       текущая папка, если проект прямо в ней
//
// Упакованная редактором игра (File > Build Game...) выглядит так:
//   MyGame/
//     MyGame           (копия SagePlayer)
//     assets/          (шейдеры рантайма)
//     project/         (project.sageproj, scenes/, assets/)
// ============================================================================
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "sage/core/GameModule.h"
#include "sage/core/Log.h"
#include "sage/core/Version.h"
#include "PlayerLayer.h"

namespace fs = std::filesystem;

sage::Application* sage::CreateApplication(int argc, char** argv) {
    Log::Init("sage_player.log");
    LOG_INFO("Player") << "SAGE Player v" << kSageEngineVersion << " запускается...";

    // Порядок поиска проекта: аргумент -> окружение -> ./project -> текущая папка.
    fs::path projectDir;
    std::error_code ec;
    if (argc > 1) projectDir = argv[1];
    else if (const char* env = std::getenv("SAGE_PROJECT")) projectDir = env;
    else if (fs::exists("project/project.sageproj", ec)) projectDir = "project";
    else projectDir = ".";
    projectDir = fs::absolute(projectDir, ec);

    sage::AppConfig config;
    config.Width = 1280;
    config.Height = 720;
    if (const char* w = std::getenv("SAGE_WINDOW_WIDTH")) config.Width = std::atoi(w);
    if (const char* h = std::getenv("SAGE_WINDOW_HEIGHT")) config.Height = std::atoi(h);
    config.Title = "SAGE Player"; // заменится именем проекта после загрузки

    auto* app = new sage::Application(config);
    app->PushLayer(std::make_unique<PlayerLayer>(projectDir));
    return app;
}

SAGE_MAIN()
