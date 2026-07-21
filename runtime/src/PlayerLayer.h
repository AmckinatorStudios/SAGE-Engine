#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "sage/core/Layer.h"
#include "sage/render/Shader.h"
#include "sage/render/Camera.h"
#include "sage/render/ShadowMap.h"
#include "sage/render/SkyRenderer.h"
#include "sage/render/ParticleSystem.h"
#include "sage/ui/UIRenderer.h"
#include "sage/ecs/RenderBatch.h"
#include "sage/net/NetworkSystem.h"
#include "sage/scene/Scene.h"
#include "sage/physics/PhysicsScene.h"

class ScriptEngine;

// ---------------------------------------------------------------------------
// PlayerLayer — универсальный рантайм игр, собранных в редакторе SAGE
// (exe SagePlayer). Это «вторая половина» системы сборки игр: редактор
// упаковывает проект (File > Build Game...), плеер его ЗАПУСКАЕТ:
//
//   1. Находит проект: argv[1] -> $SAGE_PROJECT -> ./project -> текущая папка.
//   2. Грузит главную сцену: scenes/main.sage, иначе первая по алфавиту.
//   3. Привязывает Lua-скрипты всех сущностей со ScriptComponent и сразу
//      запускает симуляцию (это игра, а не редактор — Play всегда включён).
//   4. Рендерит от Primary-камеры сцены (CameraComponent) с ПОЛНЫМ освещением
//      движка: hemisphere ambient + солнце + точечные света (LightComponent)
//      + PCF-тени. Нет камеры в сцене — статичная камера-облёт (fallback).
//
// Свои шейдеры плеер грузит ДО перехода в папку проекта (они лежат рядом с
// бинарником), затем меняет CWD на корень проекта — так относительные пути
// сцены («assets/scripts/…», «assets/models/…») резолвятся внутри проекта.
//
// Env-хуки (как у всех приложений движка): SAGE_SCREENSHOT_AT_FRAME/
// SAGE_SCREENSHOT_PATH. ESC — выход.
// ---------------------------------------------------------------------------
class PlayerLayer : public sage::Layer {
public:
    explicit PlayerLayer(std::filesystem::path projectDir);
    ~PlayerLayer() override; // unique_ptr<ScriptEngine> требует полный тип в .cpp

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(float dt) override;
    void OnRender() override;

private:
    std::filesystem::path FindMainScene() const;

    std::filesystem::path m_projectDir;
    std::string m_projectName = "SAGE Game";

    std::unique_ptr<Scene> m_scene;
    std::unique_ptr<ScriptEngine> m_scripts;
    sage::net::NetworkSystem m_network;      // мультиплеер (Lua: Net.*)
    std::unique_ptr<PhysicsScene> m_physics; // симуляция физики (игра всегда «в Play»)

    std::optional<Shader> m_shader;       // lit: ambient+sun+point lights+тени
    std::optional<Shader> m_shadowShader;
    std::optional<ShadowMap> m_shadows;
    std::optional<SkyRenderer> m_sky;     // процедурный скайбокс сцены
    std::optional<ParticleSystem> m_particles; // пул частиц сцены (эмиттеры ECS)
    // UI сцены (UIElementComponent из .sage) — рисуется поверх кадра; лениво,
    // создаётся при первом кадре со сценой, содержащей UI-сущности.
    std::unique_ptr<UIRenderer> m_ui;
    sage::ecs::RenderBatch m_batch;            // отсечение по фрустуму + инстансинг статики
    Camera m_fallbackCamera; // когда в сцене нет CameraComponent
    bool m_warnedNoCamera = false; // предупреждение «нет Primary-камеры» — один раз

    std::string m_screenshotPath = "player.png";
    int m_autoScreenshotFrame = -1;
    int m_frameCounter = 0;
};
