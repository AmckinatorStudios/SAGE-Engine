#include "sage/anim/AnimationSystem.h"

#include <algorithm>
#include <exception>

#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/anim/IK.h"
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/norm.hpp>

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


// --- Обратная кинематика поверх позы ---------------------------------------
//
// IK считается ВТОРЫМ проходом: сначала аниматор кладёт позу клипа, потом по
// её глобальным матрицам решаются цели, результат уходит в тот же слой
// переопределений, и поза пересчитывается. Иначе никак: солверу нужны
// глобальные положения костей, а они и есть результат первого прохода.
//
// Свои переопределения IK держит в ХВОСТЕ вектора позы (после тех, что
// поставил скрипт), но пишет в те же индексы костей — поэтому «повернуть
// голову скриптом» и «смотреть на цель по IK» на одну кость не уживаются, и
// это честно: две команды на одну кость и должны спорить.
void SolveIKGoals(Scene& scene, entt::entity e, AnimatedModelComponent& am, IKComponent& ik,
                  float dt) {
    if (!am.Model) return;
    const sage::anim::Skeleton& sk = am.Model->GetSkeleton();
    if (sk.Count() == 0) return;

    // Откат прошлого решения — ДО всех выходов: IK считается от позы КЛИПА, а
    // не поверх себя же. Восстанавливаем ровно то, что стояло в затронутых
    // костях до него, так что ручная поза от скрипта переживает IK.
    //
    // Именно «до выходов»: если бы откат жил ниже, рядом с решением целей, то
    // выключенная цель (или выключенный целиком компонент) уходила бы по
    // early-out и оставляла свой последний поворот в позе НАВСЕГДА — снять IK
    // было бы нечем.
    if (!ik.Touched.empty()) {
        for (size_t k = 0; k < ik.Touched.size(); ++k) {
            const int j = ik.Touched[k];
            if (j >= 0 && j < (int)am.PoseOverrides.size()) am.PoseOverrides[(size_t)j] = ik.Saved[k];
        }
        ik.Touched.clear();
        ik.Saved.clear();
        am.Anim.SetPoseOverride(am.PoseOverrides.empty() ? nullptr : &am.PoseOverrides);
        am.Anim.Update(0.0f);
    }

    if (!ik.Enabled || ik.Goals.empty()) return;

    // Мир <-> модель. Цели игра задаёт В МИРЕ (там же, где пол и предметы), а
    // солверы работают в пространстве модели — иначе они зависели бы от того,
    // куда повёрнута и как отмасштабирована сущность.
    const glm::mat4 world = scene.WorldMatrix(e);
    const glm::mat4 toModel = glm::inverse(world);

    bool any = false;
    for (IKGoal& g : ik.Goals) {
        if (!g.Enabled || g.Weight <= 0.0f) continue;
        if (!g.Resolved) {
            g.Resolved = true;
            for (int i = 0; i < sk.Count(); ++i)
                if (sk.Joints[(size_t)i].Name == g.Bone) { g.EndJoint = i; break; }
            // ChainLength считается в КОСТЯХ, а солверам нужны суставы: у двух
            // костей их три (бедро-колено-ступня). Отсюда +1 — без него корень
            // и середина схлопывались бы в одну кость, и нога не гнулась.
            if (g.EndJoint >= 0 && !g.Aim) {
                const std::vector<int> chain =
                    sage::anim::ChainFromEnd(sk, g.EndJoint, std::max(g.ChainLength, 2) + 1);
                if (chain.size() >= 3) {
                    g.RootJoint = chain.front();
                    g.MidJoint = chain[chain.size() - 2];
                }
            }
            if (g.EndJoint < 0) {
                LOG_WARN("Anim") << "IK: кости '" << g.Bone << "' нет в скелете — цель пропущена";
            }
        }
        if (g.EndJoint < 0) continue;
        any = true;
    }
    if (!any) return;

    if ((int)am.PoseOverrides.size() != sk.Count()) am.PoseOverrides.resize((size_t)sk.Count());

    // Запомнить исходное состояние кости перед тем, как её перепишет солвер.
    auto remember = [&](const sage::anim::IKResult& r) {
        for (int j : r.Joints) {
            if (j < 0 || j >= (int)am.PoseOverrides.size()) continue;
            if (std::find(ik.Touched.begin(), ik.Touched.end(), j) != ik.Touched.end()) continue;
            ik.Touched.push_back(j);
            ik.Saved.push_back(am.PoseOverrides[(size_t)j]);
        }
    };

    for (IKGoal& g : ik.Goals) {
        if (!g.Enabled || g.Weight <= 0.0f || g.EndJoint < 0) continue;
        const std::vector<glm::mat4>& globals = am.Anim.GlobalMatrices();
        if ((int)globals.size() != sk.Count()) return;

        // Прилипание к земле. Пока стопа опущена (её высота в модели ниже
        // порога), держим ту мировую точку, где она коснулась пола. Это и есть
        // лекарство от скольжения: при переносе анимации с чужого скелета длина
        // шага не совпадает с корневым движением, и без фиксации нога едет.
        glm::vec3 worldTarget = g.Target;
        if (g.Lock) {
            const glm::vec3 footModel(globals[(size_t)g.EndJoint][3]);
            const glm::vec3 footWorld(world * glm::vec4(footModel, 1.0f));
            worldTarget = sage::anim::FootLockTarget(g.Locked, g.LockedAt, g.LockBlend, dt,
                                                     footModel.y, g.PlantHeight, g.ReleaseTime,
                                                     footWorld, g.Target);
        }

        const glm::vec3 targetModel = glm::vec3(toModel * glm::vec4(worldTarget, 1.0f));

        sage::anim::IKResult res;
        if (g.Aim) {
            res = sage::anim::SolveAim(sk, globals, g.EndJoint, g.AimAxis, targetModel,
                                       g.AimMaxAngle);
        } else if (g.ChainLength <= 2 && g.MidJoint >= 0 && g.RootJoint >= 0) {
            glm::vec3 poleModel;
            const glm::vec3* pole = nullptr;
            if (g.UsePole) {
                poleModel = glm::vec3(toModel * glm::vec4(g.Pole, 1.0f));
                pole = &poleModel;
            }
            res = sage::anim::SolveTwoBone(sk, globals,
                                           {g.RootJoint, g.MidJoint, g.EndJoint},
                                           targetModel, pole);
        } else {
            const std::vector<int> chain =
                sage::anim::ChainFromEnd(sk, g.EndJoint, g.ChainLength + 1);
            if (chain.size() >= 2) res = sage::anim::SolveChain(sk, globals, chain, targetModel);
        }
        if (!res.Solved) continue;
        remember(res);
        sage::anim::ApplyIK(res, am.PoseOverrides, sk.Count(), g.Weight);

        // Пересчитываем позу, чтобы СЛЕДУЮЩАЯ цель видела уже поправленный
        // скелет: иначе две цели на одной ноге решались бы по устаревшим
        // матрицам и спорили друг с другом.
        am.Anim.SetPoseOverride(&am.PoseOverrides);
        am.Anim.Update(0.0f);

        // Доворот конца (ступня по нормали склона) — после позиционирования: он
        // опирается на уже поставленную кость.
        if (!g.Aim && glm::length2(g.AlignNormal) > 1e-6f) {
            const glm::vec3 nModel =
                glm::normalize(glm::vec3(toModel * glm::vec4(g.AlignNormal, 0.0f)));
            // Доворачиваем на РАЗНИЦУ между полом, под который анимировали
            // (ровный, нормаль +Y), и настоящим. Про оси самой кости не
            // предполагаем ничего: у реальных ригов «вверх» ступни — это и +Y,
            // и −Z, и что угодно между, так что любая догадка была бы неверна
            // ровно на половине моделей.
            const glm::quat align = glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), nModel);
            glm::mat3 basis(globals[(size_t)g.EndJoint]);
            for (int c = 0; c < 3; ++c) {
                const float len = glm::length(basis[c]);
                if (len > 1e-6f) basis[c] /= len;   // масштаб сущности не должен ехать в поворот
            }
            const glm::quat cur = glm::quat_cast(basis);
            sage::anim::IKResult rot = sage::anim::AimEnd(sk, globals, g.EndJoint,
                                                          glm::normalize(align * cur), g.Weight);
            if (rot.Solved) {
                remember(rot);
                sage::anim::ApplyIK(rot, am.PoseOverrides, sk.Count(), g.Weight);
                am.Anim.SetPoseOverride(&am.PoseOverrides);
                am.Anim.Update(0.0f);
            }
        }
    }
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

        // IK — последним: он правит уже готовую позу и обязан видеть её целиком,
        // включая кросс-фейд и ручные переопределения.
        if (auto* ik = scene.Registry().try_get<IKComponent>(e)) SolveIKGoals(scene, e, am, *ik, dt);
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
