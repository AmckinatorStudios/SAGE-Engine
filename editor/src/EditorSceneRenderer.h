#pragma once
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include "sage/render/Shader.h"
#include "sage/render/Camera.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/PostFX.h"
#include "sage/render/DebugDraw.h"
#include "sage/render/ShadowMap.h"
#include "sage/render/SkyRenderer.h"
#include "sage/render/Reflection.h"
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
    // Время сцены для собственных шейдеров материалов (uTime). Идёт и в режиме
    // правки, а не только в Play: анимированный материал должен шевелиться во
    // вьюпорте — иначе его не настроить, не запуская игру.
    void Tick(float dt) { m_sceneTime += dt; }

    void Init();

    void SetViewportSize(int w, int h) { m_vpW = w; m_vpH = h; }
    void SetGameSize(int w, int h) { m_gameW = w; m_gameH = h; }

    // Общий depth-проход солнца (одна карта теней на кадр для обоих окон).
    // camera — камера ВЬЮПОРТА: карта теней строится вокруг неё, а не вокруг
    // начала мира, иначе всё, что редактор отлистал в сторону, теней лишается.
    // Готовит отражения кадра (пересъёмка карты окружения при смене неба).
    // Зовётся ДО любых проходов, рисующих во вьюпорт.
    void PrepareReflections(Scene& scene, const LightingEnvironment& env);
    void RenderShadow(Scene& scene, const LightingEnvironment& env, const Camera& camera);

    // Превью сцены редакторской камерой. Возвращает использованные view/proj
    // (нужны вызывающему для гизмо/пикинга). mode/showGrid — из тулбара.
    void RenderViewport(Scene& scene, Camera& camera, const LightingEnvironment& env,
                        int selectedId, const std::vector<int>& selection,
                        EditorRenderMode mode, bool showGrid,
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
    float m_sceneTime = 0.0f;


    // Небо кадра (кубическая текстура или процедурный градиент).
    void DrawSky(const LightingEnvironment& env, const glm::mat4& view, const glm::mat4& proj);
    void DrawLit(Scene& scene, const LightingEnvironment& env, const glm::mat4& view,
                 const glm::mat4& proj, glm::vec3 viewPos, int shadingMode, bool wireframe);
    // Аутлайн выделения — робастный пост-проход: силуэт объекта в масочный
    // буфер, затем краевая дилатация ПОСТОЯННОЙ ширины в пикселях поверх кадра.
    // Работает для любых мешей (модели/плоскости/невыпуклые), в отличие от
    // прежней «раздутой оболочки» (толщина зависела от размера, только выпуклые).
    // Силуэты ВСЕХ выбранных сущностей в масочный буфер (одна очистка, потом все).
    void RenderOutlineMask(Scene& scene, const std::vector<int>& selection,
                           const glm::mat4& view, const glm::mat4& proj);
    void CompositeOutline(Framebuffer& target);
    void DrawEntityGizmos(Scene& scene, const std::vector<int>& selection, float gameAspect);
    static sage::render::PostFXSettings FxFromConfig(const sage::EngineConfig& cfg);

    std::optional<Shader> m_outlineShader;   // lit-шейдер как flat-цвет каймы выделения
    std::optional<ShadowMap> m_shadows;
    std::optional<Framebuffer> m_sceneFbo, m_gameFbo;
    std::optional<Framebuffer> m_postFbo, m_gamePostFbo; // LDR-выходы PostFX
    std::optional<Framebuffer> m_outlineMask;            // силуэт выделенного объекта (аутлайн)
    std::unique_ptr<sage::rhi::Geometry> m_outlineTri;   // полноэкранный треугольник для краевого прохода
    // UI сцены (UIElementComponent) — оверлей в панели Game (WYSIWYG: как в
    // собранной игре). Лениво: создаётся при первом кадре с UI-сущностями.
    std::unique_ptr<UIRenderer> m_ui;
    std::optional<sage::render::PostFX> m_postfx, m_gamePostfx;
    bool m_postApplied = false, m_gamePostApplied = false;
    std::optional<DebugDraw> m_debugDraw;
    std::optional<SkyRenderer> m_sky;
    // Отражения вьюпорта. Свои, а не общие с рантаймом: карта окружения
    // снимается из точки и принадлежит виду.
    sage::render::ReflectionSystem m_reflections;
    std::optional<ParticleSystem> m_particles;
    sage::ecs::RenderBatch m_batch;
    sage::ecs::RenderStats m_lastStats;

    int m_vpW = 1280, m_vpH = 720;
    int m_gameW = 1280, m_gameH = 720;
};
