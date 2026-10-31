// ============================================================================
// SAGE Voxel World — блочная песочница на СВОЁМ рендерере движка.
//
// Проверка системы рендеринга SAGE в боевых условиях: ни одного штатного
// PBR-прохода — свой граф кадра, свои шейдеры и материалы, свой свет по
// уровням блоков, вычислительный шейдер карты света, своя пост-обработка
// (см. VoxelPipeline.h).
// ============================================================================
#include <memory>
#include <string>

#include "sage/app/GameModule.h"
#include "sage/core/Config.h"
#include "sage/core/Log.h"
#include "sage/core/Version.h"
#include "VoxelLayer.h"

sage::Application* sage::CreateApplication(int /*argc*/, char** /*argv*/) {
    Log::Init("sage_engine.log");
    LOG_INFO("Voxel") << "SAGE Voxel World v" << kSageEngineVersion << " запускается...";

    sage::EngineConfig cfg;
    cfg.Title = std::string("SAGE Voxel World v") + kSageEngineVersion;
    cfg.LoadFile("sage.cfg");
    cfg.ApplyEnvOverrides();
    sage::EngineConfig::Set(cfg);

    auto* app = new sage::Application(sage::AppConfig::FromEngineConfig(cfg));
    app->PushLayer(std::make_unique<VoxelLayer>());
    return app;
}

SAGE_MAIN()
