// ---------------------------------------------------------------------------
// Эталонные кадры — кадр: камера, пост-обработка, сглаживание, прозрачность.
//
// Всё, что проверяет САМ КАДР: как он снят и что с ним сделала цепочка
// пост-обработки. Это самая «эталонная» часть набора — здесь картинка
// сравнивается с записанной, и любое изменение в рендере видно сразу.
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

void TestScenePerspective(FrameRenderer& r, Scene& scene) {
    Report("scene_perspective", CompareWithReference(
                                    "scene_perspective",
                                    RenderFrame(r, scene, PerspectiveProj(), BaseSettings(), kW, kH)));
}

// Выключенные тени НЕ ДОЛЖНЫ означать чёрную сцену.
//
// Привязка теней оставляла юниты сэмплеров пустыми, когда тени выключены. На
// программном растеризаторе (на котором идут эти тесты) непривязанный юнит
// ведёт себя смирно, а на настоящем GPU отдаёт ноль — то есть «всё в тени», и
// сцена с живым солнцем выходит ЧЁРНОЙ. Поймать это эталоном нельзя: эталон
// снят на том же растеризаторе. Поэтому проверяется СВОЙСТВО — кадр без теней
// не темнее кадра с тенями и вообще не чёрный.
void TestShadowsOffIsNotBlack(FrameRenderer& r, Scene& scene) {
    Framebuffer sceneFbo(kW, kH);
    const LightingEnvironment env = sage::ecs::CollectLighting(scene);
    const glm::mat4 view = TestView();

    sceneFbo.Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
    device.Clear(true, true);
    // Тени ВЫКЛЮЧЕНЫ: ShadowBinding по умолчанию — ни одной живой карты.
    r.Batch.RenderColor(scene, view, PerspectiveProj(), kEye, env, ShadowBinding(), 0);
    const Image frame = Capture(kW, kH);
    device.BindDefaultFramebuffer();

    double sum = 0.0;
    for (size_t i = 0; i < frame.Pixels.size(); ++i) sum += frame.Pixels[i];
    const double mean = frame.Pixels.empty() ? 0.0 : sum / (double)frame.Pixels.size();

    // Фон сам по себе тёмный (0.05..0.08), поэтому порог берём заметно выше
    // него: освещённая геометрия обязана поднять среднее.
    if (mean > 24.0) {
        CountPass();
        std::printf("[ ok ] %-28s среднее %.1f (сцена освещена)\n", "shadows_off_not_black", mean);
    } else {
        CountFail();
        std::printf("[FAIL] %-28s среднее %.1f — сцена ЧЁРНАЯ при выключенных тенях\n",
                    "shadows_off_not_black", mean);
    }
}

void TestSceneOrthographic(FrameRenderer& r, Scene& scene) {
    // Ортокамера — новая возможность движка, и «работает» для неё значит
    // «даёт правильную картинку», а не «компилируется».
    CameraComponent cam;
    cam.Mode = CameraComponent::Projection::Orthographic;
    cam.OrthoHeight = 6.0f;
    cam.NearClip = 0.1f;
    cam.FarClip = 100.0f;
    const glm::mat4 proj = cam.ProjectionMatrix((float)kW / (float)kH);

    const Image ortho = RenderFrame(r, scene, proj, BaseSettings(), kW, kH);
    Report("scene_orthographic", CompareWithReference("scene_orthographic", ortho));

    // Отдельная проверка смысла: орто-кадр обязан ОТЛИЧАТЬСЯ от перспективного.
    // Без неё тест прошёл бы и в случае, если ProjectionMatrix молча вернула
    // перспективу, — эталон просто записался бы дважды одинаковым.
    const Image persp = RenderFrame(r, scene, PerspectiveProj(), BaseSettings(), kW, kH);
    long long sum = 0;
    for (size_t i = 0; i < ortho.Pixels.size(); ++i) {
        sum += std::abs((int)ortho.Pixels[i] - (int)persp.Pixels[i]);
    }
    const double mean = (double)sum / (double)ortho.Pixels.size();
    std::printf("       орто против перспективы: среднее расхождение %.2f\n", mean);
    Check(mean > 5.0, "орто-камера даёт другую картинку, чем перспектива");
}

void TestNoPostFX(FrameRenderer& r, Scene& scene) {
    // Выключённая пост-обработка — отдельный путь кода (Enabled=false), и он
    // тоже должен давать стабильную картинку.
    sage::render::PostFXSettings fx = BaseSettings();
    fx.Enabled = false;
    Report("scene_no_postfx",
           CompareWithReference("scene_no_postfx", RenderFrame(r, scene, PerspectiveProj(), fx, kW, kH)));
}

void TestDepthOfField(FrameRenderer& r, Scene& scene) {
    sage::render::PostFXSettings fx = BaseSettings();
    fx.DofEnabled = true;
    fx.FocusDistance = 6.0f; // примерно на сфере
    fx.Aperture = 1.8f;
    const Image dof = RenderFrame(r, scene, PerspectiveProj(), fx, kW, kH);
    Report("scene_dof", CompareWithReference("scene_dof", dof));

    const Image sharp = RenderFrame(r, scene, PerspectiveProj(), BaseSettings(), kW, kH);
    long long sum = 0;
    for (size_t i = 0; i < dof.Pixels.size(); ++i) {
        sum += std::abs((int)dof.Pixels[i] - (int)sharp.Pixels[i]);
    }
    const double mean = (double)sum / (double)dof.Pixels.size();
    std::printf("       глубина резкости изменила кадр на %.2f в среднем\n", mean);
    Check(mean > 1.0, "глубина резкости реально влияет на картинку");
}

// --- MSAA -------------------------------------------------------------------
//
// Сглаживание бывает двух разных природ, и проверять их надо по-разному.
//
// FXAA — экранный фильтр: он ищет перепад яркости в ГОТОВОЙ картинке и
// замывает его. На пологой кромке (горизонт, длинная грань пола) перепад
// размазан по многим пикселям, догадаться о геометрии не по чему, и лесенка
// остаётся — именно её и было видно в отрендеренном ролике.
//
// MSAA работает раньше: растеризатор считает ПОКРЫТИЕ пикселя геометрией по
// нескольким точкам. Поэтому здесь проверяется не «картинка изменилась», а то,
// что у кромок появились ПРОМЕЖУТОЧНЫЕ значения — резких скачков между
// соседними пикселями стало меньше.
void TestMsaa(FrameRenderer& r) {
    // Сцена НАРОЧНО вырожденная: один повёрнутый куб на пустом фоне, без пола,
    // без теней, без освещения. Так почти каждый резкий перепад между соседними
    // пикселями — это силуэтная кромка, и метрика меряет именно её.
    //
    // На обычной сцене та же метрика считала бы заодно границы теней и складки
    // затенения, на которые MSAA не влияет никак: он про покрытие пикселя
    // геометрией, а не про то, что на этой геометрии нарисовано. Первая версия
    // теста этого не учитывала и требовала от MSAA невозможного.
    auto scene = std::make_unique<Scene>("MsaaTest");
    scene->Lighting.Skybox.Enabled = false;
    scene->Lighting.AmbientStrength = 1.0f;

    GameObject cube = scene->CreateObject("Cube");
    cube.GetTransform().Position = {0.0f, 0.8f, 0.0f};
    cube.GetTransform().Rotation = {17.0f, 31.0f, 9.0f}; // косые кромки — худший случай
    cube.GetTransform().Scale = {1.6f, 1.6f, 1.6f};
    cube.Renderer().Ref = MeshRef{MeshRef::Type::Cube};
    cube.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
    cube.Renderer().Color = {0.95f, 0.95f, 0.95f};

    const glm::mat4 proj = PerspectiveProj();
    const glm::mat4 view = TestView();

    sage::render::PostFXSettings fx = BaseSettings();
    fx.FxaaEnabled = false; // иначе измеряли бы сумму двух разных механизмов
    fx.BloomEnabled = false;
    fx.AOEnabled = false;
    fx.Vignette = 0.0f;

    auto render = [&](int samples) {
        Framebuffer sceneFbo(kW, kH, samples);
        Framebuffer output(kW, kH);
        const LightingEnvironment env = sage::ecs::CollectLighting(*scene);

        sceneFbo.Bind();
        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
        device.Clear(true, true);
        // Режим 1 (unlit): плоский цвет без освещения и без теней — на кубе не
        // будет ни градиента, ни складок, только силуэт.
        r.Batch.RenderColor(*scene, view, proj, kEye, env, ShadowBinding(), 1);
        sceneFbo.Resolve();

        r.Fx.ResetHistory();
        r.Fx.Render(sceneFbo.ColorTexture(), sceneFbo.DepthTexture(), kW, kH, proj, view, fx,
                    &output, 0, 0, kW, kH);
        output.Bind();
        return std::make_pair(Capture(kW, kH), sceneFbo.Samples());
    };

    const auto plain = render(1);
    const auto msaa = render(4);

    Check(plain.second == 1, "буфер без MSAA действительно односэмпловый");
    // Драйвер вправе выдать меньше запрошенного — тогда проверка ниже мерила бы
    // не то, что думает, и об этом надо знать сразу.
    Check(msaa.second > 1, "буфер с MSAA получил больше одного сэмпла");
    if (msaa.second <= 1) return;
    std::printf("       сэмплов выделено: %d\n", msaa.second);

    // Меряем ПРОМЕЖУТОЧНЫЕ пиксели: те, что не фон и не объект. Их наличие и
    // есть сглаживание — растеризатор посчитал частичное покрытие пикселя.
    //
    // Считать «резкие перепады» здесь нельзя, и это стоило двух неверных
    // подходов подряд. Сглаживание не убирает перепад, а разбивает один скачок
    // на несколько; с низким порогом сглаженная кромка даёт БОЛЬШЕ срабатываний
    // (проверено: 174 против 202), а с высоким счёт вообще не меняется, потому
    // что после ACES и гаммы даже наполовину покрытый пиксель яркого объекта
    // остаётся близко к его цвету.
    //
    // Границы берём из самой картинки, а не числом из воздуха: тон-маппинг,
    // экспозиция и гамма двигают абсолютные значения, и любой зашитый порог
    // сломался бы от правки грейда, ничего не сломав в рендере.
    auto intermediatePixels = [](const Image& img) {
        int lo = 255, hi = 0;
        for (size_t i = 0; i < img.Pixels.size(); i += 3) {
            lo = std::min(lo, (int)img.Pixels[i]);
            hi = std::max(hi, (int)img.Pixels[i]);
        }
        const int span = hi - lo;
        if (span < 40) return (size_t)0; // нечего различать
        const int low = lo + span / 5, high = hi - span / 5;
        size_t count = 0;
        for (size_t i = 0; i < img.Pixels.size(); i += 3) {
            const int v = img.Pixels[i];
            if (v > low && v < high) ++count;
        }
        return count;
    };

    const size_t midPlain = intermediatePixels(plain.first);
    const size_t midMsaa = intermediatePixels(msaa.first);
    std::printf("       промежуточных пикселей на кромке: без MSAA %zu, с MSAA %zu\n",
                midPlain, midMsaa);

    // Без сглаживания пиксель принадлежит либо фону, либо объекту — третьего
    // растеризатор без MSAA дать не может, и это «почти ноль» тут не оценка, а
    // свойство: единичные срабатывания приходят от дизеринга в композите.
    Check(midPlain < 20, "без MSAA промежуточных значений на кромке практически нет");
    Check(midMsaa > 30, "MSAA даёт промежуточные значения на кромке");
    Check(midMsaa > midPlain * 3, "разница между путями не на уровне шума");

    Report("scene_msaa", CompareWithReference("scene_msaa", msaa.first));
}

// --- Смаз движения от ОБЪЕКТОВ ----------------------------------------------
//
// Главное, что здесь проверяется, — не «смаз работает», а то, что он работает
// там, где старый путь работать не мог В ПРИНЦИПЕ.
//
// Старый смаз восстанавливал мировую точку из глубины и проецировал её матрицей
// прошлого кадра. Так виден только сдвиг КАМЕРЫ: мир при этом считается
// неподвижным. Значит, при неподвижной камере пролетающий мимо объект обязан
// остаться резким — и это тоже проверяется, иначе тест не отличал бы новый путь
// от старого.
void TestObjectMotionBlur(FrameRenderer& r) {
    auto scene = std::make_unique<Scene>("MotionTest");
    scene->Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.35f));
    scene->Lighting.Sun.Intensity = 1.2f;
    scene->Lighting.SkyColor = {0.40f, 0.48f, 0.64f};
    scene->Lighting.GroundColor = {0.20f, 0.17f, 0.15f};
    scene->Lighting.AmbientStrength = 0.35f;
    scene->Lighting.Skybox.Enabled = false;

    GameObject ground = scene->CreateObject("Ground");
    ground.GetTransform().Scale = {14.0f, 1.0f, 14.0f};
    ground.Renderer().Ref = MeshRef{MeshRef::Type::Plane};
    ground.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Plane);
    ground.Renderer().Color = {0.34f, 0.35f, 0.37f};

    GameObject mover = scene->CreateObject("Mover");
    mover.GetTransform().Position = {-1.5f, 0.8f, 0.0f};
    mover.GetTransform().Scale = {0.8f, 0.8f, 0.8f};
    mover.Renderer().Ref = MeshRef{MeshRef::Type::Cube};
    mover.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
    mover.Renderer().Color = {0.85f, 0.30f, 0.20f};

    const glm::mat4 proj = PerspectiveProj();
    const glm::mat4 view = TestView();
    const glm::mat4 viewProj = proj * view;

    sage::render::PostFXSettings fx = BaseSettings();
    fx.MotionBlurEnabled = true;
    fx.MotionBlurAmount = 1.0f;
    fx.BloomEnabled = false; // bloom размазал бы кромку и смазал границу измерения
    fx.AOEnabled = false;

    // Буфер скоростей размером с кадр. ColorHDRWithDepth: глубина нужна, чтобы
    // скорость писала ближайшая поверхность, а не последняя нарисованная.
    Framebuffer velocity(kW, kH);

    auto renderWithVelocity = [&](bool useVelocity) {
        Framebuffer sceneFbo(kW, kH), output(kW, kH);
        const LightingEnvironment env = sage::ecs::CollectLighting(*scene);

        r.Shadow.SetLightMatrix(env.Sun.Direction, glm::vec3(0.0f), 12.0f);
        r.Shadow.BeginRender();
        r.Batch.RenderDepth(*scene, r.Shadow.LightMatrix());
        r.Shadow.EndRender(kW, kH);

        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        sceneFbo.Bind();
        device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
        device.Clear(true, true);
        r.Batch.RenderColor(*scene, view, proj, kEye, env, ShadowBinding(r.Shadow, true), 0);

        sage::rhi::TextureHandle velTex;
        if (useVelocity) {
            velocity.Bind();
            device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            device.Clear(true, true);
            r.Batch.RenderVelocity(viewProj, viewProj); // камера НЕ движется
            velTex = velocity.ColorTexture();
        }

        r.Fx.Render(sceneFbo.ColorTexture(), sceneFbo.DepthTexture(), kW, kH, proj, view, fx,
                    &output, 0, 0, kW, kH, velTex);
        output.Bind();
        return Capture(kW, kH);
    };

    auto meanDiff = [](const Image& a, const Image& b) {
        long long sum = 0;
        for (size_t i = 0; i < a.Pixels.size(); ++i) {
            sum += std::abs((int)a.Pixels[i] - (int)b.Pixels[i]);
        }
        return (double)sum / (double)a.Pixels.size();
    };

    // --- Шаг 1: объект стоит, историю движения заводим на его текущем месте ---
    r.Batch.ResetVelocityHistory();
    r.Fx.ResetHistory();
    renderWithVelocity(true);
    r.Batch.AdvanceVelocityHistory();

    // Опорный кадр без смаза, снятый в ТОЙ ЖЕ точке, куда объект сейчас
    // переедет: с ним и сравниваем, иначе разница была бы просто от смещения.
    mover.GetTransform().Position.x = -0.3f;
    sage::render::PostFXSettings sharpFx = fx;
    sharpFx.MotionBlurEnabled = false;
    Image sharp;
    {
        Framebuffer sceneFbo(kW, kH), output(kW, kH);
        const LightingEnvironment env = sage::ecs::CollectLighting(*scene);
        r.Shadow.SetLightMatrix(env.Sun.Direction, glm::vec3(0.0f), 12.0f);
        r.Shadow.BeginRender();
        r.Batch.RenderDepth(*scene, r.Shadow.LightMatrix());
        r.Shadow.EndRender(kW, kH);
        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        sceneFbo.Bind();
        device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
        device.Clear(true, true);
        r.Batch.RenderColor(*scene, view, proj, kEye, env, ShadowBinding(r.Shadow, true), 0);
        r.Fx.Render(sceneFbo.ColorTexture(), sceneFbo.DepthTexture(), kW, kH, proj, view, sharpFx,
                    &output, 0, 0, kW, kH);
        output.Bind();
        sharp = Capture(kW, kH);
    }

    // --- Шаг 2: объект переехал, камера НЕ двигалась ---
    const Image withVelocity = renderWithVelocity(true);
    const Image withoutVelocity = renderWithVelocity(false);

    Report("motion_object", CompareWithReference("motion_object", withVelocity));

    const double velocityVsSharp = meanDiff(withVelocity, sharp);
    const double oldPathVsSharp = meanDiff(withoutVelocity, sharp);
    std::printf("       смаз с буфером скоростей: %.3f, старым путём: %.3f\n",
                velocityVsSharp, oldPathVsSharp);

    // Порог низкий не по слабости проверки: среднее берётся по ВСЕМУ кадру, а
    // смазанный объект занимает около процента его площади — то есть внутри
    // затронутой области изменение примерно в сто раз больше этого числа.
    // Локальность отдельно проверяется ниже по доле тронутых пикселей.
    Check(velocityVsSharp > 0.1, "буфер скоростей даёт смаз от движения объекта");
    // Вот ради этой проверки тест и написан: старый путь при неподвижной камере
    // не может отличить движущийся объект от стоящего, и разница выходит РОВНО
    // нулевой — не «маленькой», а нулевой.
    Check(oldPathVsSharp < 0.001, "старый путь при неподвижной камере смаза не даёт");

    // Смаз обязан быть ЛОКАЛЬНЫМ: тронуть окрестность объекта, а не весь кадр.
    size_t touched = 0;
    const size_t pixels = withVelocity.Pixels.size() / 3;
    for (size_t i = 0; i < pixels; ++i) {
        int worst = 0;
        for (int c = 0; c < 3; ++c) {
            worst = std::max(worst, std::abs((int)withVelocity.Pixels[i * 3 + c] -
                                             (int)sharp.Pixels[i * 3 + c]));
        }
        if (worst > 2) ++touched;
    }
    const double fraction = (double)touched / (double)pixels;
    std::printf("       смаз тронул %.2f%% кадра\n", fraction * 100.0);
    Check(fraction > 0.002, "смаз виден");
    Check(fraction < 0.35, "смаз локален вокруг объекта, а не размазывает весь кадр");
}

void TestFxaa(FrameRenderer& r, Scene& scene) {
    sage::render::PostFXSettings fx = BaseSettings();
    fx.FxaaEnabled = true;
    const Image aa = RenderFrame(r, scene, PerspectiveProj(), fx, kW, kH);
    Report("scene_fxaa", CompareWithReference("scene_fxaa", aa));

    const Image raw = RenderFrame(r, scene, PerspectiveProj(), BaseSettings(), kW, kH);

    // Сглаживание обязано (а) что-то изменить и (б) изменить именно КРОМКИ, а
    // не всё подряд: если тронуто больше половины кадра — это уже замыливание.
    size_t touched = 0;
    long long sum = 0;
    const size_t pixels = aa.Pixels.size() / 3;
    for (size_t i = 0; i < pixels; ++i) {
        int worst = 0;
        for (int c = 0; c < 3; ++c) {
            const int d = std::abs((int)aa.Pixels[i * 3 + c] - (int)raw.Pixels[i * 3 + c]);
            sum += d;
            worst = std::max(worst, d);
        }
        if (worst > 2) ++touched;
    }
    const double fraction = (double)touched / (double)pixels;
    std::printf("       FXAA тронул %.2f%% пикселей, среднее изменение %.3f\n", fraction * 100.0,
                (double)sum / (double)aa.Pixels.size());
    Check(fraction > 0.001, "FXAA действительно что-то сглаживает");
    Check(fraction < 0.5, "FXAA правит кромки, а не мылит весь кадр");
}

// --- Сетка -----------------------------------------------------------------

// Сетка целиком живёт в шейдере: и линии, и затухание, и обрезка по радиусу
// считаются из точки пересечения луча с плоскостью. Проверить её значениями
// нечем — наружу она не отдаёт ни одного числа. Поэтому только кадр.
//
// Проверка «радиус отличается от бесконечной» здесь не формальность: ровно так
// ловится случай, когда сетка с радиусом гаснет целиком и кадр совпадает с
// кадром вообще без сетки.
// --- Прозрачность: порядок граней внутри объекта ---------------------------
//
// Прозрачные объекты сортируются МЕЖДУ СОБОЙ, но треугольники внутри одного
// объекта рисуются в порядке индексов — то есть в произвольном относительно
// камеры. При выключенном отсечении куб смешивал свои же грани как попало, и на
// экране это выглядело пятнами: одна половина плотнее другой.
//
// Правильная проверка здесь — НЕЗАВИСИМОСТЬ ОТ ПОРЯДКА. Если грани сводятся
// корректно (задние проходом раньше передних), то перестановка треугольников в
// буфере индексов не имеет права изменить кадр. Если же они рисуются как
// попало, перестановка его изменит — потому что именно порядок и определял
// результат. Тест не знает про реализацию: он спрашивает свойство.
void TestTransparentFaceOrder(FrameRenderer& r, Scene& scene) {
    sage::render::MeshData data = sage::render::BuildCube();

    // Тот же куб с ПЕРЕСТАВЛЕННЫМИ треугольниками. Обмотка каждого треугольника
    // сохраняется (иначе поменялось бы, где у него лицо) — меняется только
    // очерёдность их рисования.
    sage::render::MeshData shuffled = data;
    const size_t triCount = data.Indices.size() / 3;
    for (size_t t = 0; t < triCount; ++t) {
        const size_t src = triCount - 1 - t;
        for (int k = 0; k < 3; ++k) shuffled.Indices[t * 3 + k] = data.Indices[src * 3 + k];
    }

    auto renderWith = [&](const sage::render::MeshData& md) {
        Scene local;
        GameObject cube = local.CreateObject("Glass");
        MeshRendererComponent& mr = cube.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Cube, ""};
        mr.MeshPtr = std::make_shared<Mesh>(md.Vertices, md.Indices);
        mr.Color = glm::vec3(0.8f, 0.25f, 0.25f);
        mr.Opacity = 0.45f;   // полупрозрачный: именно здесь порядок и решает
        cube.GetTransform().Position = glm::vec3(0.0f, 0.0f, 0.0f);
        cube.GetTransform().Scale = glm::vec3(2.0f);
        return RenderFrame(r, local, PerspectiveProj(), BaseSettings(), kW, kH);
    };

    const Image a = renderWith(data);
    const Image b = renderWith(shuffled);

    long long sum = 0;
    for (size_t i = 0; i < a.Pixels.size() && i < b.Pixels.size(); ++i)
        sum += std::abs((int)a.Pixels[i] - (int)b.Pixels[i]);
    const double mean = (double)sum / (double)std::max<size_t>(a.Pixels.size(), 1);
    std::printf("       перестановка граней меняет кадр на %.3f\n", mean);
    Check(mean < 0.5, "кадр прозрачного объекта не зависит от порядка его треугольников");

    // И проверка, что объект вообще прозрачный: полностью непрозрачная копия
    // обязана дать ДРУГОЙ кадр. Иначе тест выше прошёл бы и на непрозрачном
    // кубе, где порядок не важен по определению и доказывать нечего.
    Scene opaqueScene;
    GameObject solid = opaqueScene.CreateObject("Solid");
    MeshRendererComponent& smr = solid.Renderer();
    smr.Ref = MeshRef{MeshRef::Type::Cube, ""};
    smr.MeshPtr = std::make_shared<Mesh>(data.Vertices, data.Indices);
    smr.Color = glm::vec3(0.8f, 0.25f, 0.25f);
    smr.Opacity = 1.0f;
    solid.GetTransform().Scale = glm::vec3(2.0f);
    const Image opaque = RenderFrame(r, opaqueScene, PerspectiveProj(), BaseSettings(), kW, kH);
    long long diff = 0;
    for (size_t i = 0; i < a.Pixels.size() && i < opaque.Pixels.size(); ++i)
        diff += std::abs((int)a.Pixels[i] - (int)opaque.Pixels[i]);
    const double opaqueMean = (double)diff / (double)std::max<size_t>(a.Pixels.size(), 1);
    std::printf("       прозрачный против непрозрачного: %.2f\n", opaqueMean);
    Check(opaqueMean > 2.0, "прозрачность действительно применяется");
}

// --- Свечение и ореол ------------------------------------------------------
//
// Emissive лежал в материале с самого начала, но НИКУДА не уходил: ни в один
// шейдер он не попадал, и выставленное свечение не меняло кадр вообще. Проверка
// спрашивает ровно два свойства, ради которых оно и нужно: светящийся объект
// ярче несветящегося, и при силе выше порога он даёт ОРЕОЛ ВОКРУГ СЕБЯ — то
// есть подхватывается bloom'ом.
void TestEmissive(FrameRenderer& r, Scene& scene) {
    auto build = [&](float strength) {
        Scene local;
        GameObject cube = local.CreateObject("Glow");
        MeshRendererComponent& mr = cube.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Cube, ""};
        mr.MeshPtr = std::make_shared<Mesh>(sage::render::BuildCube().Vertices,
                                            sage::render::BuildCube().Indices);
        mr.Color = glm::vec3(0.05f, 0.05f, 0.05f);   // тёмный: весь свет — от свечения
        auto mat = std::make_shared<Material>();
        mat->Albedo = glm::vec3(0.05f);
        mat->Emissive = glm::vec3(1.0f, 0.4f, 0.1f);
        mat->EmissiveStrength = strength;
        mr.MaterialPtr = mat;
        cube.GetTransform().Scale = glm::vec3(1.2f);
        return RenderFrame(r, local, PerspectiveProj(), BaseSettings(), kW, kH);
    };

    const Image dark = build(0.0f);
    const Image glow = build(4.0f);

    // Средняя яркость обязана вырасти: свечение — это добавленный свет.
    auto mean = [](const Image& im) {
        long long s = 0;
        for (unsigned char p : im.Pixels) s += p;
        return (double)s / (double)std::max<size_t>(im.Pixels.size(), 1);
    };
    const double dm = mean(dark), gm = mean(glow);
    std::printf("       средняя яркость: без свечения %.2f, со свечением %.2f\n", dm, gm);
    Check(gm > dm + 2.0, "свечение делает объект ярче");

    // Ореол: считаем прирост яркости на ФОНЕ — там, где в тёмном кадре почти
    // чёрное. Если бы свечение просто красило сам куб, фон не изменился бы, и
    // bloom остался бы неподключённым при формально «работающем» Emissive.
    //
    // Именно «фоновые пиксели», а не рамка по краям кадра: ореол — это десятки
    // пикселей вокруг силуэта, и полоса у границы кадра до него не достаёт.
    // Первая версия проверки мерила как раз её и показывала ноль на работающем
    // bloom'е.
    long long gain = 0;
    long long bgCount = 0;
    for (size_t i = 0; i + 2 < dark.Pixels.size() && i + 2 < glow.Pixels.size(); i += 3) {
        const int d = dark.Pixels[i] + dark.Pixels[i + 1] + dark.Pixels[i + 2];
        if (d > 24) continue;   // не фон — это сам куб или подсвеченная грань
        ++bgCount;
        gain += (glow.Pixels[i] + glow.Pixels[i + 1] + glow.Pixels[i + 2]) - d;
    }
    const double halo = (double)gain / (double)std::max(1LL, bgCount);
    std::printf("       прирост яркости на фоне (ореол): %.3f по %lld пикселям\n", halo, bgCount);
    Check(halo > 0.5, "свечение даёт ореол вокруг объекта (bloom подхватывает)");
}

// --- Наклейки ---------------------------------------------------------------
//
// Проверяется то, ради чего наклейки существуют: положенная на пол картинка
// ВИДНА в кадре и не дерётся с ним за глубину. Геометрия проекции проверена
// юнит-тестами (tests/test_decals.cpp); здесь — что она доезжает до экрана.
void TestDecals(FrameRenderer& r) {
    auto build = [&](bool withDecal) {
        Scene local;
        GameObject floor = local.CreateObject("Floor");
        MeshRendererComponent& fmr = floor.Renderer();
        fmr.Ref = MeshRef{MeshRef::Type::Cube, ""};
        fmr.MeshPtr = std::make_shared<Mesh>(sage::render::BuildCube().Vertices,
                                             sage::render::BuildCube().Indices);
        fmr.Color = glm::vec3(0.25f, 0.25f, 0.28f);
        floor.GetTransform().Scale = glm::vec3(8.0f, 0.5f, 8.0f);

        if (withDecal) {
            GameObject d = local.CreateObject("Decal");
            d.Renderer().Ref = MeshRef{MeshRef::Type::None, ""};
            // Ярко-красная наклейка на верхней грани (она на y = +0.25).
            d.Renderer().Color = glm::vec3(1.0f, 0.05f, 0.05f);
            d.GetTransform().Position = glm::vec3(0.0f, 0.25f, 0.0f);
            d.GetTransform().Rotation = glm::vec3(-90.0f, 0.0f, 0.0f);
            d.GetTransform().Scale = glm::vec3(3.0f);
            local.Registry().emplace<DecalComponent>(d.Entity());
            const sage::ecs::DecalBuildStats st =
                sage::ecs::BuildDecals(local, sage::ecs::MakeDecalMesh);
            Check(st.Triangles > 0, "наклейка построила геометрию по сцене");
        }
        sage::render::PostFXSettings fx = BaseSettings();
        fx.BloomEnabled = false;  // ореол размыл бы границу, которую меряем
        return RenderFrame(r, local, PerspectiveProj(), fx, kW, kH);
    };

    const Image plain = build(false);
    const Image decaled = build(true);

    // Красного в кадре с наклейкой обязано стать заметно больше. Считаем
    // пиксели, где красный ЯВНО перевешивает остальные каналы: пол серый, и
    // сам по себе таких пикселей не даёт.
    auto redPixels = [](const Image& im) {
        long long n = 0;
        for (size_t i = 0; i + 2 < im.Pixels.size(); i += 3) {
            const int rr = im.Pixels[i], gg = im.Pixels[i + 1], bb = im.Pixels[i + 2];
            if (rr > 90 && rr > gg * 2 && rr > bb * 2) ++n;
        }
        return n;
    };
    const long long before = redPixels(plain);
    const long long after = redPixels(decaled);
    std::printf("       красных пикселей: без наклейки %lld, с наклейкой %lld\n", before, after);
    Check(before < 50, "без наклейки красного в кадре практически нет");
    Check(after > before + 500, "наклейка видна в кадре");
}

void TestGrid(FrameRenderer& r, Scene& scene) {
    const glm::mat4 proj = PerspectiveProj();

    sage::render::GridSettings infinite;
    infinite.Mode = sage::render::GridSettings::Extent::Infinite;
    infinite.CellSize = 1.0f;
    infinite.MajorEvery = 10;
    infinite.ShowAxes = true;

    sage::render::GridSettings radius = infinite;
    radius.Mode = sage::render::GridSettings::Extent::Radius;
    radius.Radius = 5.0f; // заметно меньше пола (14 м), чтобы край окружности попал в кадр

    const Image plain = RenderFrame(r, scene, proj, BaseSettings(), kW, kH);
    const Image withInfinite = RenderFrame(r, scene, proj, BaseSettings(), kW, kH, &infinite);
    const Image withRadius = RenderFrame(r, scene, proj, BaseSettings(), kW, kH, &radius);

    Report("grid_infinite", CompareWithReference("grid_infinite", withInfinite));
    Report("grid_radius", CompareWithReference("grid_radius", withRadius));

    auto meanDiff = [](const Image& a, const Image& b) {
        long long sum = 0;
        for (size_t i = 0; i < a.Pixels.size(); ++i) {
            sum += std::abs((int)a.Pixels[i] - (int)b.Pixels[i]);
        }
        return (double)sum / (double)a.Pixels.size();
    };

    const double infiniteVsPlain = meanDiff(withInfinite, plain);
    const double radiusVsPlain = meanDiff(withRadius, plain);
    const double radiusVsInfinite = meanDiff(withRadius, withInfinite);
    std::printf("       сетка против пустого кадра: бесконечная %.2f, с радиусом %.2f; "
                "между собой %.2f\n",
                infiniteVsPlain, radiusVsPlain, radiusVsInfinite);
    Check(infiniteVsPlain > 1.0, "бесконечная сетка видна в кадре");
    // Порог у сетки с радиусом ниже не по слабости проверки: она занимает
    // пятно в центре кадра, а бесконечная — весь пол до горизонта, и одинаковое
    // среднее по кадру от них требовать нечестно. Отличить «видна» от «погасла
    // целиком» этого хватает с большим запасом: погасшая даёт ноль.
    Check(radiusVsPlain > 0.3, "сетка с радиусом видна в кадре");
    Check(radiusVsInfinite > 1.0, "радиус реально обрезает сетку, а не гасит её целиком");

    // Выключенная сетка обязана не оставить ни пикселя: иначе флаг показа —
    // ложь, и снять сетку с кадра было бы нечем.
    sage::render::GridSettings off = infinite;
    off.Enabled = false;
    Check(meanDiff(RenderFrame(r, scene, proj, BaseSettings(), kW, kH, &off), plain) < 0.001,
          "выключенная сетка не рисуется");

    // Прозрачность обязана влиять монотонно: половинная сетка ближе к пустому
    // кадру, чем полная.
    sage::render::GridSettings faint = infinite;
    faint.Opacity = 0.35f;
    const double faintVsPlain =
        meanDiff(RenderFrame(r, scene, proj, BaseSettings(), kW, kH, &faint), plain);
    std::printf("       полупрозрачная сетка против пустого кадра: %.2f\n", faintVsPlain);
    Check(faintVsPlain > 0.05 && faintVsPlain < infiniteVsPlain,
          "прозрачность сетки ослабляет её, но не убирает");

    // Шаг клетки обязан менять картинку: без этого настройка была бы мёртвой.
    sage::render::GridSettings coarse = infinite;
    coarse.CellSize = 4.0f;
    Check(meanDiff(RenderFrame(r, scene, proj, BaseSettings(), kW, kH, &coarse), withInfinite) > 0.5,
          "шаг клетки меняет сетку");
}

} // namespace

void RunFrameChecks(FrameRenderer& r, Scene& scene) {
    TestScenePerspective(r, scene);
    TestShadowsOffIsNotBlack(r, scene);
    TestSceneOrthographic(r, scene);
    TestNoPostFX(r, scene);
    TestDepthOfField(r, scene);
    TestFxaa(r, scene);
    TestTransparentFaceOrder(r, scene);
    TestEmissive(r, scene);
    TestGrid(r, scene);
    TestDecals(r);
    TestObjectMotionBlur(r);
    TestMsaa(r);
}

} // namespace sage::rendertest
