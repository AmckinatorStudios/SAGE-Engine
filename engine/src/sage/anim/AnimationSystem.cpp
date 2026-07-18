#include "sage/anim/AnimationSystem.h"

#include <exception>

#include "sage/core/Log.h"
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace sage::anim {

// Ленивая инициализация одной анимированной модели: грузит ассет (или строит
// процедурный демо-щупалец при пустом Path), привязывает Animator к скелету и
// запускает клип. Ошибка загрузки помечает компонент готовым без модели —
// сущность просто не рисуется (и не пытается грузиться каждый кадр).
static void EnsureReady(AnimatedModelComponent& am) {
    if (am.Ready) return;
    am.Ready = true;
    try {
        if (am.Path.empty()) {
            am.Model = sage::render::SkinnedModel::CreateDemoTentacle(am.DemoSegments);
        } else {
            am.Model = sage::render::SkinnedModel::Load(am.Path);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Anim") << "Не удалось подготовить анимированную модель '"
                          << (am.Path.empty() ? "<demo>" : am.Path) << "': " << e.what();
        am.Model = nullptr;
        return;
    }
    am.Anim.SetRig(&am.Model->GetSkeleton(), &am.Model->Clips());
    am.Anim.SetSpeed(am.Speed);
    if (am.Model->Clips().empty()) return;      // нет клипов — остаётся bind-поза
    int clip = (am.Clip >= 0 && am.Clip < am.Model->Clips().size()) ? am.Clip : 0;
    am.Anim.Play(clip, am.Loop);
    if (!am.Playing) am.Anim.Stop();
}

void UpdateAnimators(Scene& scene, float dt) {
    auto view = scene.Registry().view<AnimatedModelComponent>();
    for (auto e : view) {
        AnimatedModelComponent& am = view.get<AnimatedModelComponent>(e);
        EnsureReady(am);
        if (!am.Model) continue;
        am.Anim.SetSpeed(am.Speed);
        am.Anim.Update(am.Playing ? dt : 0.0f); // 0 dt: держим текущую позу
    }
}

void DrawAnimatedModels(Scene& scene, const glm::mat4& view, const glm::mat4& proj,
                        const glm::vec3& viewPos, const LightingEnvironment& env,
                        const glm::mat4& lightMatrix, unsigned int shadowMap, bool shadowsEnabled) {
    auto v = scene.Registry().view<AnimatedModelComponent, Transform>();
    for (auto e : v) {
        AnimatedModelComponent& am = v.get<AnimatedModelComponent>(e);
        if (!am.Model) continue;
        const Transform& tr = v.get<Transform>(e);
        am.Model->Draw(tr.GetMatrix(), view, proj, viewPos, env, am.Anim.BoneMatrices(),
                       lightMatrix, shadowMap, shadowsEnabled);
    }
}

void DrawAnimatedModelsDepth(Scene& scene, const glm::mat4& lightMatrix) {
    auto v = scene.Registry().view<AnimatedModelComponent, Transform>();
    for (auto e : v) {
        AnimatedModelComponent& am = v.get<AnimatedModelComponent>(e);
        if (!am.Model) continue;
        const Transform& tr = v.get<Transform>(e);
        am.Model->DrawDepth(tr.GetMatrix(), lightMatrix, am.Anim.BoneMatrices());
    }
}

} // namespace sage::anim
