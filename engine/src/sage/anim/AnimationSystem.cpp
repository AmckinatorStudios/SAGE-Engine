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
    // Веса блендшейпов появляются только вместе с моделью — до её загрузки
    // неизвестно даже, сколько их. Стартовое выражение берём из файла: художник
    // мог задать его в glTF (mesh.weights).
    if (am.MorphWeights.empty() && am.Model->MorphCount() > 0) {
        am.MorphWeights = am.Model->DefaultMorphWeights();
        am.MorphWeights.resize((size_t)am.Model->MorphCount(), 0.0f);
    }
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
        // Смена Clip в компоненте (редактор/скрипт) -> плавный кросс-фейд к нему
        // (или мгновенно, если BlendTime<=0). Anim.CurrentClip() сразу становится
        // новым, поэтому переход не перезапускается каждый кадр.
        int want = am.Clip;
        int clipCount = (int)am.Model->Clips().size();
        if (clipCount > 0 && want >= 0 && want < clipCount && want != am.Anim.CurrentClip()) {
            if (am.BlendTime > 0.0f) am.Anim.CrossFade(want, am.BlendTime, am.Loop);
            else am.Anim.Play(want, am.Loop);
        }
        // Переопределения позы: указатель, а не копия — Animator их не владеет,
        // а вектор живёт в компоненте и переживает кадр. Пустой вектор снимает
        // переопределение (иначе снять его было бы нечем).
        am.Anim.SetPoseOverride(am.PoseOverrides.empty() ? nullptr : &am.PoseOverrides);
        am.Anim.Update(am.Playing ? dt : 0.0f); // 0 dt: держим текущую позу
    }
}

void DrawAnimatedModels(Scene& scene, const glm::mat4& view, const glm::mat4& proj,
                        const glm::vec3& viewPos, const LightingEnvironment& env,
                        const ShadowBinding& shadows) {
    auto v = scene.Registry().view<AnimatedModelComponent, Transform>();
    for (auto e : v) {
        AnimatedModelComponent& am = v.get<AnimatedModelComponent>(e);
        if (!am.Model) continue;
        am.Model->Draw(scene.WorldMatrix(e), view, proj, viewPos, env, am.Anim.BoneMatrices(),
                       shadows, &am.MorphWeights);
    }
}

void DrawAnimatedModelsDepth(Scene& scene, const glm::mat4& lightMatrix) {
    auto v = scene.Registry().view<AnimatedModelComponent, Transform>();
    for (auto e : v) {
        AnimatedModelComponent& am = v.get<AnimatedModelComponent>(e);
        if (!am.Model) continue;
        am.Model->DrawDepth(scene.WorldMatrix(e), lightMatrix, am.Anim.BoneMatrices(),
                            &am.MorphWeights);
    }
}

} // namespace sage::anim
