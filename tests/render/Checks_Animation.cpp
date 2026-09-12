// ---------------------------------------------------------------------------
// Эталонные кадры — анимация: морфинг и обратная кинематика.
//
// Скелет и морфинг проверяются отдельно от кадра: у них своя цепочка (свой
// шейдер со скиннингом, свой проход), и ломаются они независимо от того, что
// происходит с освещением и пост-обработкой.
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include "sage/anim/ClipFile.h"

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

// Демо-щупальце ЯВНО. Компонент Animation больше не строит его сам, когда в
// Mesh нет модели: подсовывать человеку чужую работающую анимацию вместо
// ответа «анимировать нечего» — худший способ объяснить, почему у него ничего
// не движется (см. AnimationSystem.cpp). Но как ОСНАСТКА щупальце осталось
// незаменимым: скиннинг, блендшейпы и обратная кинематика обязаны быть
// проверяемы без единого ассета на диске, иначе проверка зависит от файла,
// который кто-нибудь однажды заменит.
//
// Поэтому тесты собирают его руками и кладут в компонент готовым.
void GiveDemoSkeleton(AnimationComponent& am, int segments = 6) {
    am.Model = sage::render::SkinnedModel::CreateDemoTentacle(segments);
    am.Ready = true;              // системе переинициализировать нечего
    am.ResolvedFrom.clear();
    if (!am.Model) return;
    am.Anim.SetRig(&am.Model->GetSkeleton(), &am.Model->Clips());
    am.Anim.SetSpeed(am.Speed);
    if (am.MorphWeights.empty() && am.Model->MorphCount() > 0) {
        am.MorphWeights = am.Model->DefaultMorphWeights();
        am.MorphWeights.resize((size_t)am.Model->MorphCount(), 0.0f);
    }
    if (!am.Model->Clips().empty()) {
        am.Anim.Play(0, am.Loop);
        if (!am.Playing) am.Anim.Stop();
    }
}

// же способом, что и настоящему лицу.
void TestMorphTargets(FrameRenderer& r) {
    auto scene = std::make_unique<Scene>("MorphTest");
    scene->Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.35f));
    scene->Lighting.Sun.Intensity = 1.2f;
    scene->Lighting.SkyColor = {0.40f, 0.48f, 0.64f};
    scene->Lighting.GroundColor = {0.20f, 0.17f, 0.15f};
    scene->Lighting.AmbientStrength = 0.40f;
    // Ambient здесь задан ЯВНО и небу не подчиняется — значит «свои значения»
    // (см. LightingEnvironment::AmbientMode). Выключенное небо иначе забирает
    // с собой и окружающий свет, и проверять было бы нечего: чёрный кадр.
    scene->Lighting.AmbientMode = LightingEnvironment::AmbientSource::Custom;
    scene->Lighting.Skybox.Enabled = false;

    GameObject character = scene->CreateObject("Character");
    character.GetTransform().Position = {0.0f, 0.0f, 0.0f};
    AnimationComponent anim;
    anim.Playing = false; // поза не должна зависеть от времени: тест детерминированный
    scene->Registry().emplace<AnimationComponent>(character.Entity(), std::move(anim));

    AnimationComponent& am = scene->Registry().get<AnimationComponent>(character.Entity());
    GiveDemoSkeleton(am);
    // Тик системы: поза, блендшейпы и переопределения раскладываются ею.
    sage::anim::UpdateAnimators(*scene, 0.0f);
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
    AnimationComponent anim;
    anim.Playing = false;   // поза не должна зависеть от времени
    scene->Registry().emplace<AnimationComponent>(rig.Entity(), std::move(anim));

    AnimationComponent& am = scene->Registry().get<AnimationComponent>(rig.Entity());
    GiveDemoSkeleton(am);
    sage::anim::UpdateAnimators(*scene, 0.0f);
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


// --- Скелет берётся из Mesh, и объект не рисуется дважды --------------------
//
// Это проверка ТОЙ САМОЙ связи, ради которой «Animated Model» перестал быть
// отдельным компонентом: модель задаётся в Mesh, а Animation надевает на неё
// скелет. Проверять её структурой в памяти мало — там всё сходится по
// построению; здесь модель действительно грузится с диска, а потом считается,
// сколько раз объект попал в кадр.
//
// Двойная отрисовка — не теоретическая опасность: файл читают ДВА загрузчика,
// статический и скелетный, и до явной проверки в проходе статики персонаж
// рисовался бы дважды — раз в позе покоя и раз анимированным поверх.
void TestAnimationUsesMeshModel(FrameRenderer& r) {
    const char* model =
#ifdef SAGE_TEST_MODEL
        SAGE_TEST_MODEL;
#else
        "assets/test_model.glb";
#endif

    auto scene = std::make_unique<Scene>("MeshAnimation");
    scene->Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.35f));
    scene->Lighting.Sun.Intensity = 1.2f;
    // Ambient здесь задан ЯВНО и небу не подчиняется — значит «свои значения»
    // (см. LightingEnvironment::AmbientMode). Выключенное небо иначе забирает
    // с собой и окружающий свет, и проверять было бы нечего: чёрный кадр.
    scene->Lighting.AmbientMode = LightingEnvironment::AmbientSource::Custom;
    scene->Lighting.Skybox.Enabled = false;

    GameObject hero = scene->CreateObject("Hero");
    MeshRendererComponent& mr = scene->Registry().emplace<MeshRendererComponent>(hero.Entity());
    mr.Ref.type = MeshRef::Type::Model;
    mr.Ref.path = model;
    AnimationComponent anim;
    anim.Playing = false;
    scene->Registry().emplace<AnimationComponent>(hero.Entity(), std::move(anim));

    sage::anim::UpdateAnimators(*scene, 0.0f);
    AnimationComponent& am = scene->Registry().get<AnimationComponent>(hero.Entity());
    Check(am.Model != nullptr, "Animation взяла модель из Mesh");
    if (!am.Model) return;
    Check(am.Model->GetSkeleton().Count() > 1, "скелет модели из Mesh разобран");
    Check(!am.Model->Clips().empty(), "клипы модели из Mesh доехали до аниматора");

    // Проход статики обязан ПРОПУСТИТЬ этот объект: его рисует скелетный путь.
    Framebuffer fbo(kW, kH);
    fbo.Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
    device.Clear(true, true);
    const LightingEnvironment env = sage::ecs::CollectLighting(*scene);
    const sage::ecs::RenderStats stats =
        r.Batch.RenderColor(*scene, TestView(), PerspectiveProj(), kEye, env, ShadowBinding(), 0);
    device.BindDefaultFramebuffer();
    Check(stats.Total == 0, "статический проход не рисует анимированный объект второй раз");

    // Смена модели в Mesh обязана переехать в скелет: иначе клипы играли бы по
    // костям, которых у новой модели нет.
    mr.Ref.path.clear();
    mr.Ref.type = MeshRef::Type::None;
    sage::anim::UpdateAnimators(*scene, 0.0f);
    Check(am.ResolvedFrom.empty(), "смена модели в Mesh переинициализирует скелет");
}

// --- ПЕРСОНАЖ УЧАСТВУЕТ В ОТЛАДОЧНЫХ ВИДАХ КАДРА -----------------------------
//
// Разбор кадра по слагаемым (нормали, шероховатость, металличность…) существует
// затем, чтобы отвечать на вопрос «почему выглядит не так». Скелетный проход
// про эти виды не знал вовсе: в шейдере руками разбирались два номера из
// одиннадцати, а сам номер туда даже не передавался — uShadingMode оставался
// нулём при любом выборе в редакторе. На экране это выглядело так: включаешь
// «Нормали», всё вокруг становится цветным, и посреди этого стоит обычный
// освещённый персонаж — то есть главное, что в кадре есть, из разбора выпадало.
//
// Проверяется СВОЙСТВО: кадр в режиме «Нормали» обязан отличаться от обычного
// именно на персонаже, и цвета в нём — это нормали (сине-зелёная гамма
// N*0.5+0.5), а не albedo модели.
void TestSkinnedDebugView(FrameRenderer& r) {
    const char* model =
#ifdef SAGE_TEST_MODEL
        SAGE_TEST_MODEL;
#else
        "assets/test_model.glb";
#endif

    auto scene = std::make_unique<Scene>("SkinnedDebugView");
    scene->Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.35f));
    scene->Lighting.Sun.Intensity = 1.4f;
    scene->Lighting.AmbientMode = LightingEnvironment::AmbientSource::Custom;
    scene->Lighting.Skybox.Enabled = false;

    GameObject hero = scene->CreateObject("Hero");
    MeshRendererComponent& mr = scene->Registry().emplace<MeshRendererComponent>(hero.Entity());
    mr.Ref.type = MeshRef::Type::Model;
    mr.Ref.path = model;
    AnimationComponent anim;
    anim.Playing = false;
    scene->Registry().emplace<AnimationComponent>(hero.Entity(), std::move(anim));
    sage::anim::UpdateAnimators(*scene, 0.0f);
    if (!scene->Registry().get<AnimationComponent>(hero.Entity()).Model) {
        Check(false, "скелетная модель для отладочного вида загрузилась");
        return;
    }

    auto frameWithMode = [&](int shadingMode) {
        const LightingEnvironment env = sage::ecs::CollectLighting(*scene);
        Framebuffer fbo(kW, kH);
        fbo.Bind();
        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        device.Clear(true, true);
        sage::anim::DrawAnimatedModels(*scene, TestView(), PerspectiveProj(), kEye, env,
                                       ShadowBinding(), nullptr, shadingMode);
        Image img = Capture(kW, kH);
        device.BindDefaultFramebuffer();
        return img;
    };

    const Image shaded = frameWithMode(0);
    const Image normals = frameWithMode(2);   // DebugView::Normals

    long long diff = 0;
    size_t lit = 0;
    for (size_t i = 0; i < shaded.Pixels.size() && i < normals.Pixels.size(); ++i) {
        diff += std::abs((int)shaded.Pixels[i] - (int)normals.Pixels[i]);
        if (shaded.Pixels[i] > 8) ++lit;
    }
    const double mean = (double)diff / (double)std::max<size_t>(shaded.Pixels.size(), 1);
    std::printf("       персонаж: непустых байт %zu, отличие Нормали-vs-Shaded %.2f\n", lit, mean);
    Check(lit > 500, "персонаж вообще нарисован скелетным проходом");
    Check(mean > 2.0, "режим «Нормали» меняет и персонажа, а не только сцену вокруг");

    // И это ИМЕННО отладочный вид, а не «случайно другая картинка»: показанная
    // величина от освещения не зависит. Гасим солнце — обычный кадр обязан
    // измениться, кадр нормалей обязан остаться прежним ДО БАЙТА. Проверка не
    // знает ни про цвет модели, ни про то, куда она смотрит: она спрашивает
    // свойство отладочного вида.
    scene->Lighting.Sun.Intensity = 0.0f;
    const Image shadedDark = frameWithMode(0);
    const Image normalsDark = frameWithMode(2);

    long long shadedDiff = 0, normalsDiff = 0;
    for (size_t i = 0; i < shaded.Pixels.size(); ++i) {
        shadedDiff += std::abs((int)shaded.Pixels[i] - (int)shadedDark.Pixels[i]);
        normalsDiff += std::abs((int)normals.Pixels[i] - (int)normalsDark.Pixels[i]);
    }
    std::printf("       погасили солнце: обычный кадр изменился на %lld, нормали на %lld\n",
                shadedDiff, normalsDiff);
    Check(shadedDiff > 0, "обычный кадр персонажа зависит от света (иначе проверять нечего)");
    Check(normalsDiff == 0, "в режиме «Нормали» персонаж показан нормалями, а не освещением");
}

// --- Клип, вынутый в файл, играет ровно так же ------------------------------
//
// Ради этого формат и заводился, и проверять его надо ИМЕННО так: не «файл
// записался и прочитался» (это следствие), а «поза совпала». Клип проходит весь
// путь — из модели в .sageanim, оттуда обратно в компонент, привязка костей по
// именам, — и палитра матриц обязана совпасть с той, что даёт клип из самой
// модели в тот же момент времени. Любая потеря по дороге (кватернион без w,
// ступенчатая интерполяция, перепутанная кость) видна здесь числом, а в кадре
// выглядела бы «персонаж как-то не так дёргается».
void TestClipFileMatchesModelClip() {
    const char* model =
#ifdef SAGE_TEST_MODEL
        SAGE_TEST_MODEL;
#else
        "assets/test_model.glb";
#endif
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "sage_clipfile";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    auto scene = std::make_unique<Scene>("ClipFile");
    GameObject hero = scene->CreateObject("Hero");
    MeshRendererComponent& mr = scene->Registry().emplace<MeshRendererComponent>(hero.Entity());
    mr.Ref.type = MeshRef::Type::Model;
    mr.Ref.path = model;
    scene->Registry().emplace<AnimationComponent>(hero.Entity());

    sage::anim::UpdateAnimators(*scene, 0.0f);
    AnimationComponent& am = scene->Registry().get<AnimationComponent>(hero.Entity());
    Check(am.Model != nullptr, "модель для клипа загрузилась");
    if (!am.Model || am.Model->Clips().empty()) {
        std::printf("       у тестовой модели нет клипов — проверка пропущена\n");
        CountFail();
        std::filesystem::remove_all(dir, ec);
        return;
    }

    // Проигрываем клип МОДЕЛИ и запоминаем позу в середине.
    constexpr float kAt = 0.37f;
    sage::anim::UpdateAnimators(*scene, kAt);
    const std::vector<glm::mat4> fromModel = am.Anim.BoneMatrices();
    Check(!fromModel.empty(), "клип модели дал палитру костей");

    // Тот же клип — в файл и обратно.
    const std::filesystem::path file =
        dir / sage::anim::ClipFileName("hero", am.Model->Clips()[0].Name);
    sage::anim::SaveClip(sage::anim::ToAsset(am.Model->Clips()[0], am.Model->GetSkeleton()),
                         file.string());
    Check(std::filesystem::exists(file, ec), "файл клипа записан");

    auto scene2 = std::make_unique<Scene>("ClipFile2");
    GameObject hero2 = scene2->CreateObject("Hero");
    MeshRendererComponent& mr2 = scene2->Registry().emplace<MeshRendererComponent>(hero2.Entity());
    mr2.Ref.type = MeshRef::Type::Model;
    mr2.Ref.path = model;
    AnimationComponent fromFile;
    fromFile.ClipPath = file.string();
    scene2->Registry().emplace<AnimationComponent>(hero2.Entity(), std::move(fromFile));

    sage::anim::UpdateAnimators(*scene2, 0.0f);
    AnimationComponent& am2 = scene2->Registry().get<AnimationComponent>(hero2.Entity());
    Check(am2.MissingBones == 0, "все кости клипа нашлись в скелете своей же модели");
    Check(am2.Anim.ClipCount() == 1, "аниматор играет клип из файла");
    sage::anim::UpdateAnimators(*scene2, kAt);
    const std::vector<glm::mat4> fromDisk = am2.Anim.BoneMatrices();

    Check(fromDisk.size() == fromModel.size(), "палитра костей того же размера");
    float worst = 0.0f;
    if (fromDisk.size() == fromModel.size()) {
        for (size_t b = 0; b < fromDisk.size(); ++b)
            for (int c = 0; c < 4; ++c)
                for (int r2 = 0; r2 < 4; ++r2)
                    worst = std::max(worst, std::abs(fromDisk[b][c][r2] - fromModel[b][c][r2]));
    }
    std::printf("       клип из файла против клипа модели: худшее расхождение %.6f\n", worst);
    // Допуск — на запись чисел в текст и обратно, а не на «примерно похоже».
    Check(worst < 1e-3f, "поза из файла совпадает с позой из модели");

    std::filesystem::remove_all(dir, ec);
}

} // namespace

void RunAnimationChecks(FrameRenderer& r) {
    TestMorphTargets(r);
    TestAnimationUsesMeshModel(r);
    TestSkinnedDebugView(r);
    TestInverseKinematicsECS();
    TestClipFileMatchesModelClip();
}

} // namespace sage::rendertest
