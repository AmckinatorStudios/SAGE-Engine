// ---------------------------------------------------------------------------
// Качество теней солнца: отрыв тени, самозатенение, зерно полутени, стабильность
// при переподгонке карт, тень от каждого вида геометрии, цена прохода.
//
// Каждая проверка меряет ОДИН дефект числом и сравнивает кадр «с тенями» с тем
// же кадром «без теней» — тогда разница означает ровно тень, а не что-то ещё в
// освещении. Пороги проверены с обеих сторон: прежняя выборка (диск со
// случайным поворотом, запас глубины до полусотни текселей) их не проходит.
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/anim/AnimationSystem.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderBatch.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/Material.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/ScenePasses.h"
#include "sage/render/ShadowMap.h"
#include "sage/render/SkinnedModel.h"
#include "sage/render/Texture.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace sage::rendertest {
namespace {

// Направление, КУДА летит свет: по азимуту (от +z к +x) и высоте солнца.
glm::vec3 SunFrom(float elevationDeg, float azimuthDeg) {
    const float e = glm::radians(elevationDeg), a = glm::radians(azimuthDeg);
    return glm::normalize(glm::vec3(std::sin(a) * std::cos(e), -std::sin(e), std::cos(a) * std::cos(e)));
}

std::unique_ptr<Scene> SunScene(glm::vec3 sunDir, bool ground = true) {
    auto scene = std::make_unique<Scene>("SunShadow");
    scene->Lighting.Sun.Direction = sunDir;
    scene->Lighting.Sun.Intensity = 3.0f;
    scene->Lighting.Sun.Color = {1.0f, 0.98f, 0.94f};
    scene->Lighting.SkyColor = {0.40f, 0.48f, 0.64f};
    scene->Lighting.GroundColor = {0.20f, 0.17f, 0.15f};
    scene->Lighting.AmbientStrength = 0.15f;
    scene->Lighting.AmbientMode = LightingEnvironment::AmbientSource::Custom;
    scene->Lighting.Skybox.Enabled = false;
    if (ground) {
        GameObject g = scene->CreateObject("Ground");
        g.GetTransform().Scale = {60.0f, 1.0f, 60.0f};
        g.Renderer().Ref = MeshRef{MeshRef::Type::Plane};
        g.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Plane);
        g.Renderer().Color = {0.62f, 0.63f, 0.65f};
    }
    return scene;
}

GameObject AddPrimitive(Scene& scene, const char* name, MeshRef::Type type, glm::vec3 pos,
                        glm::vec3 scale) {
    GameObject obj = scene.CreateObject(name);
    obj.GetTransform().Position = pos;
    obj.GetTransform().Scale = scale;
    obj.Renderer().Ref = MeshRef{type};
    obj.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(type);
    obj.Renderer().Color = {0.78f, 0.76f, 0.72f};
    return obj;
}

struct Shot {
    glm::vec3 Eye;
    glm::vec3 Target;
};

// Кадр с тенями из shadows (уже подогнанной) или тот же кадр без теней.
// Проход теней — общий движковый (RenderShadowDepth): статика, инстансы и
// скелетные модели рисуются в карту тем же кодом, что и в игре.
Image RenderSun(FrameRenderer& r, Scene& scene, ShadowMap& shadows, const Shot& shot, bool enabled) {
    const LightingEnvironment env = sage::ecs::CollectLighting(scene);
    if (enabled) sage::render::RenderShadowDepth(shadows, scene, r.Batch, kW, kH);
    Framebuffer fbo(kW, kH);
    fbo.Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
    device.Clear(true, true);
    const glm::mat4 view = glm::lookAt(shot.Eye, shot.Target, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj = glm::perspective(glm::radians(45.0f), (float)kW / (float)kH, 0.05f, 300.0f);
    r.Batch.RenderColor(scene, view, proj, shot.Eye, env, ShadowBinding(shadows, enabled), 0);
    fbo.Resolve();
    fbo.Bind();
    return Capture(kW, kH);
}

// Сколько освещённых пикселей базового кадра заметно потемнело (красный канал).
long long Darkened(const Image& base, const Image& test, int minBase = 100, int minDrop = 30) {
    long long count = 0;
    const size_t n = std::min(base.Pixels.size(), test.Pixels.size());
    for (size_t i = 0; i < n; i += 3)
        if (base.Pixels[i] > minBase && base.Pixels[i] - test.Pixels[i] > minDrop) ++count;
    return count;
}

ShadowMap::CameraView ViewOf(const Shot& shot, float distance) {
    ShadowMap::CameraView v;
    v.Position = shot.Eye;
    v.Forward = glm::normalize(shot.Target - shot.Eye);
    v.Up = glm::vec3(0.0f, 1.0f, 0.0f);
    v.FovY = glm::radians(45.0f);
    v.Aspect = (float)kW / (float)kH;
    v.Near = 0.05f;
    v.ShadowDistance = distance;
    return v;
}

void MaybeDump(const char* name, const Image& image) {
    if (std::getenv("SAGE_DUMP_SHADOWS")) SavePng(name, image);
}

// --- 1. Отрыв тени (peter panning) -------------------------------------------
//
// Доска толщиной 5 см лежит на полу, солнце невысоко: её тень — полоса шириной
// около 11 см у дальнего края. Запас глубины в несколько текселей (а прежний
// доходил до десяти сантиметров при текселе в два) съедает эту полосу целиком:
// тонкие предметы — доски, ковры, ступени — переставали отбрасывать тень, а
// толстые отрывали её от основания. Тексель — 2 см, как у среднего каскада.
void TestThinCasterContact(FrameRenderer& r) {
    std::unique_ptr<Scene> scene = SunScene(SunFrom(25.0f, 0.0f));
    AddPrimitive(*scene, "Plank", MeshRef::Type::Cube, {0.0f, 0.025f, 0.0f}, {2.0f, 0.05f, 0.4f});
    const Shot shot{{0.0f, 1.1f, 1.9f}, {0.0f, 0.0f, 0.25f}};
    ShadowMap sm(2048, 1);
    sm.SetLightMatrix(scene->Lighting.Sun.Direction, glm::vec3(0.0f), 20.0f);
    const Image lit = RenderSun(r, *scene, sm, shot, false);
    const Image shadowed = RenderSun(r, *scene, sm, shot, true);
    MaybeDump("shadow_contact.png", shadowed);
    const long long strip = Darkened(lit, shadowed);
    std::printf("    тень доски толщиной 5 см (тексель %.1f см): %lld пикселей\n",
                sm.CascadeTexelSize(0) * 100.0f, strip);
    Check(strip > 900, "тонкий предмет на полу отбрасывает тень (нет отрыва от запаса глубины)");
}

// --- 2. Самозатенение (shadow acne) ------------------------------------------
//
// Ничто не заслоняет — значит, ни одного потемневшего пикселя. Проверяется на
// скользящем, среднем и высоком солнце: запас, рассчитанный «на глаз», обычно
// хорош на одном угле и рябит на другом. Отдельно — гладкий шар без пола: на
// нём нормаль вершины расходится с гранью, и плоскость приёмника по нормали
// ошибается сильнее всего.
void TestNoSelfShadowing(FrameRenderer& r) {
    long long worstFloor = 0, worstSphere = 0;
    for (float elevation : {10.0f, 35.0f, 70.0f}) {
        for (float azimuth : {20.0f, 135.0f}) {
            const glm::vec3 sun = SunFrom(elevation, azimuth);
            const Shot shot{{0.0f, 2.2f, 4.5f}, {0.0f, 0.3f, 0.0f}};
            {
                std::unique_ptr<Scene> scene = SunScene(sun);
                ShadowMap sm(2048, 3);
                sm.SetCascades(sun, ViewOf(shot, 60.0f));
                worstFloor = std::max(worstFloor, Darkened(RenderSun(r, *scene, sm, shot, false),
                                                           RenderSun(r, *scene, sm, shot, true), 60, 12));
            }
            {
                std::unique_ptr<Scene> scene = SunScene(sun, /*ground=*/false);
                AddPrimitive(*scene, "Ball", MeshRef::Type::Sphere, {0.0f, 0.5f, 0.0f}, glm::vec3(1.6f));
                ShadowMap sm(2048, 3);
                sm.SetCascades(sun, ViewOf(shot, 60.0f));
                const Image shadowed = RenderSun(r, *scene, sm, shot, true);
                if (elevation == 35.0f && azimuth == 20.0f) MaybeDump("shadow_sphere.png", shadowed);
                worstSphere = std::max(worstSphere, Darkened(RenderSun(r, *scene, sm, shot, false),
                                                             shadowed, 60, 12));
            }
        }
    }
    std::printf("    самозатенение: пол — до %lld пикселей, шар — до %lld\n", worstFloor, worstSphere);
    Check(worstFloor < 20, "открытый пол не затеняет сам себя ни при каком угле солнца");
    Check(worstSphere < 60, "гладкий шар не покрывается рябью самозатенения");
}

// --- 3. Зерно полутени --------------------------------------------------------
//
// Высокая стена, большое солнце: широкая полутень. Поперёк гладкой кромки
// яркость меняется монотонно; «зигзаг» (вверх-вниз-вверх у соседних пикселей)
// — это шум фильтра. Прежний диск с поворотом на пиксель давал его по всей
// полутени.
long long Zigzags(const Image& frame) {
    long long count = 0;
    for (int y = frame.Height / 3; y < frame.Height; ++y) {
        for (int x = 1; x + 1 < frame.Width; ++x) {
            const int a = frame.Pixels[(size_t)(y * frame.Width + x - 1) * 3];
            const int b = frame.Pixels[(size_t)(y * frame.Width + x) * 3];
            const int c = frame.Pixels[(size_t)(y * frame.Width + x + 1) * 3];
            if ((b - a) * (c - b) < 0 && std::abs(b - a) >= 3 && std::abs(c - b) >= 3) ++count;
        }
    }
    return count;
}

void TestPenumbraIsNotGrainy(FrameRenderer& r) {
    std::unique_ptr<Scene> scene = SunScene(SunFrom(40.0f, 25.0f));
    scene->Lighting.Shadows.SunAngle = 3.0f;
    AddPrimitive(*scene, "Wall", MeshRef::Type::Cube, {0.0f, 3.0f, 0.0f}, {3.0f, 6.0f, 0.3f});
    const Shot shot{{1.5f, 4.0f, 11.0f}, {0.0f, 0.0f, 5.0f}};
    ShadowMap sm(2048, 3);
    sm.SetCascades(scene->Lighting.Sun.Direction, ViewOf(shot, 60.0f));
    const Image lit = RenderSun(r, *scene, sm, shot, false);
    const Image shadowed = RenderSun(r, *scene, sm, shot, true);
    MaybeDump("shadow_penumbra.png", shadowed);
    const long long edge = Darkened(lit, shadowed, 60, 6);
    const long long zig = Zigzags(shadowed) - Zigzags(lit);
    std::printf("    полутень: затенено %lld пикселей, зигзагов яркости %lld\n", edge, zig);
    Check(edge > 2000, "стена отбрасывает тень (проверке есть что мерить)");
    Check(zig < 40, "полутень гладкая, без зерна");
}

// --- 4. Стабильность при переподгонке ------------------------------------------
//
// Камера сдвигается на миллиметры и поворачивается на десятые доли градуса, но
// кадр снимается с ПРЕЖНЕЙ точки: меняется только подгонка карт. С привязкой к
// сетке текселей содержимое карт сдвигается на целые тексели, и кадр обязан
// остаться тем же. Без неё тени «кипят» при ходьбе.
void TestShadowsStableUnderRefit(FrameRenderer& r) {
    std::unique_ptr<Scene> scene = SunScene(SunFrom(30.0f, 150.0f));
    for (int i = 0; i < 6; ++i)
        AddPrimitive(*scene, ("Post" + std::to_string(i)).c_str(), MeshRef::Type::Cube,
                     {-3.0f + 1.3f * (float)i, 1.0f, -2.0f - 4.0f * (float)i}, {0.3f, 2.0f, 0.3f});
    const Shot shot{{0.0f, 3.0f, 6.0f}, {0.0f, 0.0f, -10.0f}};
    ShadowMap sm(1024, 3);
    sm.SetCascades(scene->Lighting.Sun.Direction, ViewOf(shot, 80.0f));
    const Image base = RenderSun(r, *scene, sm, shot, true);
    long long worst = 0;
    for (int step = 1; step <= 6; ++step) {
        Shot moved = shot;
        moved.Eye += glm::vec3(0.0031f, 0.0012f, -0.0023f) * (float)step;
        ShadowMap::CameraView v = ViewOf(moved, 80.0f);
        const float yaw = glm::radians(0.11f) * (float)step;
        v.Forward = glm::vec3(glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0.0f, 1.0f, 0.0f)) *
                              glm::vec4(v.Forward, 0.0f));
        sm.SetCascades(scene->Lighting.Sun.Direction, v);
        const Image frame = RenderSun(r, *scene, sm, shot, true);
        long long changed = 0;
        for (size_t i = 0; i < base.Pixels.size(); i += 3)
            if (std::abs(base.Pixels[i] - frame.Pixels[i]) > 12) ++changed;
        worst = std::max(worst, changed);
    }
    std::printf("    переподгонка карт при движении камеры: изменилось до %lld пикселей\n", worst);
    Check(worst < 80, "тени не дрожат при сдвиге и повороте камеры");
}

// --- 5. Тень от каждого вида геометрии ----------------------------------------
//
// Инстансная статика, текстурная статика и скелетная модель рисуются в карту
// РАЗНЫМИ путями (см. RenderShadowDepth). Каждый обязан дать тень, а флаг
// «не отбрасывать тень» — работать у всех: у скелетной модели он раньше не
// читался вовсе.
long long CasterShadow(FrameRenderer& r, Scene& scene) {
    const Shot shot{{0.0f, 3.5f, 4.5f}, {0.0f, 0.0f, 0.0f}};
    ShadowMap sm(2048, 3);
    sm.SetCascades(scene.Lighting.Sun.Direction, ViewOf(shot, 40.0f));
    return Darkened(RenderSun(r, scene, sm, shot, false), RenderSun(r, scene, sm, shot, true));
}

void GiveTentacle(Scene& scene, glm::vec3 pos, bool castShadows) {
    GameObject obj = scene.CreateObject("Tentacle");
    obj.GetTransform().Position = pos;
    obj.GetTransform().Rotation = {0.0f, 0.0f, 90.0f}; // лёжа: тень на полу шире
    obj.Renderer().CastShadows = castShadows;
    AnimationComponent anim;
    anim.Playing = false;
    scene.Registry().emplace<AnimationComponent>(obj.Entity(), std::move(anim));
    AnimationComponent& am = scene.Registry().get<AnimationComponent>(obj.Entity());
    am.Model = sage::render::SkinnedModel::CreateDemoTentacle(6);
    am.Ready = true;
    am.ResolvedFrom.clear();
    if (am.Model) am.Anim.SetRig(&am.Model->GetSkeleton(), &am.Model->Clips());
    sage::anim::UpdateAnimators(scene, 0.0f);
}

void TestEveryCasterKind(FrameRenderer& r) {
    const glm::vec3 sun = SunFrom(55.0f, 30.0f);
    long long flat = 0, textured = 0, skinned = 0, skinnedOff = 0;
    {
        std::unique_ptr<Scene> scene = SunScene(sun);
        AddPrimitive(*scene, "Box", MeshRef::Type::Cube, {0.0f, 0.75f, 0.0f}, glm::vec3(0.6f, 1.5f, 0.6f));
        flat = CasterShadow(r, *scene);
    }
    {
        std::unique_ptr<Scene> scene = SunScene(sun);
        GameObject box = AddPrimitive(*scene, "Box", MeshRef::Type::Cube, {0.0f, 0.75f, 0.0f},
                                      glm::vec3(0.6f, 1.5f, 0.6f));
        std::vector<unsigned char> pixels(8 * 8 * 4, 200);
        auto material = std::make_shared<Material>();
        material->AlbedoTex = std::make_shared<Texture>(pixels.data(), 8, 8);
        box.Renderer().MaterialPtr = material;
        textured = CasterShadow(r, *scene);
    }
    {
        std::unique_ptr<Scene> scene = SunScene(sun);
        GiveTentacle(*scene, {0.0f, 0.4f, 0.0f}, true);
        skinned = CasterShadow(r, *scene);
    }
    {
        std::unique_ptr<Scene> scene = SunScene(sun);
        GiveTentacle(*scene, {0.0f, 0.4f, 0.0f}, false);
        skinnedOff = CasterShadow(r, *scene);
    }
    std::printf("    тень: инстансная %lld, текстурная %lld, скелетная %lld, скелетная без тени %lld\n",
                flat, textured, skinned, skinnedOff);
    Check(flat > 300, "инстансная статика отбрасывает тень");
    Check(textured > 300, "текстурная статика отбрасывает тень");
    Check(skinned > 150, "скелетная модель отбрасывает тень");
    Check(skinnedOff < 20, "скелетная модель с выключенной тенью её не отбрасывает");
}

// Сфера позы скелетной модели накрывает её при ЛЮБОМ движении костей: по ней
// модель отсекается в проходе теней, и промах означал бы пропавшую тень.
void TestPoseBoundsCoverMovedBones() {
    std::unique_ptr<sage::render::SkinnedModel> model = sage::render::SkinnedModel::CreateDemoTentacle(6);
    if (!model) {
        Check(false, "демо-модель для проверки габарита позы собрана");
        return;
    }
    glm::vec3 lo, hi;
    Check(model->BindBounds(lo, hi), "у демо-модели есть габарит");
    const int bones = (int)model->GetSkeleton().Count();
    std::vector<glm::mat4> pose((size_t)bones, glm::mat4(1.0f));
    // Кость уехала на шесть метров и повернулась: так выглядит корень клипа с
    // движением, который габарит позы привязки не накрывает.
    pose.back() = glm::translate(glm::mat4(1.0f), glm::vec3(6.0f, -2.0f, 3.0f)) *
                  glm::rotate(glm::mat4(1.0f), 1.2f, glm::vec3(0.3f, 1.0f, 0.2f));
    const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, -4.0f)) *
                            glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
    glm::vec3 center;
    float radius = 0.0f;
    Check(model->PoseBoundingSphere(world, pose, center, radius), "сфера позы посчитана");
    bool inside = true;
    for (const glm::mat4& b : pose)
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 corner((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
            const glm::vec3 p = glm::vec3(world * model->ImportTransform() * b * glm::vec4(corner, 1.0f));
            inside = inside && glm::length(p - center) <= radius * 1.0001f;
        }
    Check(inside, "сфера позы накрывает модель при сдвинутой кости");
}

// --- 6. Цена прохода теней ------------------------------------------------------
//
// Не проверка, а замер: две тысячи предметов, три каскада, проход целиком
// (сбор сцены, отсечение по каждому каскаду, отрисовка). Время — среднее по
// кадрам, вместе с ожиданием видеокарты (снимок кадра ждёт её).
void MeasureShadowPassCost(FrameRenderer& r) {
    std::unique_ptr<Scene> scene = SunScene(SunFrom(45.0f, 30.0f));
    for (int z = 0; z < 40; ++z)
        for (int x = 0; x < 50; ++x)
            AddPrimitive(*scene, "Crate", MeshRef::Type::Cube,
                         {-50.0f + 2.0f * (float)x, 0.5f, 5.0f - 3.0f * (float)z}, glm::vec3(1.0f));
    const Shot shot{{0.0f, 4.0f, 10.0f}, {0.0f, 0.0f, -20.0f}};
    ShadowMap sm(2048, 3);
    sm.SetCascades(scene->Lighting.Sun.Direction, ViewOf(shot, 120.0f));
    sage::render::RenderShadowDepth(sm, *scene, r.Batch, kW, kH); // прогрев
    const int frames = 10;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) sage::render::RenderShadowDepth(sm, *scene, r.Batch, kW, kH);
    (void)Capture(1, 1); // дождаться видеокарты
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / frames;
    std::printf("    проход теней (2000 предметов, 3 каскада): %.2f мс на кадр\n", ms);

    // Та же сцена в карты 64x64: растеризации почти нет, остаётся процессорная
    // часть — сбор сцены, отсечение по каскадам, раскладка и вызовы отрисовки.
    {
        ShadowMap tiny(64, 3);
        tiny.SetCascades(scene->Lighting.Sun.Direction, ViewOf(shot, 120.0f));
        sage::render::RenderShadowDepth(tiny, *scene, r.Batch, kW, kH);
        (void)Capture(1, 1);
        const auto c0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i) sage::render::RenderShadowDepth(tiny, *scene, r.Batch, kW, kH);
        (void)Capture(1, 1);
        const auto c1 = std::chrono::steady_clock::now();
        std::printf("    процессорная часть прохода теней (карты 64x64): %.2f мс на кадр\n",
                    std::chrono::duration<double, std::milli>(c1 - c0).count() / 20.0);
    }

    // Цена ВЫБОРКИ тени — цветной проход 1280x720 с тенями и без: разница и
    // есть стоимость фильтра на пиксель.
    const LightingEnvironment env = sage::ecs::CollectLighting(*scene);
    const glm::mat4 view = glm::lookAt(shot.Eye, shot.Target, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj = glm::perspective(glm::radians(45.0f), 16.0f / 9.0f, 0.05f, 300.0f);
    Framebuffer big(1280, 720);
    auto colorMs = [&](bool enabled) {
        const ShadowBinding binding(sm, enabled);
        big.Bind();
        r.Batch.RenderColor(*scene, view, proj, shot.Eye, env, binding, 0);
        (void)Capture(1, 1);
        // Лучшее из пяти серий: замер на общем процессоре шумит, а минимум
        // ближе всего к настоящей цене.
        double best = 1e9;
        for (int series = 0; series < 5; ++series) {
            const auto c0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 6; ++i) {
                big.Bind();
                sage::rhi::GraphicsDevice::Get().Clear(true, true);
                r.Batch.RenderColor(*scene, view, proj, shot.Eye, env, binding, 0);
            }
            (void)Capture(1, 1);
            const auto c1 = std::chrono::steady_clock::now();
            best = std::min(best, std::chrono::duration<double, std::milli>(c1 - c0).count() / 6.0);
        }
        return best;
    };
    const double without = colorMs(false), with = colorMs(true);
    std::printf("    цветной проход 1280x720: без теней %.2f мс, с тенями %.2f мс\n", without, with);

    // Снимки для глаз: тени на всех дальностях и разбор по каскадам.
    if (std::getenv("SAGE_DUMP_SHADOWS")) {
        for (int mode : {0, 10}) {
            big.Bind();
            sage::rhi::GraphicsDevice::Get().Clear(true, true);
            r.Batch.RenderColor(*scene, view, proj, shot.Eye, env, ShadowBinding(sm, true), mode);
            big.Resolve();
            big.Bind();
            SavePng(mode == 0 ? "shadow_field.png" : "shadow_field_cascades.png", Capture(1280, 720));
        }
    }
}

} // namespace

void RunShadowQualityChecks(FrameRenderer& r) {
    TestThinCasterContact(r);
    TestNoSelfShadowing(r);
    TestPenumbraIsNotGrainy(r);
    TestShadowsStableUnderRefit(r);
    TestEveryCasterKind(r);
    TestPoseBoundsCoverMovedBones();
    MeasureShadowPassCost(r);
}

} // namespace sage::rendertest
