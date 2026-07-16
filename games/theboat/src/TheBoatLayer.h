#pragma once
#include <optional>
#include <memory>
#include <string>

#include "sage/core/Layer.h"
#include "sage/core/InputSystem.h"
#include "sage/render/Shader.h"
#include "sage/render/Camera.h"
#include "sage/render/Texture.h"
#include "sage/render/Skybox.h"
#include "sage/render/BillboardSystem.h"
#include "sage/render/DebugOverlay.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/ShadowMap.h"
#include "sage/render/PostProcess.h"
#include "sage/render/Mesh.h"
#include "sage/ui/UIRenderer.h"
#include "sage/ui/UICanvas.h"
#include "game/GameState.h"

// ---------------------------------------------------------------------------
// TheBoatLayer — вся игра «The Boat» как один слой поверх движка. Раньше это
// был монолитный main() с ручным циклом; теперь цикл/окно/тайминг принадлежат
// sage::Application, а игровая логика и проходы отрисовки живут здесь:
//   OnAttach  — создание ресурсов рендера, мира, привязка ввода/скриптов.
//   OnUpdate  — ввод -> движение/физика -> симуляция -> действия игрока.
//   OnRender  — тени -> сцена (HDR) -> пост-процесс -> UI/HUD.
//   OnDetach  — очистка ResourceManager, пока GL-контекст ещё жив.
// Поведение идентично прежнему main() — движок лишь сменил «обёртку» цикла.
// ---------------------------------------------------------------------------
class TheBoatLayer : public sage::Layer {
public:
    TheBoatLayer() : sage::Layer("TheBoat") {}

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(float dt) override;
    void OnRender() override;

private:
    void DrawShadowCasters(Shader& depthShader);

    // ---- Ввод ----
    InputSystem m_input;

    // ---- Шейдеры (создаются в OnAttach, когда GL-контекст готов) ----
    std::optional<Shader> m_voxelShader, m_waterShader, m_basicShader, m_skyboxShader;
    std::optional<Shader> m_particleShader, m_billboardShader, m_shadowDepthShader, m_postShader;

    // ---- Пост-процессинг и тени ----
    std::optional<Framebuffer> m_sceneFbo;
    PostProcess m_post;
    PostProcessSettings m_postSettings;
    std::optional<ShadowMap> m_shadows;
    bool m_postEnabled = true;
    bool m_shadowsEnabled = true;

    // ---- Скайбокс, текстуры, билборды ----
    std::optional<Skybox> m_skybox;
    std::optional<Texture> m_blockAtlas;
    std::optional<Texture> m_alertIcon;
    BillboardSystem m_billboards;
    int m_biteIconId = -1;

    // ---- UI / отладка ----
    UIRenderer m_ui;
    DebugOverlay m_debugOverlay;
    UICanvas m_statsHud;

    std::shared_ptr<Mesh> m_cubeMesh;
    Camera m_camera;

    // ---- Игровое состояние (мир, корабль, игрок, инвентарь, системы) ----
    GameState m_game;

    // ---- Скриншоты (ручной F2 и авто для CI) ----
    std::string m_screenshotPath = "screenshot.png";
    int m_autoScreenshotFrame = -1;
    int m_frameCounter = 0;
};
