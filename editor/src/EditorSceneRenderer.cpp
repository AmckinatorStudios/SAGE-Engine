#include "EditorSceneRenderer.h"

#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/core/Application.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/render/ParticleECS.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Components.h"

void EditorSceneRenderer::Init() {
    m_outlineShader.emplace("assets/shaders/lit.vert", "assets/shaders/lit.frag");
    m_shadows.emplace(2048);
    m_sceneFbo.emplace(m_vpW, m_vpH);
    m_gameFbo.emplace(m_gameW, m_gameH);
    m_postFbo.emplace(m_vpW, m_vpH);
    m_postfx.emplace();
    m_gamePostFbo.emplace(m_gameW, m_gameH);
    m_gamePostfx.emplace();
    m_debugDraw.emplace();
    m_sky.emplace();
    m_particles.emplace();
}

sage::render::PostFXSettings EditorSceneRenderer::FxFromConfig(const sage::EngineConfig& cfg) {
    sage::render::PostFXSettings fx;
    fx.Exposure = cfg.Exposure; fx.Gamma = cfg.Gamma;
    fx.Saturation = cfg.Saturation; fx.Contrast = cfg.Contrast;
    fx.Vignette = cfg.Vignette;
    fx.BloomEnabled = cfg.Bloom; fx.BloomThreshold = cfg.BloomThreshold;
    fx.BloomIntensity = cfg.BloomIntensity;
    fx.AOEnabled = cfg.AmbientOcclusion; fx.AOStrength = cfg.AOStrength;
    fx.AORadius = cfg.AORadius;
    return fx;
}

void EditorSceneRenderer::RenderShadow(Scene& scene, const LightingEnvironment& env) {
    Window& window = sage::Application::Get().GetWindow();
    m_shadows->SetLightMatrix(env.Sun.Direction, glm::vec3(0.0f), 24.0f);
    m_shadows->BeginRender();
    // Статика в карту теней — батчем (инстансно + отсечение по фрустуму света).
    m_batch.RenderDepth(scene, m_shadows->LightMatrix());
    // Скелетные модели тоже отбрасывают тень (свой depth-шейдер, текущая поза).
    sage::anim::DrawAnimatedModelsDepth(scene, m_shadows->LightMatrix());
    m_shadows->EndRender(window.Width(), window.Height());
}

void EditorSceneRenderer::DrawLit(Scene& scene, const LightingEnvironment& env, const glm::mat4& view,
                                  const glm::mat4& proj, glm::vec3 viewPos, int shadingMode, bool wireframe) {
    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();
    if (wireframe) device.SetPolygonMode(sage::rhi::PolygonMode::Line);
    // Статика — RenderBatch (отсечение по фрустуму + инстансный батчинг).
    m_lastStats = m_batch.RenderColor(scene, view, proj, viewPos, env,
                                      m_shadows->LightMatrix(), m_shadows->DepthTexture(),
                                      /*shadowsEnabled=*/true, shadingMode);
    if (wireframe) device.SetPolygonMode(sage::rhi::PolygonMode::Fill);
}

// Аутлайн выбранного меша: масштабированная «оболочка» плоским цветом с отсечением
// ЛИЦЕВЫХ граней (видны только задние — образуют кайму). Работает для выпуклых.
void EditorSceneRenderer::DrawSelectionOutline(Scene& scene, GameObject obj,
                                               const glm::mat4& view, const glm::mat4& proj) {
    const MeshRendererComponent* mr = scene.Registry().try_get<MeshRendererComponent>(obj.Entity());
    if (!mr || !mr->MeshPtr) return;
    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();

    glm::mat4 model = glm::scale(scene.WorldMatrix(obj.Entity()), glm::vec3(1.06f));
    m_outlineShader->Use();
    m_outlineShader->SetMat4("uView", view);
    m_outlineShader->SetMat4("uProjection", proj);
    m_outlineShader->SetMat4("uModel", model);
    m_outlineShader->SetInt("uShadingMode", 1); // unlit — плоский цвет каймы
    m_outlineShader->SetInt("uUseTexture", 0);
    m_outlineShader->SetInt("uFogEnabled", 0);
    m_outlineShader->SetVec3("uObjectColor", {1.0f, 0.62f, 0.12f});

    device.SetCullMode(sage::rhi::CullMode::Front); // только задние грани оболочки
    mr->MeshPtr->Draw();
    device.SetCullMode(sage::rhi::CullMode::Back);
}

// Гизмо невидимых/физических сущностей (камера/свет/эмиттер/коллайдер) — всегда
// видны и кликабельны. Накопление в DebugDraw, Flush — у вызывающего.
void EditorSceneRenderer::DrawEntityGizmos(Scene& scene, int selectedId, float gameAspect) {
    auto& reg = scene.Registry();
    // Камеры: каркас усечённой пирамиды (frustum).
    auto camView = reg.view<CameraComponent, Transform, IdComponent>();
    for (auto e : camView) {
        bool selected = camView.get<IdComponent>(e).Id == selectedId;
        glm::vec3 color = selected ? glm::vec3(1.0f, 0.8f, 0.2f) : glm::vec3(0.5f, 0.7f, 0.9f);
        glm::mat4 world = scene.WorldMatrix(e); // мировая (иерархия)
        glm::vec3 wpos = glm::vec3(world[3]);
        glm::vec3 fwd = glm::normalize(glm::vec3(world * glm::vec4(0, 0, -1, 0)));
        const CameraComponent& cam = camView.get<CameraComponent>(e);
        m_debugDraw->WireFrustum(wpos, fwd, cam.Fov, gameAspect, 0.3f, 2.2f, color);
    }
    // Свет-сущности: маркер-сфера в позиции (у выбранного зона рисуется крупнее ниже).
    auto lightView = reg.view<LightComponent, Transform, IdComponent>();
    for (auto e : lightView) {
        if (lightView.get<IdComponent>(e).Id == selectedId) continue;
        const LightComponent& lc = lightView.get<LightComponent>(e);
        glm::vec3 wpos = glm::vec3(scene.WorldMatrix(e)[3]);
        m_debugDraw->WireSphere(wpos, 0.25f, glm::vec3(lc.Color) * 0.9f, 10);
    }
    // Эмиттеры частиц: маркер в позиции.
    auto fxView = reg.view<ParticleEmitterComponent, Transform, IdComponent>();
    for (auto e : fxView) {
        bool selected = fxView.get<IdComponent>(e).Id == selectedId;
        glm::vec3 color = selected ? glm::vec3(1.0f, 0.85f, 0.3f) : glm::vec3(0.9f, 0.6f, 0.3f);
        glm::vec3 wpos = glm::vec3(scene.WorldMatrix(e)[3]);
        m_debugDraw->WireSphere(wpos, 0.18f, color, 8);
        m_debugDraw->Axes(glm::translate(glm::mat4(1.0f), wpos), 0.4f);
    }
    // Коллайдеры: каркас формы в масштабе Transform.
    auto colView = reg.view<ColliderComponent, Transform, IdComponent>();
    for (auto e : colView) {
        const Transform& tr = colView.get<Transform>(e);
        const ColliderComponent& col = colView.get<ColliderComponent>(e);
        bool selected = colView.get<IdComponent>(e).Id == selectedId;
        glm::vec3 color = selected ? glm::vec3(0.3f, 1.0f, 0.4f) : glm::vec3(0.25f, 0.55f, 0.30f);
        glm::vec3 scale = glm::abs(tr.Scale);
        switch (col.Shape) {
            case sage::physics::ShapeType::Box:
                m_debugDraw->WireBox(tr.Position, col.HalfExtents * scale, color);
                break;
            case sage::physics::ShapeType::Sphere:
                m_debugDraw->WireSphere(tr.Position,
                    col.Radius * glm::max(scale.x, glm::max(scale.y, scale.z)), color);
                break;
            case sage::physics::ShapeType::Capsule:
                m_debugDraw->WireBox(tr.Position,
                    glm::vec3(col.Radius * scale.x, (col.HalfHeight + col.Radius) * scale.y,
                              col.Radius * scale.z), color);
                break;
        }
    }
}

void EditorSceneRenderer::RenderViewport(Scene& scene, Camera& camera, const LightingEnvironment& env,
                                         int selectedId, EditorRenderMode mode, bool showGrid,
                                         const sage::EngineConfig& cfg, glm::mat4& outView, glm::mat4& outProj) {
    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();

    m_sceneFbo->Resize(m_vpW, m_vpH);
    m_sceneFbo->Bind();
    device.SetClearColor(0.10f, 0.11f, 0.13f, 1.0f);
    device.Clear();

    outView = camera.GetViewMatrix();
    outProj = camera.GetProjectionMatrix((float)m_vpW / (float)std::max(m_vpH, 1));

    if (env.Skybox.Enabled)
        m_sky->Draw(outView, outProj, env.Skybox.TopColor, env.Skybox.HorizonColor);

    // Режим рендера из тулбара: Shaded(0)/Unlit(1)/Normals(2); Wireframe — unlit + линии.
    int shadingMode = 0;
    bool wireframe = false;
    switch (mode) {
        case EditorRenderMode::Shaded:    shadingMode = 0; break;
        case EditorRenderMode::Wireframe: shadingMode = 1; wireframe = true; break;
        case EditorRenderMode::Unlit:     shadingMode = 1; break;
        case EditorRenderMode::Normals:   shadingMode = 2; break;
    }
    DrawLit(scene, env, outView, outProj, camera.Position, shadingMode, wireframe);

    sage::anim::DrawAnimatedModels(scene, outView, outProj, camera.Position, env,
                                   m_shadows->LightMatrix(), m_shadows->DepthTexture(), true);
    m_particles->Draw(camera, outView, outProj);

    GameObject selectedObj = scene.Get(selectedId);
    if (selectedObj.Valid()) DrawSelectionOutline(scene, selectedObj, outView, outProj);

    // Гизмо-графика (DebugDraw) — в тот же буфер, с тестом глубины (объекты заслоняют сетку).
    if (showGrid) m_debugDraw->Grid({0.0f, 0.0f, 0.0f}, 12.0f, 1.0f, {0.32f, 0.33f, 0.38f});
    DrawEntityGizmos(scene, selectedId, (float)m_gameW / (float)std::max(m_gameH, 1));
    if (selectedObj.Valid()) {
        glm::mat4 world = scene.WorldMatrix(selectedObj.Entity());
        m_debugDraw->Axes(world, 1.4f);
        if (const LightComponent* light = scene.Registry().try_get<LightComponent>(selectedObj.Entity())) {
            glm::vec3 wpos = glm::vec3(world[3]);
            glm::vec3 lightColor = glm::vec3(light->Color) * 0.9f;
            if (light->Kind == LightComponent::Type::Spot) {
                glm::vec3 dir = glm::normalize(glm::vec3(world * glm::vec4(0, 0, -1, 0)));
                m_debugDraw->WireCone(wpos, dir, light->Range, light->OuterConeDeg, lightColor);
            } else {
                m_debugDraw->WireSphere(wpos, light->Range, lightColor);
            }
        }
    }
    m_debugDraw->Flush(outView, outProj);

    // Пост-обработка — только Shaded + включена в конфиге (отладочные режимы как есть).
    m_postApplied = false;
    if (cfg.PostProcessing && mode == EditorRenderMode::Shaded) {
        m_postFbo->Resize(m_vpW, m_vpH);
        m_postfx->Render(m_sceneFbo->ColorTexture(), m_sceneFbo->DepthTexture(),
                         m_sceneFbo->Width(), m_sceneFbo->Height(), outProj, FxFromConfig(cfg),
                         /*output=*/&*m_postFbo, 0, 0, m_vpW, m_vpH);
        m_postApplied = true;
    }
    device.BindDefaultFramebuffer();
}

void EditorSceneRenderer::RenderGame(Scene& scene, const LightingEnvironment& env, const sage::EngineConfig& cfg) {
    // Первая Primary-камера сцены; нет — кадр не рисуем (панель Game покажет подсказку).
    entt::entity camEntity = entt::null;
    auto camView = scene.Registry().view<CameraComponent, Transform>();
    for (auto e : camView)
        if (camView.get<CameraComponent>(e).Primary) { camEntity = e; break; }
    if (camEntity == entt::null) { m_gamePostApplied = false; return; }

    const CameraComponent& cam = camView.get<CameraComponent>(camEntity);
    glm::mat4 camWorld = scene.WorldMatrix(camEntity); // учёт иерархии родителей
    glm::vec3 camPos = glm::vec3(camWorld[3]);
    glm::vec3 fwd = glm::normalize(glm::vec3(camWorld * glm::vec4(0, 0, -1, 0)));
    glm::vec3 up = glm::normalize(glm::vec3(camWorld * glm::vec4(0, 1, 0, 0)));
    glm::mat4 view = glm::lookAt(camPos, camPos + fwd, up);
    float aspect = (float)m_gameW / (float)std::max(m_gameH, 1);
    glm::mat4 proj = glm::perspective(glm::radians(cam.Fov), aspect, cam.NearClip, cam.FarClip);

    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();
    m_gameFbo->Resize(m_gameW, m_gameH);
    m_gameFbo->Bind();
    device.SetClearColor(env.SkyColor.r * 0.9f, env.SkyColor.g * 0.9f, env.SkyColor.b * 0.9f, 1.0f);
    device.Clear();

    if (env.Skybox.Enabled)
        m_sky->Draw(view, proj, env.Skybox.TopColor, env.Skybox.HorizonColor);
    // Игровое окно — всегда Shaded, без гизмо (как увидит игрок).
    DrawLit(scene, env, view, proj, camPos, /*shadingMode=*/0, /*wireframe=*/false);
    sage::anim::DrawAnimatedModels(scene, view, proj, camPos, env,
                                   m_shadows->LightMatrix(), m_shadows->DepthTexture(), true);
    m_particles->DrawFromView(view, proj);

    m_gamePostApplied = false;
    if (cfg.PostProcessing) {
        m_gamePostFbo->Resize(m_gameW, m_gameH);
        m_gamePostfx->Render(m_gameFbo->ColorTexture(), m_gameFbo->DepthTexture(),
                             m_gameFbo->Width(), m_gameFbo->Height(), proj, FxFromConfig(cfg),
                             /*output=*/&*m_gamePostFbo, 0, 0, m_gameW, m_gameH);
        m_gamePostApplied = true;
    }
    device.BindDefaultFramebuffer();
}

unsigned int EditorSceneRenderer::ViewportTexture() const {
    return m_postApplied && m_postFbo ? m_postFbo->ColorTexture() : m_sceneFbo->ColorTexture();
}
unsigned int EditorSceneRenderer::GameTexture() const {
    return m_gamePostApplied && m_gamePostFbo ? m_gamePostFbo->ColorTexture() : m_gameFbo->ColorTexture();
}

sage::ecs::RenderStats EditorSceneRenderer::RenderColorForTest(Scene& scene, const glm::mat4& view,
                                                              const glm::mat4& proj, const glm::vec3& viewPos,
                                                              const LightingEnvironment& env) {
    return m_batch.RenderColor(scene, view, proj, viewPos, env,
                               m_shadows->LightMatrix(), m_shadows->DepthTexture(), true, 0);
}
