// ---------------------------------------------------------------------------
// Эталонные кадры — анимация: морфинг и обратная кинематика.
//
// Скелет и морфинг проверяются отдельно от кадра: у них своя цепочка (свой
// шейдер со скиннингом, свой проход), и ломаются они независимо от того, что
// происходит с освещением и пост-обработкой.
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/anim/AnimationSystem.h"
#include "sage/assets/AssetCache.h"
#include "sage/ecs/DecalSystem.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderBatch.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/GridRenderer.h"
#include "sage/render/LensFlare.h"
#include "sage/render/PostFX.h"
#include "sage/render/Reflection.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/ShadowAtlas.h"
#include "sage/render/ShadowMap.h"
#include "sage/render/SkinnedModel.h"
#include "sage/render/SkyRenderer.h"
#include "sage/rhi/Conformance.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace sage::rendertest {
namespace {

// --- Блендшейпы ------------------------------------------------------------

// Морф-цели меняют саму ФОРМУ меша, а не его положение, поэтому единственная
// честная проверка — картинка. Демо-модель движка несёт две процедурные цели
// («Fatten» — раздуть, «Bend» — отклонить в сторону), и веса им ставятся ровно тем
// же способом, что и настоящему лицу.
void TestMorphTargets(FrameRenderer& r) {
    auto scene = std::make_unique<Scene>("MorphTest");
    scene->Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.35f));
    scene->Lighting.Sun.Intensity = 1.2f;
    scene->Lighting.SkyColor = {0.40f, 0.48f, 0.64f};
    scene->Lighting.GroundColor = {0.20f, 0.17f, 0.15f};
    scene->Lighting.AmbientStrength = 0.40f;
    scene->Lighting.Skybox.Enabled = false;

    GameObject character = scene->CreateObject("Character");
    character.GetTransform().Position = {0.0f, 0.0f, 0.0f};
    AnimatedModelComponent anim;
    anim.Playing = false; // поза не должна зависеть от времени: тест детерминированный
    scene->Registry().emplace<AnimatedModelComponent>(character.Entity(), std::move(anim));

    // Модель грузится лениво — один тик системы поднимает её и привязывает риг.
    sage::anim::UpdateAnimators(*scene, 0.0f);

    AnimatedModelComponent& am = scene->Registry().get<AnimatedModelComponent>(character.Entity());
    if (!am.Model) {
        std::printf("[FAIL] демо-модель не загрузилась — блендшейпы проверить нечем\n");
        CountFail();
        return;
    }
    Check(am.Model->MorphCount() == 2, "у демо-модели две морф-цели");
    Check(am.Model->FindMorph("Fatten") == 0, "морф-цель находится по имени");
    Check(am.Model->FindMorph("НетТакой") == -1, "несуществующая цель не находится");
    Check((int)am.MorphWeights.size() == am.Model->MorphCount(),
          "веса завелись по числу целей модели");

    const glm::mat4 proj = PerspectiveProj();

    // Нулевые веса — исходная форма.
    const Image neutral = RenderFrame(r, *scene, proj, BaseSettings(), kW, kH);
    Report("morph_neutral", CompareWithReference("morph_neutral", neutral));

    // Полный вес первой цели.
    am.MorphWeights = {1.0f, 0.0f};
    const Image fat = RenderFrame(r, *scene, proj, BaseSettings(), kW, kH);
    Report("morph_fatten", CompareWithReference("morph_fatten", fat));

    // Вторая цель — независимо от первой.
    am.MorphWeights = {0.0f, 1.0f};
    const Image bend = RenderFrame(r, *scene, proj, BaseSettings(), kW, kH);
    Report("morph_bend", CompareWithReference("morph_bend", bend));

    auto meanDiff = [](const Image& a, const Image& b) {
        long long sum = 0;
        for (size_t i = 0; i < a.Pixels.size(); ++i) {
            sum += std::abs((int)a.Pixels[i] - (int)b.Pixels[i]);
        }
        return (double)sum / (double)a.Pixels.size();
    };

    const double fatVsNeutral = meanDiff(fat, neutral);
    const double bendVsNeutral = meanDiff(bend, neutral);
    const double fatVsBend = meanDiff(fat, bend);
    std::printf("       Fatten против исходной: %.2f, Bend: %.2f, между собой: %.2f\n",
                fatVsNeutral, bendVsNeutral, fatVsBend);
    Check(fatVsNeutral > 1.0, "вес первой цели меняет форму");
    Check(bendVsNeutral > 1.0, "вес второй цели меняет форму");
    // Без этой проверки обе цели могли бы применяться как одна и та же.
    Check(fatVsBend > 1.0, "цели независимы — дают разную форму");

    // Половинный вес обязан дать промежуточную форму, а не переключение.
    am.MorphWeights = {0.5f, 0.0f};
    const Image half = RenderFrame(r, *scene, proj, BaseSettings(), kW, kH);
    const double halfVsNeutral = meanDiff(half, neutral);
    std::printf("       половинный вес: до исходной %.2f, до полной %.2f\n", halfVsNeutral,
                meanDiff(half, fat));
    Check(halfVsNeutral > 0.2 && halfVsNeutral < fatVsNeutral,
          "половинный вес даёт промежуточную форму");

    // Возврат к нулю обязан вернуть исходную форму в точности.
    am.MorphWeights = {0.0f, 0.0f};
    const Image back = RenderFrame(r, *scene, proj, BaseSettings(), kW, kH);
    Check(meanDiff(back, neutral) < 0.01, "нулевые веса возвращают исходную форму");
}

// --- Обратная кинематика в ECS ---------------------------------------------

// Солверы проверены в юнит-тестах на голом скелете; здесь проверяется ОБВЯЗКА —
// то, что между Lua и математикой: поиск кости по имени, перевод цели из мира в
// пространство модели через трансформ сущности, откат прошлого решения и то,
// что выключенная цель перестаёт держать кость. Именно на этом слое и ломалось:
// цепочка набиралась на сустав короче, и корень с серединой схлопывались в одну
// кость.
//
// Модель — процедурный демо-щупалец: цепочка из шести суставов вдоль +Y, без
// внешних ассетов. GL-контекст нужен потому, что SkinnedModel строит буферы на
// видеокарте уже при загрузке.
void TestInverseKinematicsECS() {
    std::printf("=== IK в ECS: цель в мире -> поза ===\n");
    auto scene = std::make_unique<Scene>("IKTest");

    GameObject rig = scene->CreateObject("Rig");
    // Сущность СДВИНУТА и ПОВЁРНУТА: если бы движок считал цель в пространстве
    // модели, тест бы это поймал — при повороте на 90° промах стал бы метровым.
    rig.GetTransform().Position = {3.0f, 1.0f, -2.0f};
    rig.GetTransform().Rotation = {0.0f, 90.0f, 0.0f};
    AnimatedModelComponent anim;
    anim.Playing = false;   // поза не должна зависеть от времени
    scene->Registry().emplace<AnimatedModelComponent>(rig.Entity(), std::move(anim));
    sage::anim::UpdateAnimators(*scene, 0.0f);

    AnimatedModelComponent& am = scene->Registry().get<AnimatedModelComponent>(rig.Entity());
    if (!am.Model || am.Model->GetSkeleton().Count() < 3) {
        std::printf("[FAIL] демо-модель не поднялась — IK проверять не на чем\n");
        CountFail();
        return;
    }
    const sage::anim::Skeleton& sk = am.Model->GetSkeleton();
    const std::string endBone = sk.Joints[(size_t)sk.Count() - 1].Name;

    auto endWorld = [&] {
        return glm::vec3(scene->WorldMatrix(rig.Entity()) *
                         glm::vec4(glm::vec3(am.Anim.GlobalMatrices()[(size_t)sk.Count() - 1][3]),
                                   1.0f));
    };
    const glm::vec3 restWorld = endWorld();

    // Цель — рядом с концом цепочки, чтобы она была достижима на любом риге.
    const glm::vec3 target = restWorld + glm::vec3(0.35f, -0.25f, 0.15f);

    IKComponent ik;
    IKGoal goal;
    goal.Bone = endBone;
    goal.ChainLength = 3;
    goal.Target = target;
    ik.Goals.push_back(goal);
    scene->Registry().emplace<IKComponent>(rig.Entity(), std::move(ik));

    sage::anim::UpdateAnimators(*scene, 1.0f / 60.0f);
    const float missed = glm::length(endWorld() - target);
    std::printf("       промах конца цепочки: %.4f м (был %.3f м)\n", missed,
                glm::length(restWorld - target));
    Check(missed < 0.02f, "цель в МИРОВЫХ координатах достигнута сквозь трансформ сущности");

    IKComponent& live = scene->Registry().get<IKComponent>(rig.Entity());
    Check(live.Goals[0].EndJoint == sk.Count() - 1, "кость найдена по имени");
    Check(live.Goals[0].RootJoint >= 0 && live.Goals[0].MidJoint > live.Goals[0].RootJoint,
          "цепочка набралась: корень и середина — РАЗНЫЕ кости");

    // Повторные кадры без движения цели не должны никуда уползать: IK обязан
    // считаться от позы клипа заново, а не поверх собственного прошлого ответа.
    for (int i = 0; i < 30; ++i) sage::anim::UpdateAnimators(*scene, 1.0f / 60.0f);
    const float driftMiss = glm::length(endWorld() - target);
    std::printf("       после 30 кадров: %.4f м\n", driftMiss);
    Check(std::fabs(driftMiss - missed) < 0.005f, "решение не уползает от кадра к кадру");

    // Выключенная цель обязана ОТПУСТИТЬ кость обратно в позу клипа. Без отката
    // прошлого решения она осталась бы висеть в последней точке навсегда.
    live.Goals[0].Enabled = false;
    sage::anim::UpdateAnimators(*scene, 1.0f / 60.0f);
    const float backToRest = glm::length(endWorld() - restWorld);
    std::printf("       после выключения цели до позы покоя: %.4f м\n", backToRest);
    Check(backToRest < 0.01f, "выключенная цель возвращает кость в позу клипа");

    // И обратно: включённая цель снова держит.
    live.Goals[0].Enabled = true;
    sage::anim::UpdateAnimators(*scene, 1.0f / 60.0f);
    Check(glm::length(endWorld() - target) < 0.02f, "включённая обратно цель снова держит");
}

} // namespace

void RunAnimationChecks(FrameRenderer& r) {
    TestMorphTargets(r);
    TestInverseKinematicsECS();
}

} // namespace sage::rendertest
