#pragma once
#include <memory>
#include <optional>
#include <string>

#include "sage/core/Layer.h"
#include "sage/input/GlfwBridge.h"
#include "sage/input/InputSystem.h"
#include "sage/render/RenderPipeline.h"

#include "Player.h"
#include "VoxelPipeline.h"
#include "World.h"

class Texture;
class UIRenderer;

// ---------------------------------------------------------------------------
// VoxelLayer — игра: мир, игрок, ломать и ставить блоки, смена дня и ночи,
// интерфейс. Рисует всё свой рендерер (VoxelPipeline) через исполнитель
// движка; интерфейс — расширение графа (проход «Интерфейс» после поста), так
// дополняют чужой рендерер, не трогая его.
//
// Самопроверка (SAGE_SCREENSHOT_AT_FRAME): свет после правок, сборка
// шейдеров, граф кадра — в лог строкой «VOXEL: SELFTEST PASS/FAIL».
// ---------------------------------------------------------------------------
class VoxelLayer : public sage::Layer {
public:
    VoxelLayer();
    ~VoxelLayer() override;

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(float dt) override;
    void OnRender() override;

private:
    void DrawHud();
    void SelfTest();

    std::unique_ptr<voxel::World> m_world;
    voxel::VoxelPipeline* m_pipeline = nullptr; // владеет m_renderer
    std::unique_ptr<sage::render::PipelineRenderer> m_renderer;
    voxel::Player m_player;
    voxel::RayHit m_target;

    sage::input::InputSystem m_input;
    sage::input::GlfwBridge m_bridge;
    std::unique_ptr<UIRenderer> m_ui;
    std::unique_ptr<Texture> m_atlasUi;

    int m_slot = 0;
    float m_time = 0.0f;
    float m_timeOfDay = 0.22f;
    float m_fps = 0.0f;
    float m_reloadTimer = 0.0f;
    bool m_debug = false;
    bool m_captured = false;
    bool m_autopilot = false;

    std::string m_screenshotPath = "screenshot.png";
    int m_autoScreenshotFrame = -1;
    long long m_frame = 0;
};
