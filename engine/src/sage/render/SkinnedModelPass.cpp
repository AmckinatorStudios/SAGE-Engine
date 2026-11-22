#include "sage/render/SkinnedModelPass.h"

#include "sage/anim/AnimationComponents.h"
#include "sage/core/Profiler.h"
#include "sage/ecs/RenderComponents.h"
#include "sage/render/Frustum.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace sage::render {

// Материал сцены -> материал модели со скелетом: те же поля, что понимает
// шейдер скиннинга (альбедо с картой, вырез, две стороны, без освещения).
SkinnedMaterial SkinnedMaterialFromScene(const Material& mat) {
    SkinnedMaterial m;
    m.Name = "override";
    m.Tint = mat.Albedo;
    m.Opacity = mat.Opacity;
    m.Metallic = mat.Metallic;
    m.Roughness = mat.Roughness;
    m.Emissive = mat.Emissive * mat.EmissiveStrength;
    m.Unlit = mat.Render.Unlit;
    m.DoubleSided = mat.Render.Cull == CullFaces::None;
    if (mat.Render.AlphaCutoff > 0.0f) {
        m.Mode = SkinnedMaterial::Alpha::Mask;
        m.AlphaCutoff = mat.Render.AlphaCutoff;
    } else if (mat.Opacity < 0.999f) {
        m.Mode = SkinnedMaterial::Alpha::Blend;
    }
    if (mat.AlbedoTex) m.Albedo = mat.AlbedoTex;
    else if (!mat.TexturePath.empty()) m.Albedo = ResourceManager::Instance().GetTexture(mat.TexturePath);
    return m;
}

void DrawAnimatedModels(Scene& scene, const glm::mat4& view, const glm::mat4& proj,
                        const glm::vec3& viewPos, const LightingEnvironment& env,
                        const ShadowBinding& shadows,
                        const ReflectionBinding* reflections, int shadingMode,
                        const LightClusters* clusters, const sage::ecs::LayerFilter* layers) {
    SAGE_PROFILE("Скиннинг");
    sage::gi::GIState* gi = scene.GI ? scene.GI.get() : nullptr;
    auto v = scene.Registry().view<AnimationComponent, Transform>();
    for (auto e : v) {
        AnimationComponent& am = v.get<AnimationComponent>(e);
        if (!am.Model) continue;
        if (layers) {
            const auto* lmr = scene.Registry().try_get<MeshRendererComponent>(e);
            if (!layers->Accepts(lmr ? lmr->Layer : std::string())) continue;
        }
        // Материал, назначенный сущности (инспектор, SetMaterial из скрипта),
        // красит модель вместо материалов файла. Раньше он молча
        // игнорировался: персонажу нельзя было сменить скин или сделать его
        // неосвещённым, хотя у любого другого объекта это одна строка.
        SkinnedMaterial override;
        const SkinnedMaterial* overridePtr = nullptr;
        if (const auto* mr = scene.Registry().try_get<MeshRendererComponent>(e)) {
            if (mr->MaterialPtr) {
                override = SkinnedMaterialFromScene(*mr->MaterialPtr);
                overridePtr = &override;
            }
        }
        am.Model->Draw(scene.WorldMatrix(e), view, proj, viewPos, env, am.Anim.BoneMatrices(),
                       shadows, reflections, &am.MorphWeights, shadingMode, clusters, gi, overridePtr);
    }
}

void DrawAnimatedModelsDepth(Scene& scene, const glm::mat4& lightMatrix) {
    const Frustum frustum = Frustum::FromViewProj(lightMatrix);
    auto v = scene.Registry().view<AnimationComponent, Transform>();
    for (auto e : v) {
        AnimationComponent& am = v.get<AnimationComponent>(e);
        if (!am.Model) continue;
        // «Не отбрасывать тень» у персонажа задаётся тем же флагом Mesh, что и у
        // статики (руки от первого лица — почти всегда скелетная модель). Раньше
        // этот путь флаг не читал: рука, спрятанная из карты теней у статики,
        // у скелетной модели продолжала ложиться тенью на пол.
        if (const auto* mr = scene.Registry().try_get<MeshRendererComponent>(e)) {
            if (!mr->CastShadows) continue;
        }
        const glm::mat4 world = scene.WorldMatrix(e);
        // Отсечение по каскаду — по сфере ТЕКУЩЕЙ позы (см. PoseBoundingSphere).
        // Скиннинг — самая дорогая вершинная работа кадра, а без отсечения
        // каждый персонаж сцены рисовался в каждый каскад, даже за сотню метров
        // от него. Модель с активными морфами не отсекается: смещения
        // блендшейпов не входят в габарит, и строгой границы для них нет.
        bool morphed = false;
        for (float w : am.MorphWeights) morphed = morphed || w != 0.0f;
        glm::vec3 center;
        float radius = 0.0f;
        if (!morphed && am.Model->PoseBoundingSphere(world, am.Anim.BoneMatrices(), center, radius) &&
            !frustum.IntersectsSphere(center, radius)) {
            continue;
        }
        am.Model->DrawDepth(world, lightMatrix, am.Anim.BoneMatrices(), &am.MorphWeights);
    }
}

} // namespace sage::render
