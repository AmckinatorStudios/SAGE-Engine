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
#include "sage/core/Config.h"
#include "sage/core/Version.h"
#include "PlayerLayer.h"

namespace fs = std::filesystem;

sage::Application* sage::CreateApplication(int argc, char** argv) {
    Log::Init("sage_player.log");
    LOG_INFO("Player") << "SAGE Player v" << kSageEngineVersion << " запускается...";

    // Порядок поиска проекта: аргумент -> окружение -> ./project -> текущая папка.
    // Аргументы вида «--ключ=значение» проектом не считаются: это параметры
    // запуска для скриптов игры (LaunchArg), см. ниже.
    fs::path projectDir;
    std::string launchArgs;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--", 0) == 0) launchArgs += (launchArgs.empty() ? "" : " ") + arg;
        else if (projectDir.empty()) projectDir = arg;
    }
    if (const char* env = std::getenv("SAGE_GAME_ARGS"))
        launchArgs += (launchArgs.empty() ? "" : " ") + std::string(env);

    std::error_code ec;
    if (!projectDir.empty()) { /* путь взят из аргументов */ }
    else if (const char* env = std::getenv("SAGE_PROJECT")) projectDir = env;
    else if (fs::exists("project/project.sageproj", ec)) projectDir = "project";
    else projectDir = ".";
    projectDir = fs::absolute(projectDir, ec);

    // Гибкие настройки: файл (рядом с игрой и/или в проекте) + env-оверрайды.
    // Приоритет: значения по умолчанию -> sage.cfg (CWD) -> <проект>/sage.cfg -> env.
    sage::EngineConfig cfg;
    cfg.Title = "SAGE Player";
    cfg.LoadFile("sage.cfg");
    cfg.LoadFile((projectDir / "sage.cfg").string());
    cfg.ApplyEnvOverrides();
    sage::EngineConfig::Set(cfg);

    sage::AppConfig config = sage::AppConfig::FromEngineConfig(cfg);
    config.Title = "SAGE Player"; // заменится именем проекта после загрузки

    auto* app = new sage::Application(config);
    app->PushLayer(std::make_unique<PlayerLayer>(projectDir, launchArgs));
    return app;
}

SAGE_MAIN()
