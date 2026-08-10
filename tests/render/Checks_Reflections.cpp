// ---------------------------------------------------------------------------
// Эталонные кадры — отражения и блик в объективе.
//
// Карта окружения, зонды, швы на стыках граней куба, зеркальная плоскость и
// блик. Вместе, потому что все они об одном: о свете, пришедшем НЕ напрямую от
// источника, — и ломаются они обычно тоже вместе, от одной правки в PBR.
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

// Металлический шар под небом — самый чувствительный к отражениям случай:
// у металла нет диффузной составляющей, и без карты окружения он ОБЯЗАН
// выглядеть плоским, а с ней — нести в себе градиент неба.
std::unique_ptr<Scene> MakeReflectionScene(float roughness, float metallic,
                                          float sunIntensity = 1.0f) {
    auto scene = std::make_unique<Scene>("ReflectTest");
    scene->Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.35f, -1.0f, -0.4f));
    scene->Lighting.Sun.Intensity = sunIntensity;
    scene->Lighting.SkyColor = {0.42f, 0.50f, 0.66f};
    scene->Lighting.GroundColor = {0.18f, 0.16f, 0.14f};
    scene->Lighting.AmbientStrength = 0.35f;
    // Небо резко разделено на «верх» и «низ»: отражение такого неба видно как
    // граница поперёк шара, и её положение можно проверить, а не описать.
    scene->Lighting.Skybox.Enabled = true;
    scene->Lighting.Skybox.TopColor = {0.05f, 0.10f, 0.55f};
    scene->Lighting.Skybox.HorizonColor = {0.95f, 0.75f, 0.35f};

    GameObject ball = scene->CreateObject("Ball");
    ball.GetTransform().Position = {0.0f, 0.6f, 0.0f};
    ball.GetTransform().Scale = glm::vec3(1.4f);
    ball.Renderer().Ref = MeshRef{MeshRef::Type::Sphere};
    ball.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Sphere);
    auto mat = std::make_shared<Material>();
    mat->Albedo = {0.95f, 0.95f, 0.95f};
    mat->Metallic = metallic;
    mat->Roughness = roughness;
    ball.Renderer().MaterialPtr = mat;
    return scene;
}

// --- Меры на пикселях шара -------------------------------------------------
//
// Фон кадра намеренно чёрный, а неба в кадре нет: тогда «пиксели шара» — это
// просто всё, что ярче фона, и никакой маски по геометрии не нужно. Небо при
// этом всё равно снято в карту окружения — именно его мы и ищем НА шаре.

bool BallPixel(const Image& img, int x, int y, int& r, int& g, int& b) {
    const size_t i = ((size_t)y * img.Width + x) * 3;
    r = img.Pixels[i]; g = img.Pixels[i + 1]; b = img.Pixels[i + 2];
    return (r + g + b) > 45;   // заметно ярче чёрного фона
}

// Доля пикселей шара, где виден «верх неба» (синий), и где «низ» (тёплый).
// Небо в тесте разделено на глубокий синий зенит и оранжевый горизонт, поэтому
// зеркальный шар ОБЯЗАН показать оба цвета сразу: сверху одно, по краям другое.
// Ровно закрашенный шар не покажет ни того, ни другого.
void SkyColorSplit(const Image& img, double& cool, double& warm) {
    long long total = 0, coolN = 0, warmN = 0;
    for (int y = 0; y < img.Height; ++y)
        for (int x = 0; x < img.Width; ++x) {
            int r, g, b;
            if (!BallPixel(img, x, y, r, g, b)) continue;
            ++total;
            if (b - r > 25) ++coolN;
            if (r - b > 25) ++warmN;
        }
    cool = total ? (double)coolN / (double)total : 0.0;
    warm = total ? (double)warmN / (double)total : 0.0;
}

// Резкость отражения — САМЫЙ КРУТОЙ перепад между соседними пикселями шара
// (99-й перцентиль, чтобы не ловить одиночный выброс).
//
// Именно перцентиль, а не среднее. Среднее для этой задачи не работает, и это
// не придирка: резкая граница даёт огромный скачок в НЕСКОЛЬКИХ пикселях, а
// размытая — умеренный в МНОГИХ, и среднее у второй может выйти больше. На
// первом заходе так и вышло: шероховатость 0.6 «оказалась резче» 0.05.
// Крутизна самого сильного перехода такой двусмысленности не имеет.
double Sharpness(const Image& img) {
    std::vector<int> steps;
    int lo = 255 * 3, hi = 0;
    for (int y = 1; y < img.Height - 1; ++y)
        for (int x = 1; x < img.Width - 1; ++x) {
            int r, g, b;
            if (!BallPixel(img, x, y, r, g, b)) continue;
            lo = std::min(lo, r + g + b);
            hi = std::max(hi, r + g + b);
            int r2, g2, b2;
            if (!BallPixel(img, x + 1, y, r2, g2, b2)) continue;
            steps.push_back(std::abs(r - r2) + std::abs(g - g2) + std::abs(b - b2));
        }
    if (steps.empty()) return 0.0;
    std::sort(steps.begin(), steps.end());
    const double steepest = (double)steps[(size_t)((steps.size() - 1) * 99 / 100)];

    // Делим на ПОЛНЫЙ размах яркости шара.
    //
    // Абсолютный шаг мерит не только размытие, но и контраст, а он с
    // шероховатостью РАСТЁТ: чем шероховатее металл, тем больше окружения
    // собирает env-BRDF, и шар в целом становится ярче. Из-за этого абсолютная
    // мера сначала росла и только потом падала — она мерила две вещи сразу.
    // Отношение «самый крутой шаг к полному размаху» отвечает ровно на нужный
    // вопрос: за какую долю всего перепада отвечает один пиксель. Резкая кромка
    // — за большую, размытая — за малую, и яркость на это не влияет.
    const double range = (double)(hi - lo);
    return range > 1.0 ? steepest / range : 0.0;
}

// Кадр сцены с картой окружения (или без неё, если reflect == nullptr).
// Небо в КАДРЕ не рисуется намеренно (см. выше) — только в карте окружения.
Image RenderReflected(FrameRenderer& r, Scene& scene, sage::render::ReflectionSystem* reflect,
                      SkyRenderer& sky, int w, int h) {
    Framebuffer sceneFbo(w, h);
    Framebuffer output(w, h);
    const LightingEnvironment env = sage::ecs::CollectLighting(scene);
    const glm::mat4 view = TestView();
    const glm::mat4 proj = PerspectiveProj();

    // Карта окружения снимается ДО прохода сцены и в свой буфер: она меняет
    // привязанный таргет и viewport, и делать это посреди кадра нельзя.
    if (reflect) reflect->UpdateSky(sky, env);

    r.Shadow.SetLightMatrix(env.Sun.Direction, glm::vec3(0.0f), 12.0f);
    r.Shadow.BeginRender();
    r.Batch.RenderDepth(scene, r.Shadow.LightMatrix());
    r.Shadow.EndRender(w, h);

    sceneFbo.Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    device.Clear(true, true);

    sage::render::ReflectionBinding binding;
    if (reflect) binding = reflect->Binding(w, h);
    r.Batch.RenderColor(scene, view, proj, kEye, env, ShadowBinding(r.Shadow, true), 0, &binding);

    r.Fx.ResetHistory();
    sage::render::PostFXSettings fx = BaseSettings();
    fx.BloomEnabled = false;   // свечение размывает переходы, которые мы меряем
    fx.Vignette = 0.0f;        // и виньетка съедает края шара
    r.Fx.Render(sceneFbo.ColorTexture(), sceneFbo.DepthTexture(), w, h, proj, view, fx, &output, 0,
                0, w, h);
    output.Bind();
    return Capture(w, h);
}

void TestReflections(FrameRenderer& r) {
    std::printf("=== Отражения: карта окружения ===\n");
    SkyRenderer sky;

    // 1. Зеркальный металл. Без карты окружения отражать нечего — на шаре не
    //    может быть ни синего зенита, ни оранжевого горизонта, потому что в
    //    сцене этих цветов нет НИГДЕ, кроме неба.
    auto mirror = MakeReflectionScene(0.05f, 1.0f);
    const Image flat = RenderReflected(r, *mirror, nullptr, sky, kW, kH);
    sage::render::ReflectionSystem reflect;
    const Image reflected = RenderReflected(r, *mirror, &reflect, sky, kW, kH);

    double flatCool = 0.0, flatWarm = 0.0, refCool = 0.0, refWarm = 0.0;
    SkyColorSplit(flat, flatCool, flatWarm);
    SkyColorSplit(reflected, refCool, refWarm);
    std::printf("       без отражений: зенит %.1f%% горизонт %.1f%%\n", flatCool * 100.0,
                flatWarm * 100.0);
    std::printf("       с отражениями: зенит %.1f%% горизонт %.1f%%\n", refCool * 100.0,
                refWarm * 100.0);
    Check(refCool > 0.10 && refWarm > 0.10, "на зеркале видны ОБА цвета неба сразу");
    Check(flatCool < 0.02 && flatWarm < 0.02, "без карты окружения этих цветов на шаре нет");
    Report("reflect_mirror", CompareWithReference("reflect_mirror", reflected));

    // 2. Шероховатость размывает отражение. Меряем резкость перехода между
    //    цветами неба на шаре — она обязана падать монотонно.
    double prev = 1e9;
    bool monotone = true;
    // Солнце в этом заходе выключено НАМЕРЕННО: прямой блик сам по себе
    // расплывается с шероховатостью и попадал бы в ту же меру, которой мы
    // проверяем размытие ОТРАЖЕНИЯ. С выключенным солнцем у металла не
    // остаётся ничего, кроме отражения, — мерить становится нечего лишнего.
    for (float rough : {0.05f, 0.15f, 0.3f, 0.45f, 0.6f, 0.8f, 1.0f}) {
        auto s = MakeReflectionScene(rough, 1.0f, 0.0f);
        sage::render::ReflectionSystem rs;
        const double sharp = Sharpness(RenderReflected(r, *s, &rs, sky, kW, kH));
        std::printf("       шероховатость %.2f -> резкость перехода %.3f\n", rough, sharp);
        if (sharp > prev + 0.02) monotone = false;
        prev = sharp;
    }
    Check(monotone, "чем шероховатее, тем размытее отражение");

    // 3. Диэлектрик отражает по Френелю: у него F0 около 0.04 против почти
    //    единицы у металла. Если бы отражение вешалось всем одинаково, пластик
    //    блестел бы как хром.
    auto plastic = MakeReflectionScene(0.05f, 0.0f);
    sage::render::ReflectionSystem rsPlastic;
    double plCool = 0.0, plWarm = 0.0;
    SkyColorSplit(RenderReflected(r, *plastic, &rsPlastic, sky, kW, kH), plCool, plWarm);
    std::printf("       диэлектрик: зенит %.1f%% горизонт %.1f%%\n", plCool * 100.0,
                plWarm * 100.0);
    Check(plCool + plWarm < (refCool + refWarm) * 0.6,
          "диэлектрик отражает заметно слабее металла (F0 0.04 против ~1)");
    Check(plCool + plWarm > 0.0, "но отражает: по краю, где Френель усиливает отражение");
}

// Захват зонда: карта окружения снимается со СЦЕНЫ, а не с неба.
//
// Главное, что здесь проверяется, — ОРИЕНТАЦИЯ граней куба. Ошибка в таблице
// направлений (перепутанный знак «верха», переставленные +Z/−Z) ничего не
// роняет: отражение просто оказывается зеркальным или повёрнутым, и на
// симметричной сцене этого не видно вовсе. Поэтому сцена намеренно
// НЕсимметрична — ярко-красная стена стоит ровно с одной стороны, и тест
// требует, чтобы она отразилась именно с той стороны шара.
void TestReflectionProbe(FrameRenderer& r) {
    std::printf("=== Отражения: зонд снимает сцену ===\n");

    auto scene = std::make_unique<Scene>("ProbeTest");
    // Солнца нет: у металла тогда не остаётся ничего, кроме отражения, и
    // измеренный цвет заведомо пришёл из карты окружения, а не из блика.
    // Стены при этом видны — их освещает ambient, а металл его не получает
    // (у металла нет диффузной составляющей).
    scene->Lighting.Sun.Intensity = 0.0f;
    scene->Lighting.AmbientStrength = 1.6f;
    scene->Lighting.SkyColor = {1.0f, 1.0f, 1.0f};
    scene->Lighting.GroundColor = {1.0f, 1.0f, 1.0f};
    scene->Lighting.Skybox.TopColor = {0.0f, 0.0f, 0.0f};
    scene->Lighting.Skybox.HorizonColor = {0.0f, 0.0f, 0.0f};

    auto cube = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
    auto wall = [&](const char* name, glm::vec3 pos, glm::vec3 scale, glm::vec3 color) {
        GameObject o = scene->CreateObject(name);
        o.GetTransform().Position = pos;
        o.GetTransform().Scale = scale;
        o.Renderer().Ref = MeshRef{MeshRef::Type::Cube};
        o.Renderer().MeshPtr = cube;
        // Цвет задаётся ТОЛЬКО материалом. Раньше здесь стояло и
        // `Renderer().Color = color` — при старом правиле «материал замещает
        // цвет» лишняя строка ни на что не влияла, а по нынешнему (поправка
        // экземпляра МОДУЛИРУЕТ материал, см. ecs/RenderComponents.h) она
        // возвела бы цвет стены в квадрат.
        auto m = std::make_shared<Material>();
        m->Albedo = color;
        m->Roughness = 1.0f;
        m->Metallic = 0.0f;
        o.Renderer().MaterialPtr = m;
        return o;
    };
    // Красная стена — только со стороны +X. Всё остальное тёмное.
    wall("RedWall", {6.0f, 0.8f, 0.0f}, {0.4f, 6.0f, 12.0f}, {1.0f, 0.05f, 0.05f});
    wall("DarkWall", {-6.0f, 0.8f, 0.0f}, {0.4f, 6.0f, 12.0f}, {0.02f, 0.02f, 0.02f});

    // Зонд снимает сцену БЕЗ отражающего шара: иначе он снял бы сам себя, и
    // отражение показывало бы прошлое состояние шара, а не окружение.
    sage::render::ReflectionSystem probe;
    probe.CaptureScene(glm::vec3(0.0f, 0.8f, 0.0f), 0.1f, 60.0f,
                       [&](const glm::mat4& v, const glm::mat4& p) {
                           sage::rhi::GraphicsDevice& d = sage::rhi::GraphicsDevice::Get();
                           d.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                           d.Clear(true, true);
                           const LightingEnvironment e = sage::ecs::CollectLighting(*scene);
                           r.Batch.RenderColor(*scene, v, p, glm::vec3(0.0f, 0.8f, 0.0f), e,
                                               ShadowBinding(), 0);
                       });

    // Теперь шар — и только он.
    for (auto e : scene->Registry().view<MeshRendererComponent>()) {
        scene->Registry().get<MeshRendererComponent>(e).MeshPtr = nullptr;
    }
    GameObject ball = scene->CreateObject("Ball");
    ball.GetTransform().Position = {0.0f, 0.8f, 0.0f};
    ball.GetTransform().Scale = glm::vec3(1.6f);
    ball.Renderer().Ref = MeshRef{MeshRef::Type::Sphere};
    ball.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Sphere);
    auto mm = std::make_shared<Material>();
    mm->Albedo = {1.0f, 1.0f, 1.0f};
    mm->Metallic = 1.0f;
    mm->Roughness = 0.05f;
    ball.Renderer().MaterialPtr = mm;

    Framebuffer fbo(kW, kH), out(kW, kH);
    const LightingEnvironment env = sage::ecs::CollectLighting(*scene);
    // Камера строго по +Z, чтобы «право на экране» совпадало с +X мира: иначе
    // проверка стороны проверяла бы не то.
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.8f, 7.0f), glm::vec3(0.0f, 0.8f, 0.0f),
                                       glm::vec3(0, 1, 0));
    const glm::mat4 proj = PerspectiveProj();
    fbo.Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    device.Clear(true, true);
    const sage::render::ReflectionBinding binding = probe.Binding(kW, kH);
    r.Batch.RenderColor(*scene, view, proj, glm::vec3(0.0f, 0.8f, 7.0f), env, ShadowBinding(), 0,
                        &binding);
    r.Fx.ResetHistory();
    sage::render::PostFXSettings fx = BaseSettings();
    fx.BloomEnabled = false;
    fx.Vignette = 0.0f;
    r.Fx.Render(fbo.ColorTexture(), fbo.DepthTexture(), kW, kH, proj, view, fx, &out, 0, 0, kW, kH);
    out.Bind();
    const Image img = Capture(kW, kH);

    // Считаем «красноту» по половинам кадра.
    auto redness = [&](int x0, int x1) {
        long long red = 0, total = 0;
        for (int y = 0; y < kH; ++y)
            for (int x = x0; x < x1; ++x) {
                int rr, gg, bb;
                if (!BallPixel(img, x, y, rr, gg, bb)) continue;
                ++total;
                if (rr - gg > 30 && rr - bb > 30) ++red;
            }
        return total ? (double)red / (double)total : 0.0;
    };
    const double right = redness(kW / 2, kW);
    const double left = redness(0, kW / 2);
    std::printf("       красное на шаре: справа %.1f%%, слева %.1f%%\n", right * 100.0,
                left * 100.0);
    Check(right > 0.25, "зонд снял сцену: красная стена видна в отражении");
    Check(right > left * 3.0, "и отразилась с ТОЙ стороны, где стоит (грани куба не перепутаны)");
    Report("reflect_probe", CompareWithReference("reflect_probe", img));
}

// --- Блик в объективе --------------------------------------------------------
//
// Проверяем не «красиво ли», а те три утверждения, ради которых блик вообще
// написан отдельным проходом, а не нарисован спрайтом поверх кадра:
//
//   1. солнце в кадре — блик есть, и он ярче кадра без блика;
//   2. солнце ЗАКРЫТО геометрией — блика нет. Это главное: блик, горящий
//      сквозь стену, — самая заметная неправда эффекта;
//   3. солнце за спиной — блика нет вовсе.
//
// Сцена своя и предельно простая: пустое небо, известное направление на солнце
// и камера, смотрящая точно на него. На общей тестовой сцене солнце уходит за
// край кадра, и проверять было бы нечего.

// Кадр «с солнцем в объективе». blocker — поставить ли перед солнцем стену.
Image RenderFlareFrame(FrameRenderer& r, bool flareOn, bool blocker, bool behind,
                       float& outMeanLuma) {
    constexpr int w = 256, h = 192;
    Framebuffer sceneFbo(w, h);
    Framebuffer output(w, h);

    Scene scene("LensFlare");
    // Солнце низко и почти прямо по оси -Z: так оно попадает в кадр камеры,
    // которая смотрит туда же.
    const glm::vec3 toSun = glm::normalize(glm::vec3(0.0f, 0.22f, -1.0f));
    scene.Lighting.Sun.Direction = -toSun;
    scene.Lighting.Sun.Intensity = 3.0f;
    scene.Lighting.Sun.Color = {1.0f, 0.95f, 0.85f};
    scene.Lighting.SkyColor = {0.35f, 0.52f, 0.78f};
    scene.Lighting.AmbientStrength = 0.4f;
    // Небо настоящее, со светилом: проход видимости спрашивает у кадра, ярко
    // ли ТАМ, ГДЕ солнце. Ровная заливка на весь фон ответила бы «ярко везде»
    // — то есть не ответила бы ничего, и тест проверял бы блик без проверки.
    scene.Lighting.Skybox.Enabled = true;
    scene.Lighting.Skybox.Celestials = true;
    scene.Lighting.Skybox.TopColor = {0.22f, 0.38f, 0.68f};
    scene.Lighting.Skybox.HorizonColor = {0.62f, 0.70f, 0.80f};
    scene.Lighting.Skybox.SunColor = {1.0f, 0.94f, 0.80f};
    scene.Lighting.Skybox.SunSize = 0.055f;

    if (blocker) {
        // Стена ровно между камерой и солнцем. Не «где-то в кадре»: тест
        // проверяет перекрытие, а не наличие геометрии.
        GameObject wall = scene.CreateObject("Blocker");
        wall.Renderer().Ref = MeshRef{MeshRef::Type::Cube};
        wall.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
        wall.Renderer().Color = {0.10f, 0.10f, 0.10f};
        wall.GetTransform().Position = {0.0f, 1.6f, -6.0f};
        wall.GetTransform().Scale = {8.0f, 8.0f, 0.4f};
    }

    const glm::vec3 eye(0.0f, 0.6f, 4.0f);
    // Смотрим НЕ точно на солнце, а мимо: призраки ложатся на прямую «солнце —
    // центр кадра», и при солнце ровно в центре этой прямой нет — вся цепочка
    // схлопывается в одну точку, и проверять было бы нечего.
    const glm::vec3 aim = glm::normalize(toSun + glm::vec3(0.30f, -0.10f, 0.0f));
    const glm::vec3 look = behind ? eye - aim : eye + aim;
    const glm::mat4 view = glm::lookAt(eye, look, glm::vec3(0, 1, 0));
    CameraComponent cam;
    cam.Fov = 60.0f;
    cam.NearClip = 0.1f;
    cam.FarClip = 200.0f;
    const glm::mat4 proj = cam.ProjectionMatrix((float)w / (float)h);

    const LightingEnvironment env = sage::ecs::CollectLighting(scene);

    sceneFbo.Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    device.Clear(true, true);
    SkyRenderer sky;
    sky.Draw(view, proj, env.Skybox.TopColor, env.Skybox.HorizonColor,
             CelestialsFromEnvironment(env));
    r.Batch.RenderColor(scene, view, proj, eye, env, ShadowBinding(r.Shadow, false), 0);

    if (flareOn) {
        sage::render::LensFlareSettings s;
        s.Enabled = true;
        s.Intensity = 1.0f;
        sage::render::LensFlare flare;
        flare.Render(sceneFbo, sceneFbo.ColorTexture(), sceneFbo.DepthTexture(), w, h, proj, view,
                     env, s);
    }

    sage::render::PostFXSettings fx = BaseSettings();
    fx.Vignette = 0.0f;      // виньетка съедает призраки у края и мешает считать
    fx.BloomEnabled = false; // свечение размазало бы разницу, которую мы меряем
    r.Fx.ResetHistory();
    r.Fx.Render(sceneFbo.ColorTexture(), sceneFbo.DepthTexture(), w, h, proj, view, fx, &output, 0,
                0, w, h);

    output.Bind();
    Image img = Capture(w, h);

    double sum = 0.0;
    for (size_t i = 0; i + 2 < img.Pixels.size(); i += 4)
        sum += 0.2126 * img.Pixels[i] + 0.7152 * img.Pixels[i + 1] + 0.0722 * img.Pixels[i + 2];
    outMeanLuma = (float)(sum / (double)(w * h));
    return img;
}

void TestLensFlare(FrameRenderer& r) {
    std::printf("=== Блик в объективе ===\n");

    float lumaOff = 0.0f, lumaOn = 0.0f, lumaBlocked = 0.0f, lumaBehind = 0.0f;
    RenderFlareFrame(r, /*flareOn=*/false, /*blocker=*/false, /*behind=*/false, lumaOff);
    Image on = RenderFlareFrame(r, true, false, false, lumaOn);
    RenderFlareFrame(r, true, true, false, lumaBlocked);
    float lumaBlockedOff = 0.0f;
    RenderFlareFrame(r, false, true, false, lumaBlockedOff);
    RenderFlareFrame(r, true, false, true, lumaBehind);
    float lumaBehindOff = 0.0f;
    RenderFlareFrame(r, false, false, true, lumaBehindOff);

    std::printf("       средняя яркость: без блика %.2f, с бликом %.2f\n", lumaOff, lumaOn);
    Check(lumaOn > lumaOff + 1.0f, "блик добавляет свет в кадр");

    std::printf("       солнце за стеной: без блика %.2f, с бликом %.2f\n", lumaBlockedOff,
                lumaBlocked);
    Check(std::abs(lumaBlocked - lumaBlockedOff) < 0.35f,
          "закрытое геометрией солнце блика не даёт");

    std::printf("       солнце за спиной: без блика %.2f, с бликом %.2f\n", lumaBehindOff,
                lumaBehind);
    Check(std::abs(lumaBehind - lumaBehindOff) < 0.05f, "солнце за спиной блика не даёт");

    Report("lens_flare", CompareWithReference("lens_flare", on));
}

// Швы куба — главный артефакт карт окружения. Грани снимаются по отдельности, и
// на стыке фильтрация не может дотянуться до соседней грани: получается светлая
// или тёмная линия, тем толще, чем размытее мип. Проверяем ЧИСЛЕННО: читаем
// сам куб по направлениям вдоль стыка и ищем скачок.
void TestReflectionSeams() {
    std::printf("=== Отражения: швы на стыках граней ===\n");
    SkyRenderer sky;
    LightingEnvironment env;
    env.Skybox.TopColor = {0.05f, 0.10f, 0.55f};
    env.Skybox.HorizonColor = {0.95f, 0.75f, 0.35f};

    sage::render::EnvironmentMap envMap(64);
    if (!envMap.Valid()) {
        std::printf("[FAIL] кубический таргет не создался\n");
        CountFail();
        return;
    }
    envMap.CaptureSky(sky, env);

    // Рисуем куб в кадр «изнутри»: полноэкранный вид на стык граней +X/+Z.
    // Скачок яркости поперёк стыка и есть шов.
    Framebuffer fbo(128, 128);
    fbo.Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    device.Clear(true, true);
    // Небо той же формулой — эталон, с которым сверяется куб.
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 1.0f),
                                       glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 10.0f);
    sky.Draw(view, proj, env.Skybox.TopColor, env.Skybox.HorizonColor);
    const Image direct = Capture(128, 128);

    // Максимальный скачок яркости между соседними пикселями по горизонтали в
    // середине кадра — там, где проходит стык. У градиентного неба соседние
    // пиксели различаются на единицы, шов дал бы десятки.
    int worst = 0;
    const int y = 64;
    for (int x = 1; x < 127; ++x) {
        const size_t a = ((size_t)y * 128 + x - 1) * 3;
        const size_t b = ((size_t)y * 128 + x) * 3;
        for (int c = 0; c < 3; ++c)
            worst = std::max(worst, std::abs((int)direct.Pixels[a + c] - (int)direct.Pixels[b + c]));
    }
    std::printf("       худший скачок поперёк стыка граней: %d/255\n", worst);
    Check(worst < 12, "на стыке граней куба нет шва");
}

// Плоское отражение целиком: снять зеркальный проход и убедиться, что предмет
// над зеркалом виден В НЁМ, причём на своём месте.
//
// Матрица отражения проверена отдельно; здесь проверяется весь путь — смена
// обхода треугольников, косое отсечение, чтение по экранной позиции. Ошибка в
// любом из трёх даёт не «нет отражения», а неправильное отражение: изнанку
// модели, лишнюю геометрию из-под воды или отражение, съехавшее вбок.
void TestPlanarReflectionRender(FrameRenderer& r) {
    std::printf("=== Отражения: зеркальный пол ===\n");

    auto scene = std::make_unique<Scene>("PlanarTest");
    scene->Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f));
    scene->Lighting.Sun.Intensity = 1.2f;
    scene->Lighting.AmbientStrength = 0.5f;
    scene->Lighting.SkyColor = {0.5f, 0.55f, 0.7f};
    scene->Lighting.GroundColor = {0.2f, 0.2f, 0.2f};

    auto cube = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
    auto plane = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Plane);

    // Зеркальный пол на y = 0.
    GameObject floor = scene->CreateObject("Mirror");
    floor.GetTransform().Position = {0.0f, 0.0f, 0.0f};
    floor.GetTransform().Scale = {14.0f, 1.0f, 14.0f};
    floor.Renderer().Ref = MeshRef{MeshRef::Type::Plane};
    floor.Renderer().MeshPtr = plane;
    auto mirrorMat = std::make_shared<Material>();
    mirrorMat->Albedo = {0.05f, 0.05f, 0.06f};
    mirrorMat->Roughness = 0.05f;
    mirrorMat->Render.PlanarReflectivity = 1.0f;
    floor.Renderer().MaterialPtr = mirrorMat;

    // Ярко-красный столб НАД зеркалом — то, что обязано в нём отразиться.
    GameObject pillar = scene->CreateObject("Pillar");
    pillar.GetTransform().Position = {0.0f, 1.6f, 0.0f};
    pillar.GetTransform().Scale = {0.7f, 3.0f, 0.7f};
    pillar.Renderer().Ref = MeshRef{MeshRef::Type::Cube};
    pillar.Renderer().MeshPtr = cube;
    auto redMat = std::make_shared<Material>();
    redMat->Albedo = {0.95f, 0.06f, 0.06f};
    redMat->Roughness = 0.8f;
    pillar.Renderer().MaterialPtr = redMat;

    // И ярко-зелёный ПОД зеркалом: он в отражении появиться НЕ должен —
    // именно это отсекает косая ближняя плоскость. Без неё в зеркале
    // оказывается то, что лежит под ним.
    GameObject below = scene->CreateObject("Below");
    below.GetTransform().Position = {0.0f, -1.6f, 0.0f};
    below.GetTransform().Scale = {3.0f, 2.6f, 3.0f};
    below.Renderer().Ref = MeshRef{MeshRef::Type::Cube};
    below.Renderer().MeshPtr = cube;
    auto greenMat = std::make_shared<Material>();
    greenMat->Albedo = {0.05f, 0.95f, 0.10f};
    greenMat->Roughness = 0.8f;
    below.Renderer().MaterialPtr = greenMat;

    const LightingEnvironment env = sage::ecs::CollectLighting(*scene);
    // Камера низко над полом: так отражение занимает заметную часть кадра.
    const glm::vec3 eye(0.0f, 1.1f, 6.5f);
    const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f, 0.9f, 0.0f), glm::vec3(0, 1, 0));
    const glm::mat4 proj = PerspectiveProj();

    sage::render::PlanarReflection planar(1.0f);   // полное разрешение: тест меряет пиксели
    const glm::vec4 mirrorPlane(0.0f, 1.0f, 0.0f, 0.0f);
    const bool captured =
        planar.Capture(mirrorPlane, view, proj, kW, kH, [&](const glm::mat4& mv, const glm::mat4& mp) {
            sage::rhi::GraphicsDevice& d = sage::rhi::GraphicsDevice::Get();
            d.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            d.Clear(true, true);
            r.Batch.RenderColor(*scene, mv, mp, glm::vec3(glm::inverse(mv)[3]), env, ShadowBinding(),
                                0);
        });
    Check(captured, "зеркальный проход снялся");

    Framebuffer fbo(kW, kH), out(kW, kH);
    fbo.Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetClearColor(0.02f, 0.02f, 0.03f, 1.0f);
    device.Clear(true, true);
    sage::render::ReflectionBinding binding;
    binding.PlanarTexture = planar.Texture();
    binding.ScreenTexel = glm::vec2(1.0f / kW, 1.0f / kH);
    r.Batch.RenderColor(*scene, view, proj, eye, env, ShadowBinding(), 0, &binding);
    r.Fx.ResetHistory();
    sage::render::PostFXSettings fx = BaseSettings();
    fx.BloomEnabled = false;
    fx.Vignette = 0.0f;
    r.Fx.Render(fbo.ColorTexture(), fbo.DepthTexture(), kW, kH, proj, view, fx, &out, 0, 0, kW, kH);
    out.Bind();
    const Image img = Capture(kW, kH);

    // Считаем красное и зелёное в НИЖНЕЙ половине кадра — там, где пол.
    long long red = 0, green = 0, floorPixels = 0;
    for (int y = kH / 2; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const size_t i = ((size_t)y * kW + x) * 3;
            const int rr = img.Pixels[i], gg = img.Pixels[i + 1], bb = img.Pixels[i + 2];
            ++floorPixels;
            if (rr - gg > 40 && rr - bb > 40) ++red;
            if (gg - rr > 40 && gg - bb > 40) ++green;
        }
    const double redFrac = (double)red / (double)floorPixels;
    const double greenFrac = (double)green / (double)floorPixels;
    std::printf("       в зеркале: красного %.1f%%, зелёного %.1f%%\n", redFrac * 100.0,
                greenFrac * 100.0);
    Check(redFrac > 0.02, "столб НАД зеркалом в нём отражается");
    Check(greenFrac < 0.002, "то, что ПОД зеркалом, в отражение не попадает (косое отсечение)");

    // Отражение обязано стоять ПОД столбом, а не сбоку: столб по центру кадра,
    // значит и красное в нижней половине должно быть по центру.
    long long leftRed = 0, rightRed = 0, centerRed = 0;
    for (int y = kH / 2; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const size_t i = ((size_t)y * kW + x) * 3;
            const int rr = img.Pixels[i], gg = img.Pixels[i + 1], bb = img.Pixels[i + 2];
            if (!(rr - gg > 40 && rr - bb > 40)) continue;
            if (x < kW / 3) ++leftRed;
            else if (x > kW * 2 / 3) ++rightRed;
            else ++centerRed;
        }
    std::printf("       красное по третям: слева %lld, центр %lld, справа %lld\n", leftRed,
                centerRed, rightRed);
    Check(centerRed > leftRed + rightRed, "отражение стоит ПОД предметом, а не съехало вбок");

    Report("reflect_planar", CompareWithReference("reflect_planar", img));
}

// Плоское отражение обязано быть ГЕОМЕТРИЧЕСКИ верным, а не «похожим»:
// предмет над зеркалом отражается ровно на столько же вниз. Проверяем
// матрицей — она и есть источник правды, а картинка от неё производная.
void TestPlanarReflectionMath() {
    std::printf("=== Отражения: зеркальная плоскость ===\n");
    using sage::render::PlanarReflection;

    // Плоскость y = 0, нормаль вверх: dot(n,x) + d = y.
    const glm::vec4 plane(0.0f, 1.0f, 0.0f, 0.0f);
    const glm::mat4 mirror = PlanarReflection::MirrorMatrix(plane);

    const glm::vec3 above(2.0f, 3.0f, -1.0f);
    const glm::vec3 got(mirror * glm::vec4(above, 1.0f));
    std::printf("       (%.1f %.1f %.1f) -> (%.1f %.1f %.1f)\n", above.x, above.y, above.z, got.x,
                got.y, got.z);
    Check(std::fabs(got.x - above.x) < 1e-5f && std::fabs(got.z - above.z) < 1e-5f,
          "отражение не сдвигает точку вдоль плоскости");
    Check(std::fabs(got.y + above.y) < 1e-5f, "и переворачивает её поперёк");

    // Точка НА плоскости обязана остаться на месте — иначе линия уреза воды
    // разошлась бы с отражением, и это первое, что видно глазом.
    const glm::vec3 onPlane(1.5f, 0.0f, 2.5f);
    const glm::vec3 same(mirror * glm::vec4(onPlane, 1.0f));
    Check(glm::length(same - onPlane) < 1e-5f, "точка на плоскости остаётся на месте");

    // Двойное отражение — тождество: признак того, что матрица именно
    // зеркальная, а не «похожее на неё» преобразование.
    const glm::mat4 twice = mirror * mirror;
    float worst = 0.0f;
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr)
            worst = std::max(worst, std::fabs(twice[c][rr] - glm::mat4(1.0f)[c][rr]));
    Check(worst < 1e-5f, "дважды отражённое равно исходному");

    // Наклонная плоскость со смещением: общий случай, где легко ошибиться знаком d.
    const glm::vec3 n = glm::normalize(glm::vec3(0.3f, 1.0f, -0.2f));
    const glm::vec3 pointOnPlane(0.0f, 2.0f, 0.0f);
    const glm::vec4 tilted(n, -glm::dot(n, pointOnPlane));
    const glm::mat4 m2 = PlanarReflection::MirrorMatrix(tilted);
    const glm::vec3 p(1.0f, 5.0f, 3.0f);
    const glm::vec3 q(m2 * glm::vec4(p, 1.0f));
    const float dp = glm::dot(n, p) + tilted.w;
    const float dq = glm::dot(n, q) + tilted.w;
    std::printf("       наклонная плоскость: расстояние %.3f -> %.3f\n", dp, dq);
    Check(std::fabs(dp + dq) < 1e-4f, "на наклонной плоскости расстояние меняет знак");
    Check(glm::length(glm::cross(n, glm::normalize(p - q))) < 1e-3f,
          "точка движется строго вдоль нормали");
}

} // namespace

void RunReflectionChecks(FrameRenderer& r) {
    TestLensFlare(r);
    TestReflections(r);
    TestReflectionProbe(r);
    TestReflectionSeams();
    TestPlanarReflectionMath();
    TestPlanarReflectionRender(r);
}

} // namespace sage::rendertest
