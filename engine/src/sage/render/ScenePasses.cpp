#include "sage/render/ScenePasses.h"

#include "sage/render/Framebuffer.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/ecs/CameraView.h"
#include "sage/ecs/RenderComponents.h"
#include "sage/render/RenderTexture.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Scene.h"

#include "sage/anim/AnimationSystem.h"
#include "sage/core/Profiler.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Light.h"

namespace sage::render {

void RenderShadowDepth(ShadowMap& shadows, Scene& scene, sage::ecs::RenderBatch& batch,
                       int screenWidth, int screenHeight, const DepthDrawCallback& extra) {
    SAGE_PROFILE("Тени");
    shadows.SetSunAngle(scene.Lighting.Shadows.SunAngle);

    for (int c = 0; c < shadows.CascadeCount(); ++c) {
        shadows.BeginRender(c);
        const glm::mat4& light = shadows.LightMatrix(c);

        // Ручная геометрия ПЕРВОЙ: у неё свой шейдер, и переключать его туда-
        // обратно между каскадами дешевле, чем между видами геометрии.
        if (extra) extra(light);

        batch.RenderDepth(scene, light);                    // ECS-статика (инстансно + отсечение)
        sage::anim::DrawAnimatedModelsDepth(scene, light);  // скелеты тоже отбрасывают тень

        // Одна карта на всю сцену: остальные каскады — её копии, и рисовать в
        // них значило бы платить за N одинаковых карт.
        if (shadows.ActiveCascades() == 1) break;
    }
    shadows.EndRender(screenWidth, screenHeight);
}

void RenderLocalShadowDepth(LocalShadowAtlas& atlas, Scene& scene, sage::ecs::RenderBatch& batch,
                            int screenWidth, int screenHeight, const DepthDrawCallback& extra) {
    if (atlas.PassCount() <= 0) return;
    SAGE_PROFILE("Тени ламп");

    for (int p = 0; p < atlas.PassCount(); ++p) {
        atlas.BeginPass(p);
        const glm::mat4& light = atlas.PassMatrix(p);
        if (extra) extra(light);
        batch.RenderDepth(scene, light);
        sage::anim::DrawAnimatedModelsDepth(scene, light);
    }
    atlas.End(screenWidth, screenHeight);
}

CameraPassPlan PlanCameraPasses(const CameraComponent* camera) {
    CameraPassPlan plan;
    if (!camera || camera->Passes.empty()) return plan;
    int mainAt = -1;
    for (size_t i = 0; i < camera->Passes.size(); ++i) {
        const CameraPass& p = camera->Passes[i];
        bool isMain = false;
        for (const std::string& l : p.Layers)
            if (sage::ecs::IsDefaultLayer(l)) isMain = true;
        if (isMain && mainAt < 0) {
            mainAt = (int)i;
            plan.MainClearDepth = p.ClearDepth;
            plan.MainFov = p.Fov;
            plan.MainNear = p.Near;
            plan.MainFar = p.Far;
            // Основной рисует ещё и названные здесь слои, кроме "".
            continue;
        }
        for (const std::string& l : p.Layers)
            if (!sage::ecs::IsDefaultLayer(l)) plan.Main.Exclude.push_back(l);
    }
    // Слои, названные в самом основном проходе, у других не отнимаются.
    if (mainAt >= 0) {
        for (const std::string& l : camera->Passes[(size_t)mainAt].Layers) {
            auto& ex = plan.Main.Exclude;
            ex.erase(std::remove(ex.begin(), ex.end(), l), ex.end());
        }
    }
    for (size_t i = 0; i < camera->Passes.size(); ++i) {
        if ((int)i == mainAt) continue;
        if (mainAt >= 0 && (int)i < mainAt) plan.Before.push_back(camera->Passes[i]);
        else plan.After.push_back(camera->Passes[i]);
    }
    return plan;
}

glm::mat4 PassProjection(const CameraComponent& camera, float fov, float nearClip, float farClip, float aspect) {
    CameraComponent c = camera;
    if (fov > 0.0f) c.Fov = fov;
    if (nearClip > 0.0f) c.NearClip = nearClip;
    if (farClip > 0.0f) c.FarClip = farClip;
    return c.ProjectionMatrix(aspect);
}

void RenderCameraPasses(Scene& scene, sage::ecs::RenderBatch& batch, const SceneColorInput& base,
                        const std::vector<CameraPass>& passes, const CameraComponent& camera, float aspect) {
    if (passes.empty()) return;
    SAGE_PROFILE("Проходы камеры");
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    for (const CameraPass& p : passes) {
        if (p.ClearDepth) device.Clear(false, true);
        sage::ecs::LayerFilter filter;
        filter.Only = p.Layers;
        // Пустой список — проход ничего не рисует (а не «всё»): слой надо назвать.
        if (filter.Only.empty()) continue;
        SceneColorInput in = base;
        in.Proj = PassProjection(camera, p.Fov, p.Near, p.Far, aspect);
        in.Layers = &filter;
        in.OcclusionCulling = false;   // ответы о перекрытии — у основного прохода
        RenderSceneColor(scene, batch, in);
    }
}

void CameraPassFrame::Prepare(Scene& scene, float aspect) {
    Active = false;
    Aspect = aspect;
    const entt::entity e = sage::ecs::PrimaryCameraEntity(scene);
    if (e == entt::null) return;
    const CameraComponent* cam = scene.Registry().try_get<CameraComponent>(e);
    if (!cam || cam->Passes.empty()) return;
    Camera = *cam;
    Plan = PlanCameraPasses(cam);
    Active = true;
}

void CameraPassFrame::BeforeMain(Scene& scene, sage::ecs::RenderBatch& batch, const SceneColorInput& base) const {
    if (!Active) return;
    RenderCameraPasses(scene, batch, base, Plan.Before, Camera, Aspect);
    if (Plan.MainClearDepth && !Plan.Before.empty()) sage::rhi::GraphicsDevice::Get().Clear(false, true);
}

void CameraPassFrame::ApplyMain(SceneColorInput& input) const {
    if (!Active) return;
    input.Layers = &Plan.Main;
    if (Plan.MainFov > 0.0f || Plan.MainNear > 0.0f || Plan.MainFar > 0.0f)
        input.Proj = PassProjection(Camera, Plan.MainFov, Plan.MainNear, Plan.MainFar, Aspect);
}

void CameraPassFrame::AfterMain(Scene& scene, sage::ecs::RenderBatch& batch, const SceneColorInput& base) const {
    if (!Active) return;
    RenderCameraPasses(scene, batch, base, Plan.After, Camera, Aspect);
}

sage::ecs::RenderStats RenderSceneColor(Scene& scene, sage::ecs::RenderBatch& batch,
                                        const SceneColorInput& input) {
    SAGE_PROFILE("Геометрия");
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();

    if (input.Wireframe) device.SetPolygonMode(sage::rhi::PolygonMode::Line);
    batch.SetTime(input.Time);
    // Режим и НОМЕР ВИДА выставляются ДО сбора, а не после него: сбор сам
    // читает ответы о перекрытии, и до этой строки он читал их от прошлого
    // прохода — то есть от чужой камеры. Раньше вызов стоял ниже, и работало
    // это лишь потому, что вид в кадре был один.
    batch.SetOcclusionCulling(input.OcclusionCulling, input.ViewId);
    batch.SetLayerFilter(input.Layers);
    const sage::ecs::RenderStats stats =
        batch.RenderColor(scene, input.View, input.Proj, input.ViewPos, *input.Env, input.Shadows,
                          input.ShadingMode, &input.Reflection);
    if (input.Wireframe) device.SetPolygonMode(sage::rhi::PolygonMode::Fill);

    // Проверка перекрытия — СРАЗУ после статики: буфер глубины кадра уже
    // заполнен, а служебные коробки не пишут ни цвет, ни глубину и потому не
    // мешают тому, что рисуется дальше. Результат заберётся следующим кадром.
    if (input.OcclusionCulling) {
        batch.RenderOcclusionProbes(input.Proj * input.View, input.ViewPos);
    }

    // Режим отладочного вида — тот же, что у статики: персонаж обязан
    // участвовать в разборе кадра наравне со всем остальным.
    // Скелетные модели — тем же текстурным PBR-шейдером, что и статика, и
    // G-буфер пишут так же (см. RenderBatch::RenderColor).
    device.SetGBufferWrites(true);
    sage::anim::DrawAnimatedModels(scene, input.View, input.Proj, input.ViewPos, *input.Env,
                                   input.Shadows, &input.Reflection, input.ShadingMode,
                                   &batch.Clusters(), input.Layers);
    batch.SetLayerFilter(nullptr);
    device.SetGBufferWrites(false);
    return stats;
}

VelocityBuffer::VelocityBuffer() = default;
VelocityBuffer::~VelocityBuffer() = default;

sage::rhi::TextureHandle VelocityBuffer::Render(sage::ecs::RenderBatch& batch,
                                                const glm::mat4& view, const glm::mat4& proj,
                                                int width, int height) {
    SAGE_PROFILE("Скорости");
    if (width < 1 || height < 1) return {};
    if (!m_target) m_target = std::make_unique<Framebuffer>(width, height);
    else m_target->Resize(width, height);
    const glm::mat4 viewProj = proj * view;
    const glm::mat4 prev = m_hasPrev ? m_prevViewProj : viewProj;

    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    m_target->Bind();
    device.SetViewport(0, 0, width, height);
    // Синий = 1 — «здесь нет геометрии» (см. заголовок): проход скоростей
    // пишет в синий ноль.
    device.SetClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    device.Clear(true, true);
    device.SetBlend(false);
    device.SetDepthTest(true);
    device.SetDepthWrite(true);
    device.SetCullMode(sage::rhi::CullMode::Back);
    batch.RenderVelocity(viewProj, prev);

    m_prevViewProj = viewProj;
    m_hasPrev = true;
    return m_target->ColorTexture();
}

bool ChainNeedsVelocity(const PostChain& chain) {
    for (const PostEffect& e : chain.Effects)
        if (e.Enabled && e.Kind == "motionblur" && e.Float("amount", 0.5f) > 0.001f) return true;
    return false;
}

int RenderTextureViews(Scene& scene, sage::ecs::RenderBatch& batch,
                       const LightingEnvironment& env, float time) {
    auto view = scene.Registry().view<RenderTextureComponent, Transform>();
    if (view.begin() == view.end()) return 0;

    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    RenderTextureRegistry& registry = RenderTextureRegistry::Instance();
    int drawn = 0;

    for (auto e : view) {
        RenderTextureComponent& rt = view.get<RenderTextureComponent>(e);
        if (rt.Target.empty()) continue;
        if (!rt.Continuous && !rt.Dirty) continue;

        const glm::vec3 eye = glm::vec3(scene.WorldMatrix(e)[3]);
        // Взгляд строго вниз ломает look-at: «вверх» и направление совпадают, и
        // матрица вырождается. Подпираем вектор вверх наклоном.
        glm::vec3 dir = rt.LookAt - eye;
        if (glm::dot(dir, dir) < 1e-8f) dir = glm::vec3(0.0f, 0.0f, -1.0f);
        dir = glm::normalize(dir);
        const glm::vec3 up = std::abs(dir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                     : glm::vec3(0.0f, 1.0f, 0.0f);

        RenderTexture& target = registry.GetOrCreate(rt.Target, rt.Width, rt.Height);
        const float aspect = (float)target.Width() / (float)std::max(target.Height(), 1);

        // Освещение: сценическое или своё ровное (см. StudioLight).
        LightingEnvironment studio;
        if (rt.StudioLight) {
            studio.Sun.Direction = glm::normalize(rt.LightDir);
            studio.Sun.Color = glm::vec3(1.0f);
            studio.Sun.Intensity = rt.LightIntensity;
            studio.AmbientStrength = rt.Ambient;
            studio.SkyColor = glm::vec3(0.72f, 0.78f, 0.88f);
            studio.GroundColor = glm::vec3(0.34f, 0.32f, 0.30f);
            studio.Fog.Enabled = false;
            // Свет студии задан ЗДЕСЬ и небу не подчиняется — значит режим
            // «Свои значения» (см. LightingEnvironment::AmbientMode). Без
            // этого выключенное ниже небо забрало бы с собой и ambient, и
            // съёмка в текстуру приезжала бы чёрной.
            studio.AmbientMode = LightingEnvironment::AmbientSource::Custom;
            studio.Skybox.Enabled = false;
            studio.Shadows.Distance = 0.0f;
        }

        SceneColorInput input;
        input.View = glm::lookAt(eye, eye + dir, up);
        input.Proj = rt.Ortho ? glm::ortho(-rt.OrthoSize * aspect, rt.OrthoSize * aspect,
                                           -rt.OrthoSize, rt.OrthoSize, rt.Near, rt.Far)
                              : glm::perspective(glm::radians(rt.Fov), aspect, rt.Near, rt.Far);
        input.ViewPos = eye;
        input.Env = rt.StudioLight ? &studio : &env;
        input.Time = time;
        // Ни теней, ни отражений: у съёмки своя камера, и каскады, посчитанные
        // под камеру игрока, лягут мимо. Отдельный проход теней на каждую
        // картинку стоил бы дороже самой картинки.

        target.Begin(rt.ClearColor);
        device.SetDepthTest(true);
        RenderSceneColor(scene, batch, input);
        target.End();

        rt.Dirty = false;
        ++drawn;
    }
    return drawn;
}

} // namespace sage::render
