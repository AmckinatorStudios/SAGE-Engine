#include "sage/anim/AnimationSystem.h"

#include <exception>

#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

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
            // Через кэш: дюжина одинаковых NPC — это одна модель, а не дюжина.
            am.Model = ResourceManager::Instance().GetSkinnedModel(am.Path);
            if (!am.Model) throw std::runtime_error("модель не загрузилась");
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
        am.Anim.SetRootMotion(am.RootMotion);
        am.Anim.Update(am.Playing ? dt : 0.0f); // 0 dt: держим текущую позу

        // Корневое движение переносим на трансформ сущности. Поворотом
        // сущности — иначе персонаж, развёрнутый на 180°, шёл бы задом наперёд:
        // клип двигает его «вперёд» в СВОИХ координатах, а куда смотрит
        // сущность, знает только её трансформ.
        if (am.RootMotion) {
            const glm::vec3 delta = am.Anim.ConsumeRootDelta();
            if (glm::dot(delta, delta) > 0.0f) {
                Transform& tr = scene.Registry().get<Transform>(e);
                const glm::mat4 rot = glm::mat4_cast(glm::quat(glm::radians(tr.Rotation)));
                tr.Position += glm::vec3(rot * glm::vec4(delta * tr.Scale, 0.0f));
            }
        }
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
