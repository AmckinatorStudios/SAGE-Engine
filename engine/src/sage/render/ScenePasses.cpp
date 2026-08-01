#include "sage/render/ScenePasses.h"

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

sage::ecs::RenderStats RenderSceneColor(Scene& scene, sage::ecs::RenderBatch& batch,
                                        const SceneColorInput& input) {
    SAGE_PROFILE("Геометрия");
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();

    if (input.Wireframe) device.SetPolygonMode(sage::rhi::PolygonMode::Line);
    batch.SetTime(input.Time);
    const sage::ecs::RenderStats stats =
        batch.RenderColor(scene, input.View, input.Proj, input.ViewPos, *input.Env, input.Shadows,
                          input.ShadingMode);
    if (input.Wireframe) device.SetPolygonMode(sage::rhi::PolygonMode::Fill);

    // Проверка перекрытия — СРАЗУ после статики: буфер глубины кадра уже
    // заполнен, а служебные коробки не пишут ни цвет, ни глубину и потому не
    // мешают тому, что рисуется дальше. Результат заберётся следующим кадром.
    batch.SetOcclusionCulling(input.OcclusionCulling);
    if (input.OcclusionCulling) {
        batch.RenderOcclusionProbes(input.Proj * input.View, input.ViewPos);
    }

    sage::anim::DrawAnimatedModels(scene, input.View, input.Proj, input.ViewPos, *input.Env,
                                   input.Shadows);
    return stats;
}

} // namespace sage::render
