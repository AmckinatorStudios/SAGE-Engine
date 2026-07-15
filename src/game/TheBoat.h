#pragma once
#include "../core/Game.h"
#include "../core/Engine.h"
#include "../render/Shader.h"
#include "../render/Camera.h"
#include "../render/Texture.h"
#include "../render/Skybox.h"
#include "../render/BillboardSystem.h"
#include "../render/Framebuffer.h"
#include "../render/ShadowMap.h"
#include "../render/PostProcess.h"
#include "../render/Mesh.h"
#include "../ui/UIRenderer.h"
#include "../ui/UICanvas.h"
#include "../render/DebugOverlay.h"
#include "GameState.h"
#include "PlayerActions.h"
#include <memory>

// ---------------------------------------------------------------------
// TheBoat — конкретная ИГРА на SAGE Engine, реализующая интерфейс Game.
// Всё, что раньше было навалено в main() (ресурсы, состояние партии, порядок
// отрисовки, реакция на ввод), теперь живёт здесь — а движок (Engine) про это
// ничего не знает и просто вызывает хуки OnStart/OnUpdate/OnRender.
//
// Именно это разделение и есть «движок не знает об игре»: main.cpp свёлся к
// паре строк (создать Engine, создать TheBoat, engine.Run), а чтобы сделать
// ДРУГУЮ игру, пишешь свой класс с Game-интерфейсом вместо этого — код движка
// (core/render/scene/scripting/ui) не меняется вообще.
//
// GPU-ресурсы (шейдеры/текстуры/скайбокс/буферы) грузятся в конструкторе:
// TheBoat создаётся ПОСЛЕ Engine (см. main.cpp и комментарий у Engine),
// поэтому GL-контекст и окно на момент конструирования уже существуют.
// ---------------------------------------------------------------------
class TheBoat : public Game {
public:
    // Размер/заголовок окна для движка — статически, т.к. окно создаётся
    // раньше самой игры (см. main.cpp).
    static EngineConfig DefaultConfig();

    explicit TheBoat(Engine& engine);

    void OnStart(Engine& engine) override;
    void OnUpdate(Engine& engine, float dt) override;
    void OnRender(Engine& engine) override;
    void OnShutdown(Engine& engine) override;

private:
    // --- Вспомогательные методы (были свободными функциями в main.cpp) ---
    void BuildStatsHud();                                   // собирает m_statsHud из виджетов
    void ApplyDebugEnvOverrides();                          // SAGE_CAM_*, SAGE_TIME_OF_DAY и т.п.
    glm::vec3 ReadMoveDirection(InputMap& actions) const;  // ввод -> направление движения
    void UpdateNoclipFly(InputMap& actions, float dt);     // свободный полёт (noclip)
    void DrawShadowCasters(Shader& depthShader);           // геометрия для depth-прохода теней

    // --- Камера и рендер-ресурсы ---
    Camera m_camera;

    Shader m_voxelShader;
    Shader m_waterShader;
    Shader m_basicShader;
    Shader m_skyboxShader;
    Shader m_particleShader;
    Shader m_billboardShader;
    Shader m_shadowDepthShader;
    Shader m_postShader;

    Skybox m_skybox;
    Texture m_blockAtlas;
    Texture m_alertIcon;

    BillboardSystem m_billboards;
    UIRenderer m_ui;
    DebugOverlay m_debugOverlay;

    Framebuffer m_sceneFbo;
    PostProcess m_post;
    PostProcessSettings m_postSettings;
    ShadowMap m_shadows;

    std::shared_ptr<Mesh> m_cubeMesh; // мусор, поплавок
    int m_biteIconId = -1;            // билборд-иконка "клюёт!" над поплавком

    // --- Состояние партии + производные за кадр ---
    GameState m_state;
    UICanvas m_statsHud;
    PlayerActions::LookContext m_look; // считается в OnUpdate, используется в OnRender

    // --- Тумблеры рендер-пайплайна (env-флаги для сравнения "до/после") ---
    bool m_postEnabled = true;
    bool m_shadowsEnabled = true;
};
