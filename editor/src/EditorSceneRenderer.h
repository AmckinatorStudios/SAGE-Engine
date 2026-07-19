#pragma once
#include <optional>

#include <glm/glm.hpp>

#include "sage/render/Shader.h"
#include "sage/render/Camera.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/PostFX.h"
#include "sage/render/DebugDraw.h"
#include "sage/render/ShadowMap.h"
#include "sage/render/SkyRenderer.h"
#include "sage/render/ParticleSystem.h"
#include "sage/ui/UIRenderer.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Light.h"
#include "sage/core/Config.h"
#include "sage/ecs/RenderBatch.h"

#include "EditorHost.h" // EditorRenderMode

// ---------------------------------------------------------------------------
// EditorSceneRenderer — весь превью-рендер редактора, вынесенный из EditorLayer
// (разгрузка god-object): владеет рендер-ресурсами (карта теней, offscreen-FBO
// сцены/Game, PostFX, батч статики, скайбокс, частицы, DebugDraw, outline-шейдер)
// и рисует два кадра — Viewport (редакторская камера + гизмо/аутлайн/сетка) и
// Game (Primary-камера сцены, без гизмо) — с общим shadow-проходом и полной
// пост-обработкой. EditorLayer только оркестрирует: собирает освещение, зовёт
// RenderShadow → RenderViewport → RenderGame и показывает их текстуры в панелях.
// ---------------------------------------------------------------------------
class EditorSceneRenderer {
public:
    void Init();

    void SetViewportSize(int w, int h) { m_vpW = w; m_vpH = h; }
    void SetGameSize(int w, int h) { m_gameW = w; m_gameH = h; }

    // Общий depth-проход солнца (одна карта теней на кадр для обоих окон).
    void RenderShadow(Scene& scene, const LightingEnvironment& env);

    // Превью сцены редакторской камерой. Возвращает использованные view/proj
    // (нужны вызывающему для гизмо/пикинга). mode/showGrid — из тулбара.
    void RenderViewport(Scene& scene, Camera& camera, const LightingEnvironment& env,
                        int selectedId, EditorRenderMode mode, bool showGrid,
                        const sage::EngineConfig& cfg, glm::mat4& outView, glm::mat4& outProj);

    // Игровое окно от первой Primary-камеры сцены (нет камеры — кадр не рисуется,
    // GameApplied() остаётся false). Всегда Shaded + пост-обработка.
    void RenderGame(Scene& scene, const LightingEnvironment& env, const sage::EngineConfig& cfg);

    // Текстуры для ImGui-панелей: после PostFX — LDR-выход, иначе HDR-цвет FBO.
    unsigned int ViewportTexture() const;
    unsigned int GameTexture() const;

    const sage::ecs::RenderStats& LastStats() const { return m_lastStats; }
    ParticleSystem& Particles() { return *m_particles; } // UpdateEmitters + self-test

    // Для self-test редактора: прогнать инстансный батч с заданной камерой.
    sage::ecs::RenderStats RenderColorForTest(Scene& scene, const glm::mat4& view,
                                              const glm::mat4& proj, const glm::vec3& viewPos,
                                              const LightingEnvironment& env);

private:
    void DrawLit(Scene& scene, const LightingEnvironment& env, const glm::mat4& view,
                 const glm::mat4& proj, glm::vec3 viewPos, int shadingMode, bool wireframe);
    void DrawSelectionOutline(Scene& scene, GameObject obj, const glm::mat4& view, const glm::mat4& proj);
    void DrawEntityGizmos(Scene& scene, int selectedId, float gameAspect);
    static sage::render::PostFXSettings FxFromConfig(const sage::EngineConfig& cfg);

    std::optional<Shader> m_outlineShader;   // lit-шейдер как flat-цвет каймы выделения
    std::optional<ShadowMap> m_shadows;
    std::optional<Framebuffer> m_sceneFbo, m_gameFbo;
    std::optional<Framebuffer> m_postFbo, m_gamePostFbo; // LDR-выходы PostFX
    // UI сцены (UIElementComponent) — оверлей в панели Game (WYSIWYG: как в
    // собранной игре). Лениво: создаётся при первом кадре с UI-сущностями.
    std::unique_ptr<UIRenderer> m_ui;
    std::optional<sage::render::PostFX> m_postfx, m_gamePostfx;
    bool m_postApplied = false, m_gamePostApplied = false;
    std::optional<DebugDraw> m_debugDraw;
    std::optional<SkyRenderer> m_sky;
    std::optional<ParticleSystem> m_particles;
    sage::ecs::RenderBatch m_batch;
    sage::ecs::RenderStats m_lastStats;

    int m_vpW = 1280, m_vpH = 720;
    int m_gameW = 1280, m_gameH = 720;
};
