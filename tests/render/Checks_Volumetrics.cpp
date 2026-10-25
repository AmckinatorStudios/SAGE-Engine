// ---------------------------------------------------------------------------
// Эталонные кадры — объёмный свет: качество марша.
//
// Марш по лучу — это интеграл, взятый конечным числом шагов, и КАЖДЫЙ его
// артефакт измерим числом, а не «выглядит лучше»:
//
//   • ПОЛОСЫ (мало шагов, шаг не сдвинут) — правильные, но резко ступенчатые
//     переходы: у соседних пикселей одинаковое значение, а раз в несколько
//     пикселей — скачок. Ловится долей пикселей, отличающихся от соседа
//     заметнее порога, при том что вторая производная почти везде ноль.
//   • ЗЕРНО (шаг сдвинут шумом, накопления нет) — соседние пиксели отличаются
//     СЛУЧАЙНО. Ловится средним модулем лапласиана: у гладкой дымки он около
//     нуля, у зернистой растёт вместе с амплитудой шума.
//   • КАЙМА НА СИЛУЭТЕ (уменьшенный буфер, чужие выборки протекли через край)
//     — светлая или тёмная линия ВОКРУГ предмета, которой нет ни на предмете,
//     ни на фоне.
//
// Сравнение идёт не с картинкой на диске, а между режимами В ОДНОМ ПРОГОНЕ:
// «накопление включено» против «выключено», «полное разрешение» против
// «половины». Так проверка не зависит ни от драйвера, ни от того, что кто-то
// перезапишет эталон, и отвечает ровно на тот вопрос, ради которого написана.
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/ecs/LightSystem.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/ShadowMap.h"
#include "sage/render/Volumetrics.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace sage::rendertest {
namespace {

// Сцена под лучи: решётка столбов между солнцем и камерой. Именно она даёт
// чередование освещённого и затенённого воздуха — то, ради чего объём и нужен,
// и то, на чём видны все три артефакта сразу.
std::unique_ptr<Scene> BuildShaftScene() {
    auto scene = std::make_unique<Scene>("VolumetricShafts");
    // Солнце ЗА столбами и почти НАВСТРЕЧУ камере: только так фазовая функция
    // даёт всплеск вперёд, ради которого объём и включают, — и только так лучи
    // в кадре яркие, а не «где-то есть». Камера фикстуры смотрит из (4.2, 3.4,
    // 5.6) в начало координат, поэтому направление НА солнце примерно совпадает
    // с направлением взгляда.
    scene->Lighting.Sun.Direction = glm::normalize(glm::vec3(0.55f, -0.28f, 0.75f));
    scene->Lighting.Sun.Intensity = 6.0f;
    scene->Lighting.Sun.Color = glm::vec3(1.0f, 0.96f, 0.88f);
    scene->Lighting.AmbientStrength = 0.18f;
    // Ambient здесь задан ЯВНО и небу не подчиняется — значит «свои значения»
    // (см. LightingEnvironment::AmbientMode). Выключенное небо иначе забирает
    // с собой и окружающий свет, и проверять было бы нечего: чёрный кадр.
    scene->Lighting.AmbientMode = LightingEnvironment::AmbientSource::Custom;
    scene->Lighting.Skybox.Enabled = false;

    auto cube = [&](const char* name, glm::vec3 pos, glm::vec3 scale, glm::vec3 color) {
        GameObject o = scene->CreateObject(name);
        o.GetTransform().Position = pos;
        o.GetTransform().Scale = scale;
        o.Renderer().Ref = MeshRef{MeshRef::Type::Cube};
        o.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
        o.Renderer().Color = color;
    };

    cube("Ground", {0.0f, -1.0f, 0.0f}, {24.0f, 0.4f, 24.0f}, {0.32f, 0.33f, 0.35f});
    // Столбы между солнцем и камерой: между ними свет проходит, за ними — нет.
    // Тонкие намеренно: на толстых кайма подъёма не видна.
    for (int i = 0; i < 7; ++i) {
        cube(("Pillar" + std::to_string(i)).c_str(),
             {-4.0f + 1.4f * (float)i, 1.4f, -3.5f}, {0.30f, 5.0f, 0.30f},
             {0.55f, 0.52f, 0.48f});
    }
    return scene;
}

sage::render::VolumetricSettings ShaftSettings() {
    sage::render::VolumetricSettings v;
    v.Enabled = true;
    v.LightShafts = true;
    v.Clouds = false;          // облака здесь не при чём: проверяются лучи
    v.Density = 0.05f;         // густо: артефакты марша видны, а не угадываются
    v.Anisotropy = 0.7f;
    v.MaxDistance = 60.0f;
    v.Steps = 24;              // НАРОЧНО немного: на сотне шагов артефактов нет
                               // ни у кого, и проверка ничего бы не показала
    v.Intensity = 0.30f;
    v.HeightFalloff = 0.01f;
    v.Scale = 1.0f;
    v.Temporal = true;
    v.TemporalBlend = 0.9f;
    return v;
}

// Кадр объёма БЕЗ сцены под ним: чистый вклад прохода на чёрном. Так измеряется
// он сам, а не сумма с освещением, тенями и пост-обработкой.
//
// frames — сколько кадров прогнать. Накоплению нужно несколько: один кадр у
// него по определению такой же зернистый, как и без него.
Image RenderVolume(sage::render::Volumetrics& vol, Scene& scene,
                   const sage::render::VolumetricSettings& s, int frames, int viewId) {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    const LightingEnvironment env = sage::ecs::CollectLighting(scene);
    const glm::mat4 view = TestView();
    const glm::mat4 proj = PerspectiveProj();

    ShadowMap shadow(1024);
    shadow.SetLightMatrix(env.Sun.Direction, glm::vec3(0.0f), 16.0f);
    shadow.BeginRender();
    sage::ecs::RenderBatch batch;
    batch.RenderDepth(scene, shadow.LightMatrix());
    shadow.EndRender(kW, kH);

    Framebuffer fbo(kW, kH);
    Image last;
    for (int f = 0; f < frames; ++f) {
        // Глубина сцены нужна настоящая: марш обрывается о геометрию, и без неё
        // столбы не отбрасывали бы в воздухе ничего.
        fbo.Bind();
        device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        device.Clear(true, true);
        batch.RenderColor(scene, view, proj, kEye, env, ShadowBinding(shadow, true), 0);

        // Цвет гасим, глубину оставляем: в кадре остаётся ТОЛЬКО объём.
        fbo.Bind();
        device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        device.Clear(true, false);

        vol.Render(fbo, fbo.DepthTexture(), kW, kH, proj, view, kEye, env,
                   ShadowBinding(shadow, true), s, 0.0f, viewId);

        fbo.Resolve();
        fbo.Bind();
        last = Capture(kW, kH);
    }
    return last;
}

// --- Меры -------------------------------------------------------------------

// Средний модуль лапласиана по зелёному каналу: у гладкой дымки почти ноль, у
// зернистой или полосящей растёт. Считается по ОБЛАСТИ ВОЗДУХА (верхняя часть
// кадра), где нет геометрии: на кромках предметов лапласиан велик законно.
double Roughness(const Image& img, int y0, int y1) {
    if (img.Empty()) return 0.0;
    double sum = 0.0;
    long n = 0;
    for (int y = y0 + 1; y < y1 - 1; ++y) {
        for (int x = 1; x < img.Width - 1; ++x) {
            auto at = [&](int xx, int yy) {
                return (double)img.Pixels[((size_t)yy * img.Width + xx) * 3 + 1];
            };
            const double lap = 4.0 * at(x, y) - at(x - 1, y) - at(x + 1, y) - at(x, y - 1) -
                               at(x, y + 1);
            sum += std::abs(lap);
            ++n;
        }
    }
    return n ? sum / (double)n : 0.0;
}

double MeanBrightness(const Image& img, int y0, int y1) {
    if (img.Empty()) return 0.0;
    double sum = 0.0;
    long n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < img.Width; ++x) {
            const size_t i = ((size_t)y * img.Width + x) * 3;
            sum += (img.Pixels[i] + img.Pixels[i + 1] + img.Pixels[i + 2]) / 3.0;
            ++n;
        }
    }
    return n ? sum / (double)n : 0.0;
}

// --- Проверки ---------------------------------------------------------------

void TestTemporalRemovesGrain(sage::render::Volumetrics& vol, Scene& scene) {
    sage::render::VolumetricSettings s = ShaftSettings();

    // ЭТАЛОН — тот же марш, но 128 шагами и без накопления. Это и есть «как
    // должно выглядеть»: столько выборок, что сдвиг начала уже ничего не решает.
    // Сравнивать с ним честнее, чем с числом из воздуха: проверка сама говорит,
    // где потолок, и требует до него дойти.
    s.Temporal = false;
    s.Steps = 128;
    vol.ResetHistory();
    const Image ref = RenderVolume(vol, scene, s, /*frames=*/1, /*viewId=*/0);

    // Без накопления и с рабочим числом шагов: сдвиг шага фиксирован, зерно
    // стоит на экране узором.
    s.Steps = 24;
    vol.ResetHistory();
    const Image raw = RenderVolume(vol, scene, s, /*frames=*/1, /*viewId=*/1);

    // С накоплением: тот же марш, те же 24 шага, но усреднённый по кадрам.
    s.Temporal = true;
    vol.ResetHistory();
    const Image acc = RenderVolume(vol, scene, s, /*frames=*/32, /*viewId=*/2);

    const int y1 = kH / 2;   // верхняя половина — воздух, геометрии почти нет
    const double refR = Roughness(ref, 0, y1);
    const double rawR = Roughness(raw, 0, y1);
    const double accR = Roughness(acc, 0, y1);
    const double refB = MeanBrightness(ref, 0, y1);
    const double accB = MeanBrightness(acc, 0, y1);

    std::printf("    шероховатость: 24 шага %.2f, они же с накоплением %.2f, "
                "эталон в 128 шагов %.2f (яркость %.1f против %.1f)\n",
                rawR, accR, refR, accB, refB);

    Check(rawR > refR * 1.3, "24 шага без накопления действительно зернят");
    Check(accR < refR * 1.15,
          "накопление доводит 24 шага до гладкости стадвадцативосьмишагового марша");
    // И не ценой картинки: усреднение обязано сойтись к ПРАВИЛЬНОМУ ответу, а
    // не размыть неправильный.
    Check(std::abs(accB - refB) < std::max(refB * 0.04, 1.0),
          "накопленный кадр сходится к эталону по яркости, а не размывается");
}

void TestFullResIsSharp(sage::render::Volumetrics& vol, Scene& scene) {
    sage::render::VolumetricSettings s = ShaftSettings();
    s.Temporal = true;

    s.Scale = 1.0f;
    vol.ResetHistory();
    const Image full = RenderVolume(vol, scene, s, 24, /*viewId=*/3);

    s.Scale = 0.5f;
    vol.ResetHistory();
    const Image half = RenderVolume(vol, scene, s, 24, /*viewId=*/4);

    // Полоса вокруг силуэтов столбов: там, где половинный буфер протекает,
    // разница между режимами максимальна. Меряем среднюю разницу по всему кадру
    // — она обязана быть МАЛОЙ: подъём для того и написан, чтобы половина
    // отличалась от полного разрешения мягкостью, а не другой картинкой.
    double diff = 0.0;
    long n = 0;
    for (size_t i = 0; i + 2 < full.Pixels.size() && i + 2 < half.Pixels.size(); i += 3) {
        diff += std::abs((int)full.Pixels[i + 1] - (int)half.Pixels[i + 1]);
        ++n;
    }
    diff = n ? diff / (double)n : 0.0;

    // Резкость: у половинного разрешения детали крупнее, поэтому лапласиан на
    // ГЕОМЕТРИИ (нижняя половина кадра, кромки столбов) у полного выше. Если бы
    // подъём просто размывал, разницы бы не было.
    const double fullR = Roughness(full, kH / 2, kH);
    const double halfR = Roughness(half, kH / 2, kH);
    std::printf("    полное против половины: средняя разница %.2f из 255, "
                "резкость на кромках %.2f против %.2f\n", diff, fullR, halfR);

    Check(diff < 6.0, "половинный проход даёт ту же картинку, а не другую");
    Check(fullR >= halfR * 0.95, "полное разрешение не мягче половинного");
}

void TestHistoryIsPerView(sage::render::Volumetrics& vol, Scene& scene) {
    sage::render::VolumetricSettings s = ShaftSettings();
    s.Temporal = true;

    // Два «окна» с ОДНОЙ камерой, но чередующиеся, как вьюпорт и панель Game.
    // Если история общая, второе окно подмешивает кадры первого — и при
    // одинаковой камере это незаметно. Поэтому первому окну даём накопиться, а
    // второе просим ОДИН кадр: с раздельной историей он обязан быть сырым.
    vol.ResetHistory();
    const Image warm = RenderVolume(vol, scene, s, 24, /*viewId=*/5);
    const Image cold = RenderVolume(vol, scene, s, 1, /*viewId=*/6);

    const int y1 = kH / 2;
    const double warmR = Roughness(warm, 0, y1);
    const double coldR = Roughness(cold, 0, y1);
    std::printf("    накопленное окно %.2f, свежее окно %.2f\n", warmR, coldR);
    // Общая история сделала бы свежее окно ТАКИМ ЖЕ гладким, как накопленное
    // (отношение около единицы). Раздельная оставляет его сырым.
    Check(coldR > warmR * 1.25,
          "история принадлежит виду: чужое накопление в новое окно не течёт");
}

// Кадр объёма в своём буфере заданного размера — для проверок, где сцена не
// нужна (дымка до дальней плоскости, облака в небе).
Image RenderEmptyVolume(sage::render::Volumetrics& vol, const LightingEnvironment& env,
                        const sage::render::VolumetricSettings& s, int w, int h,
                        const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye,
                        int viewId, int frames = 1) {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    Framebuffer fbo(w, h);
    Image last;
    for (int f = 0; f < frames; ++f) {
        fbo.Bind();
        device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        device.Clear(true, true);
        vol.Render(fbo, fbo.DepthTexture(), w, h, proj, view, eye, env, ShadowBinding(), s, 0.0f, viewId);
        fbo.Bind();
        last = Capture(w, h);
    }
    return last;
}

// Окна РАЗНОГО размера в одном кадре — как вьюпорт и панель Game. Раньше
// буферы прохода были общими, смена размера сбрасывала историю всех окон, и
// накопление не доживало ни до одного смешивания.
void TestHistorySurvivesOtherWindowSize(sage::render::Volumetrics& vol, Scene& scene) {
    const LightingEnvironment env = sage::ecs::CollectLighting(scene);
    sage::render::VolumetricSettings s = ShaftSettings();
    vol.ResetHistory();
    RenderEmptyVolume(vol, env, s, 96, 64, TestView(), PerspectiveProj(), kEye, /*viewId=*/11);
    RenderEmptyVolume(vol, env, s, 64, 40, TestView(), PerspectiveProj(), kEye, /*viewId=*/12);
    const bool survived = vol.HistoryValid(11);
    std::printf("    история окна 96x64 после окна 64x40: %s\n", survived ? "цела" : "сброшена");
    Check(survived, "накопление окна переживает кадр другого окна другого размера");
}

// Ортокамера смотрит на плоскость отвесно: путь каждого луча до неё одинаков,
// и дымка обязана лечь РОВНО. Пока лучи шли из точки камеры веером, у края
// кадра они шли наискось и набирали больше дымки — кадр светлел к краям.
void TestOrthoHazeIsEven(sage::render::Volumetrics& vol) {
    Scene scene("OrthoHaze");
    scene.Lighting.Sun.Direction = glm::normalize(glm::vec3(0.0f, -1.0f, 0.0f));
    scene.Lighting.Sun.Intensity = 4.0f;
    // Пол обязателен: без него луч любого пикселя идёт до дальности марша, и
    // разница «из камеры / с ближней плоскости» в длине пути не видна.
    GameObject ground = scene.CreateObject("Ground");
    ground.GetTransform().Position = {0.0f, -0.5f, 0.0f};
    ground.GetTransform().Scale = {200.0f, 1.0f, 200.0f};
    ground.Renderer().Ref = MeshRef{MeshRef::Type::Cube};
    ground.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
    const LightingEnvironment env = sage::ecs::CollectLighting(scene);
    sage::render::VolumetricSettings s = ShaftSettings();
    s.Temporal = false;
    s.Steps = 64;
    s.HeightFalloff = 0.0f;
    s.Density = 0.03f;
    s.Intensity = 3.0f;
    s.MaxDistance = 40.0f;
    const int w = 96, h = 64;
    const glm::vec3 eye(0.0f, 20.0f, 0.0f);
    const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0, 0, -1));
    const glm::mat4 proj = glm::ortho(-30.0f, 30.0f, -20.0f, 20.0f, 0.5f, 20.0f);
    vol.ResetHistory();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    Framebuffer fbo(w, h);
    fbo.Bind();
    device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    device.Clear(true, true);
    sage::ecs::RenderBatch batch;
    batch.RenderColor(scene, view, proj, eye, env, ShadowBinding(), 0);
    fbo.Bind();
    device.Clear(true, false);   // цвет гасим, глубину пола оставляем
    vol.Render(fbo, fbo.DepthTexture(), w, h, proj, view, eye, env, ShadowBinding(), s, 0.0f, 13);
    fbo.Bind();
    const Image img = Capture(w, h);
    auto column = [&](int x0) {
        double sum = 0.0;
        for (int y = 0; y < h; ++y)
            for (int x = x0; x < x0 + 8; ++x) sum += img.Pixels[((size_t)y * w + x) * 3 + 1];
        return sum / (h * 8.0);
    };
    const double edge = column(0), centre = column(w / 2 - 4);
    std::printf("    дымка в ортографии: у края %.1f, в центре %.1f\n", edge, centre);
    Check(centre > 2.0 && std::abs(edge - centre) < centre * 0.1,
          "в ортографии дымка ложится ровно (лучи параллельны, а не веером из камеры)");
}

// Ночью облака не светятся днём: их подсвечивает небо ТЕКУЩЕГО часа. Раньше
// бралось дневное «Sky Color» из настроек окружения и цвет тумана — и облака
// сияли голубым поверх чёрного ночного неба.
void TestNightCloudsAreDark(sage::render::Volumetrics& vol) {
    LightingEnvironment env;
    env.Sun.Intensity = 0.0f;
    env.SkyColor = {1.0f, 1.0f, 1.0f};
    env.Fog.Color = {1.0f, 1.0f, 1.0f};
    env.SkyResolved = true;
    env.SkyTopNow = {0.0f, 0.0f, 0.0f};
    env.SkyHorizonNow = {0.0f, 0.0f, 0.0f};
    sage::render::VolumetricSettings s;
    s.Enabled = true;
    s.LightShafts = false;
    s.Clouds = true;
    s.Coverage = 0.9f;
    s.Temporal = false;
    const int w = 96, h = 64;
    const glm::vec3 eye(0.0f, 2.0f, 0.0f);
    const glm::mat4 view = glm::lookAt(eye, eye + glm::vec3(0.0f, 1.0f, -0.4f), glm::vec3(0, 0, -1));
    const glm::mat4 proj = glm::perspective(glm::radians(70.0f), (float)w / (float)h, 0.1f, 5000.0f);
    vol.ResetHistory();
    const Image night = RenderEmptyVolume(vol, env, s, w, h, view, proj, eye, /*viewId=*/14);
    env.SkyTopNow = {0.4f, 0.6f, 1.0f};
    env.SkyHorizonNow = {0.7f, 0.8f, 1.0f};
    vol.ResetHistory();
    const Image day = RenderEmptyVolume(vol, env, s, w, h, view, proj, eye, /*viewId=*/14);
    const double n = MeanBrightness(night, 0, h), d = MeanBrightness(day, 0, h);
    std::printf("    облака под ночным небом %.1f, под дневным %.1f\n", n, d);
    Check(d > 3.0 && n < d * 0.2, "ночью облака не подсвечены дневным небом");
}

} // namespace

void RunVolumetricChecks() {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    (void)device;
    std::unique_ptr<Scene> scene = BuildShaftScene();
    sage::render::Volumetrics vol;

    TestTemporalRemovesGrain(vol, *scene);
    TestFullResIsSharp(vol, *scene);
    TestHistoryIsPerView(vol, *scene);
    TestHistorySurvivesOtherWindowSize(vol, *scene);
    TestOrthoHazeIsEven(vol);
    TestNightCloudsAreDark(vol);
}

} // namespace sage::rendertest
