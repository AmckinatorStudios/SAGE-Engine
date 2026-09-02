#include "sage/render/ScenePasses.h"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

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

    sage::anim::DrawAnimatedModels(scene, input.View, input.Proj, input.ViewPos, *input.Env,
                                   input.Shadows, &input.Reflection);
    return stats;
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
