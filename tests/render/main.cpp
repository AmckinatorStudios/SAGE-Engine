// ============================================================================
//  sage_render_tests — проверка КАРТИНКИ по эталонным кадрам.
//
//  Обычные тесты движка проверяют математику, сериализацию и анимацию. Ошибку в
//  шейдере они не увидят: код собирается, значения считаются, а кадр выглядит
//  иначе. Здесь сцена рисуется в offscreen-буфер и сравнивается с образцом.
//
//  Тест ТРЕБУЕТ графического контекста, поэтому живёт отдельной целью и в CI
//  запускается под xvfb. Про допуски и переносимость эталонов — см. подробный
//  комментарий в RenderTestHarness.h.
// ============================================================================
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include "RenderTestHarness.h"

#include "sage/core/Log.h"
#include "sage/core/Window.h"
#include "sage/ecs/LightSystem.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/ecs/RenderBatch.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/GridRenderer.h"
#include "sage/ecs/DecalSystem.h"
#include "sage/render/PostFX.h"
#include "sage/rhi/Conformance.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "sage/assets/AssetCache.h"
#include "sage/render/SkinnedModel.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/ShadowAtlas.h"
#include "sage/render/ShadowMap.h"
#include "sage/render/SkyRenderer.h"
#include "sage/render/Reflection.h"
#include "sage/render/SkinnedModel.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

using namespace sage::rendertest;

namespace {

int g_passed = 0;
int g_failed = 0;
int g_written = 0;

// Допуски. Сознательно не нулевые: разные драйверы дают разные последние биты,
// и требовать побитового совпадения значило бы получить тест, который «падает
// сам по себе» и которому перестают верить.
constexpr double kMaxMeanDiff = 1.5;   // средняя разница по каналам, 0..255
constexpr double kMaxDiffFraction = 0.02; // не больше 2% заметно отличающихся пикселей

void Report(const std::string& name, const Comparison& c) {
    if (!c.ReferenceExisted) {
        std::printf("[ new] %-28s эталон записан (сравнивать было не с чем)\n", name.c_str());
        ++g_written;
        return;
    }
    const bool ok = c.MeanDiff <= kMaxMeanDiff && c.DiffFraction <= kMaxDiffFraction;
    if (ok) {
        ++g_passed;
        std::printf("[ ok ] %-28s среднее %.3f, худший канал %d, отличается %.2f%%\n",
                    name.c_str(), c.MeanDiff, c.MaxDiff, c.DiffFraction * 100.0);
    } else {
        ++g_failed;
        std::printf("[FAIL] %-28s среднее %.3f (допуск %.2f), худший канал %d, "
                    "отличается %.2f%% (допуск %.2f%%)\n",
                    name.c_str(), c.MeanDiff, kMaxMeanDiff, c.MaxDiff,
                    c.DiffFraction * 100.0, kMaxDiffFraction * 100.0);
        std::printf("       рядом с эталоном лежат %s.actual.png и %s.diff.png\n",
                    name.c_str(), name.c_str());
    }
}

// Обычная проверка на значение — для тех утверждений, которые видны в цифрах, а
// не в картинке (например, «сглаживание изменило именно кромки»).
void Check(bool condition, const char* what) {
    if (condition) {
        ++g_passed;
        std::printf("[ ok ] %s\n", what);
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what);
    }
}

// --- Сцена -----------------------------------------------------------------

// Одна и та же сцена для всех кадров: несколько примитивов с разными цветами,
// пол и наклонное солнце. Наклон намеренный — прямой свет сверху не показал бы
// ни теней, ни градиента на сферах.
std::unique_ptr<Scene> MakeScene() {
    auto scene = std::make_unique<Scene>("RenderTest");

    scene->Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.35f));
    scene->Lighting.Sun.Intensity = 1.2f;
    scene->Lighting.Sun.Color = {1.0f, 0.97f, 0.9f};
    scene->Lighting.SkyColor = {0.40f, 0.48f, 0.64f};
    scene->Lighting.GroundColor = {0.20f, 0.17f, 0.15f};
    scene->Lighting.AmbientStrength = 0.35f;
    scene->Lighting.Skybox.Enabled = false; // небо здесь не проверяем — только геометрию и свет

    // Ref задаёт, ЧТО рисовать, а MeshPtr — чем: GPU-меш ставится
    // ResourceManager'ом (в обычной жизни это делает загрузчик сцены). Без
    // этого шага сущность есть, а рисовать нечего — кадр выйдет пустым.
    auto primitive = [](GameObject obj, MeshRef::Type type, glm::vec3 color) {
        obj.Renderer().Ref = MeshRef{type};
        obj.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(type);
        obj.Renderer().Color = color;
    };

    GameObject ground = scene->CreateObject("Ground");
    ground.GetTransform().Position = {0.0f, 0.0f, 0.0f};
    ground.GetTransform().Scale = {14.0f, 1.0f, 14.0f};
    primitive(ground, MeshRef::Type::Plane, {0.34f, 0.35f, 0.37f});

    GameObject cube = scene->CreateObject("Cube");
    cube.GetTransform().Position = {-1.6f, 0.5f, 0.0f};
    cube.GetTransform().Rotation = {0.0f, 24.0f, 0.0f};
    primitive(cube, MeshRef::Type::Cube, {0.80f, 0.45f, 0.22f});

    GameObject sphere = scene->CreateObject("Sphere");
    sphere.GetTransform().Position = {1.0f, 0.75f, 0.4f};
    sphere.GetTransform().Scale = {1.5f, 1.5f, 1.5f};
    primitive(sphere, MeshRef::Type::Sphere, {0.30f, 0.55f, 0.85f});

    GameObject pillar = scene->CreateObject("Pillar");
    pillar.GetTransform().Position = {2.6f, 1.2f, -2.0f};
    pillar.GetTransform().Scale = {0.5f, 2.4f, 0.5f};
    primitive(pillar, MeshRef::Type::Cube, {0.75f, 0.75f, 0.78f});

    return scene;
}

// Камера кадра. Одна и та же точка съёмки для всех тестов, чтобы отличия между
// эталонами были только от того, что тест и проверяет.
const glm::vec3 kEye(4.2f, 3.4f, 5.6f);
const glm::vec3 kTarget(0.0f, 0.8f, 0.0f);

glm::mat4 TestView() { return glm::lookAt(kEye, kTarget, glm::vec3(0, 1, 0)); }

// Рисует сцену в буфер и прогоняет пост-обработку в него же. Возвращает готовый
// кадр. Это ровно та же последовательность, что в редакторе и в инструменте:
// тени -> цветной проход -> PostFX.
// Владелец GPU-объектов прохода. Именно ВЛАДЕЛЕЦ, а не набор статиков внутри
// функции: статик разрушился бы после выхода из main, когда графического
// контекста уже нет, — и удаление буферов упало бы (проверено падением).
struct FrameRenderer {
    sage::ecs::RenderBatch Batch;
    ShadowMap Shadow{1024};
    sage::render::PostFX Fx;
    sage::render::GridRenderer Grid;
};

// grid — рисовать ли сетку поверх геометрии (nullptr — не рисовать). Сетка
// идёт ДО пост-обработки и после геометрии: ровно там же, где во вьюпорте
// инструмента, иначе тест проверял бы не тот путь.
// shadowRadius — полусторона ортобокса теней. Параметр, а не константа: от
// него напрямую зависит размер текселя, а значит и то, ВИДНО ли работу
// фильтра. На тесной коробке фильтровать почти нечего.
Image RenderFrame(FrameRenderer& r, Scene& scene, const glm::mat4& proj,
                  const sage::render::PostFXSettings& fx, int width, int height,
                  const sage::render::GridSettings* grid = nullptr,
                  float shadowRadius = 12.0f) {
    Framebuffer sceneFbo(width, height);
    Framebuffer output(width, height);

    const LightingEnvironment env = sage::ecs::CollectLighting(scene);
    const glm::mat4 view = TestView();

    // Тени: бокс света центрируем на сцене, радиус с запасом на пол.
    r.Shadow.SetLightMatrix(env.Sun.Direction, glm::vec3(0.0f, 0.0f, 0.0f), shadowRadius);
    r.Shadow.BeginRender();
    r.Batch.RenderDepth(scene, r.Shadow.LightMatrix());
    r.Shadow.EndRender(width, height);

    sceneFbo.Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
    device.Clear(true, true);
    r.Batch.RenderColor(scene, view, proj, kEye, env, ShadowBinding(r.Shadow, true), 0);
    // Скелетные модели рисуются отдельным проходом (свой шейдер со скиннингом),
    // как и в редакторе с инструментом.
    sage::anim::DrawAnimatedModels(scene, view, proj, kEye, env, ShadowBinding(r.Shadow, true));

    if (grid) r.Grid.Draw(view, proj, kEye, *grid);

    // Смаз движения опирается на историю кадров, а тест должен быть
    // детерминированным при любом порядке запуска — сбрасываем её явно.
    r.Fx.ResetHistory();
    r.Fx.Render(sceneFbo.ColorTexture(), sceneFbo.DepthTexture(), width, height, proj, view, fx,
                  &output, 0, 0, width, height);

    output.Bind();
    return Capture(width, height);
}

// --- Тесты -----------------------------------------------------------------

constexpr int kW = 320;
constexpr int kH = 240;

sage::render::PostFXSettings BaseSettings() {
    sage::render::PostFXSettings fx;
    // Значения фиксированы в тесте, а не взяты из конфига: иначе правка
    // дефолтов конфига «ломала» бы эталоны, ничего не сломав в рендере.
    fx.Enabled = true;
    fx.Exposure = 1.05f;
    fx.Gamma = 2.2f;
    fx.Saturation = 1.16f;
    fx.Contrast = 1.06f;
    fx.BloomEnabled = true;
    fx.BloomThreshold = 1.0f;
    fx.BloomIntensity = 0.55f;
    fx.AOEnabled = true;
    fx.AORadius = 0.5f;
    fx.AOStrength = 1.0f;
    fx.Vignette = 0.35f;
    fx.FxaaEnabled = false; // включается отдельным тестом
    return fx;
}

glm::mat4 PerspectiveProj() {
    CameraComponent cam;
    cam.Fov = 50.0f;
    cam.NearClip = 0.1f;
    cam.FarClip = 100.0f;
    return cam.ProjectionMatrix((float)kW / (float)kH);
}

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
        ++g_passed;
        std::printf("[ ok ] %-28s среднее %.1f (сцена освещена)\n", "shadows_off_not_black", mean);
    } else {
        ++g_failed;
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
        ++g_failed;
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
        ++g_failed;
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

// --- Отражения -------------------------------------------------------------

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

} // namespace

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
        ++g_failed;
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

// --- Мягкость теней --------------------------------------------------------

// Эталонные кадры к качеству теней ПОЧТИ НЕ ЧУВСТВИТЕЛЬНЫ: правка кромок
// задевает полпроцента пикселей и укладывается в допуск. То есть тени могли бы
// вернуться к лесенке, и ни один тест этого не заметил бы.
//
// Здесь меряется то, что и отличает хорошую тень от плохой: доля ПЛАВНЫХ
// переходов яркости на её кромке. У жёсткой тени переход занимает один пиксель
// (резкий скачок), у фильтрованной — несколько (череда промежуточных значений).
void TestShadowSoftness(FrameRenderer& r, Scene& scene) {
    // Радиус НАРОЧНО большой. При тесной коробке (12 м на карту 2048) тексель
    // и так меньше пикселя, фильтровать нечего, и старый жёсткий вариант
    // показывает ровно те же числа — первая версия этой проверки на том и
    // провалилась: она проходила и до правки. Сорок метров дают тексель в
    // четыре сантиметра, то есть ровно тот случай, ради которого фильтр и
    // делался.
    const Image frame = RenderFrame(r, scene, PerspectiveProj(), BaseSettings(), kW, kH,
                                    nullptr, /*shadowRadius=*/40.0f);

    int hard = 0, soft = 0;
    for (int y = frame.Height / 2; y < frame.Height; ++y) {
        for (int x = 0; x + 1 < frame.Width; ++x) {
            const int a = frame.Pixels[(size_t)(y * frame.Width + x) * 3];
            const int b = frame.Pixels[(size_t)(y * frame.Width + x + 1) * 3];
            const int d = std::abs(a - b);
            if (d > 18) ++hard;
            else if (d >= 3) ++soft;
        }
    }
    const double smoothShare = 100.0 * soft / std::max(hard + soft, 1);
    std::printf("    кромки теней: резких %d, плавных %d (%.1f%% плавных)\n",
                hard, soft, smoothShare);

    // Порог проверен С ОБЕИХ СТОРОН, а не подобран от достигнутого: на этой
    // сцене жёсткое сравнение даёт 79.6%, билинейное — 89.3%. Планка в 85%
    // лежит между ними, то есть возврат к жёсткому сравнению её действительно
    // роняет. Первая версия ставила 75% и проходила в обоих случаях — такая
    // проверка не проверяет ничего.
    Check(smoothShare > 85.0, "кромка тени фильтруется, а не ступенчатая");
}

// --- Каскадные тени --------------------------------------------------------

// Длинная сцена: три широкие стены на разной дальности, каждая кладёт на землю
// длинную тень. Обычная тестовая сцена для каскадов не годится — она вся
// умещается в ОДИН ближний каскад, и проверка на ней проходит даже с полностью
// отключённым выбором каскада (проверено: отключил — тест не заметил).
//
// Стены, а не столбы: тень столба на семидесяти метрах занимает десяток
// пикселей, и померить её нечем. Тень стены — широкая полоса, которую видно и
// на маленьком тестовом кадре.
std::unique_ptr<Scene> BuildLongScene() {
    auto scene = std::make_unique<Scene>("CascadeTest");
    // Свет летит В СТОРОНУ КАМЕРЫ (+z): иначе тени ложатся за стены, где их с
    // этой точки съёмки просто не видно, и мерить нечего. На первом заходе
    // солнце светило от камеры — кадр вышел почти без теней.
    scene->Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.55f, -0.70f, 0.45f));
    // Солнце ярче обычного, ambient слабее: мерить будем разницу освещённого и
    // затенённого, и чем она крупнее в восьми битах, тем меньше шума в числах.
    scene->Lighting.Sun.Intensity = 3.0f;
    scene->Lighting.Sun.Color = {1.0f, 0.98f, 0.94f};
    scene->Lighting.SkyColor = {0.40f, 0.48f, 0.64f};
    scene->Lighting.GroundColor = {0.20f, 0.17f, 0.15f};
    scene->Lighting.AmbientStrength = 0.15f;
    scene->Lighting.Skybox.Enabled = false;

    auto primitive = [](GameObject obj, MeshRef::Type type, glm::vec3 color) {
        obj.Renderer().Ref = MeshRef{type};
        obj.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(type);
        obj.Renderer().Color = color;
    };

    GameObject ground = scene->CreateObject("Ground");
    ground.GetTransform().Position = {0.0f, 0.0f, -50.0f};
    ground.GetTransform().Scale = {60.0f, 1.0f, 120.0f};
    primitive(ground, MeshRef::Type::Plane, {0.62f, 0.63f, 0.65f});

    // Стены на 8, 35 и 75 метрах: ближний, средний и дальний каскады.
    const float kZ[] = {-8.0f, -35.0f, -75.0f};
    for (int i = 0; i < 3; ++i) {
        GameObject wall = scene->CreateObject("Wall" + std::to_string(i));
        wall.GetTransform().Position = {2.0f, 2.5f, kZ[i]};
        wall.GetTransform().Scale = {10.0f, 5.0f, 0.8f};
        primitive(wall, MeshRef::Type::Cube, {0.78f, 0.76f, 0.72f});
    }
    return scene;
}

// Камера длинной сцены: высоко и с наклоном вниз, чтобы все три тени лежали в
// кадре как полосы на земле.
const glm::vec3 kLongEye(0.0f, 14.0f, 22.0f);
const glm::vec3 kLongTarget(0.0f, 0.0f, -55.0f);

// Рисует кадр ЗАДАННЫМ набором карт теней. Отдельно от RenderFrame, потому что
// проверять надо именно карты: всё остальное в кадре должно совпадать до
// пикселя, иначе разница окажется не про каскады. Пост-обработки нет намеренно:
// тон-маппинг сжимает разницу яркостей, а мерить мы собираемся именно её.
Image RenderWithShadows(FrameRenderer& r, Scene& scene, ShadowMap& shadows,
                        const glm::mat4& view, const glm::mat4& proj, const glm::vec3& eye) {
    Framebuffer sceneFbo(kW, kH);
    const LightingEnvironment env = sage::ecs::CollectLighting(scene);

    for (int c = 0; c < shadows.CascadeCount(); ++c) {
        shadows.BeginRender(c);
        r.Batch.RenderDepth(scene, shadows.LightMatrix(c));
    }
    shadows.EndRender(kW, kH);

    sceneFbo.Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
    device.Clear(true, true);
    r.Batch.RenderColor(scene, view, proj, eye, env, ShadowBinding(shadows, true), 0);
    sceneFbo.Resolve();
    sceneFbo.Bind();
    return Capture(kW, kH);
}

// Сколько пикселей ОСВЕЩЁННОЙ земли базового кадра ушло в тень на проверяемом.
// Прямой счёт площади новой тени.
//
// Первая версия мерила «суммарную недостачу яркости по всему кадру», и это не
// работало: в сумму входили тёмное небо и сами стены, на их фоне дальняя тень
// давала меньше процента, то есть тонула в постоянном слагаемом. Считать надо
// то, что изменилось, а не то, что есть.
long long NewlyShadowed(const Image& base, const Image& test) {
    long long count = 0;
    const size_t n = std::min(base.Pixels.size(), test.Pixels.size());
    for (size_t i = 0; i < n; i += 3) {
        const int b = base.Pixels[i];
        const int t = test.Pixels[i];
        // 100 отсекает небо и тёмные грани стен: интересует только земля,
        // которая была освещена. 30 — уверенное потемнение, а не шум сжатия.
        if (b > 100 && b - t > 30) ++count;
    }
    return count;
}

// Каскады проверяются с ДВУХ сторон, и обе нужны.
//
// 1. Тень есть ВДАЛИ. Это проверка ВЫБОРА каскада: если шейдер всегда смотрит в
//    нулевую карту, дальние стены теряют тень. Первая версия этого теста мерила
//    только подробность вблизи — и проходила с намеренно сломанным выбором
//    каскада, то есть не проверяла главного.
// 2. Вблизи тень ПОДРОБНЕЕ. Одна карта на всю даль даёт тексель в десяток
//    сантиметров, и ближняя тень едет. Эталон подробности — тесная карта,
//    натянутая только на ближний кусок.
void TestShadowCascades(FrameRenderer& r) {
    std::unique_ptr<Scene> scene = BuildLongScene();
    const glm::mat4 view = glm::lookAt(kLongEye, kLongTarget, glm::vec3(0, 1, 0));
    const glm::mat4 proj =
        glm::perspective(glm::radians(45.0f), (float)kW / (float)kH, 0.1f, 400.0f);

    ShadowMap::CameraView v;
    v.Position = kLongEye;
    v.Forward = glm::normalize(kLongTarget - kLongEye);
    v.Up = glm::vec3(0.0f, 1.0f, 0.0f);
    v.FovY = glm::radians(45.0f);
    v.Aspect = (float)kW / (float)kH;
    v.Near = 0.1f;
    v.ShadowDistance = 140.0f;

    ShadowMap cascaded(1024, 3);
    cascaded.SetCascades(scene->Lighting.Sun.Direction, v);
    const Image multi = RenderWithShadows(r, *scene, cascaded, view, proj, kLongEye);

    // Одна карта РОВНО в размер ближнего каскада: вблизи она идеальна (это и
    // есть эталон подробности), вдаль не достаёт вовсе.
    ShadowMap nearOnly(1024, 1);
    nearOnly.SetLightMatrix(scene->Lighting.Sun.Direction,
                            kLongEye + v.Forward * cascaded.CascadeRadius(0),
                            cascaded.CascadeRadius(0));
    const Image nearImage = RenderWithShadows(r, *scene, nearOnly, view, proj, kLongEye);

    // Одна карта на всю даль: тени есть везде, но тексель крупный.
    ShadowMap wide(1024, 1);
    wide.SetLightMatrix(scene->Lighting.Sun.Direction, kLongEye + v.Forward * 70.0f, 80.0f);
    const Image wideImage = RenderWithShadows(r, *scene, wide, view, proj, kLongEye);

    if (std::getenv("SAGE_DUMP_CASCADES")) {
        SavePng("cascade_multi.png", multi);
        SavePng("cascade_near.png", nearImage);
        SavePng("cascade_wide.png", wideImage);
    }

    std::printf("    тексель каскадов: %.1f / %.1f / %.1f см; одна карта на ту же даль: %.1f см\n",
                cascaded.CascadeTexelSize(0) * 100.0, cascaded.CascadeTexelSize(1) * 100.0,
                cascaded.CascadeTexelSize(2) * 100.0, wide.CascadeTexelSize(0) * 100.0);

    // --- 1. Тень вдали (проверка ВЫБОРА каскада) ---------------------------
    // За базу берём кадр с одной ближней картой: в нём дальних теней нет
    // вовсе. Всё, что потемнело относительно него, — это и есть дальняя тень.
    const long long farWide = NewlyShadowed(nearImage, wideImage);
    const long long farMulti = NewlyShadowed(nearImage, multi);
    std::printf("    площадь дальней тени (пикселей): широкая карта %lld, каскады %lld\n",
                farWide, farMulti);

    // Сначала убеждаемся, что сцена вообще годится для этой проверки: широкой
    // карте есть что нарисовать вдали. Иначе следующая проверка ничего не
    // проверяет — ровно эта ошибка и была в первой версии теста.
    Check(farWide > 300, "сцена действительно проверяет дальнюю тень");
    // Каскады обязаны вернуть большую часть той тени, до которой ближняя карта
    // не достаёт. При сломанном выборе каскада здесь будет около нуля.
    Check(farMulti > farWide / 2, "каскады рисуют тень ВДАЛИ — выбор каскада работает");

    // --- 2. Подробность вблизи ---------------------------------------------
    // Проверяется размером текселя, а не пикселями кадра. Причина: разница
    // между грубой и подробной тенью живёт на КРОМКЕ, а кромка занимает доли
    // процента кадра — попиксельная сумма её не видит (пробовал: 105275 против
    // 109817, то есть 4% при пятикратной разнице в текселе). Размер текселя —
    // ровно та величина, которая определяет предел подробности, и меряется
    // она точно.
    Check(cascaded.CascadeTexelSize(0) < wide.CascadeTexelSize(0) * 0.35f,
          "ближний каскад втрое с лишним мельче текселем, чем одна карта на ту же даль");

    // Каскады обязаны РАСТИ: если дальний не крупнее ближнего, деление
    // дальности не сработало и все карты накрывают одно и то же.
    Check(cascaded.CascadeRadius(2) > cascaded.CascadeRadius(0) * 2.0f,
          "дальний каскад заметно больше ближнего");
}

// --- Уровни детализации ----------------------------------------------------

// Аллея одинаковых сфер, уходящая вдаль. Именно то, на чём LOD должен работать:
// ближние сферы занимают полкадра, дальние — единицы пикселей, и рисовать их
// одной и той же геометрией на тридцать тысяч треугольников бессмысленно.
std::unique_ptr<Scene> BuildLodScene(std::shared_ptr<Mesh>& sphere, bool withLod) {
    auto scene = std::make_unique<Scene>("LodTest");
    scene->Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.4f, -0.9f, 0.3f));
    scene->Lighting.Sun.Intensity = 2.2f;
    scene->Lighting.AmbientStrength = 0.25f;
    scene->Lighting.Skybox.Enabled = false;

    // Сферу строим САМИ, с копией геометрии: уровни выводятся из неё, а
    // ResourceManager отдаёт примитивы без копии (и правильно делает — платить
    // памятью за неё во всей сцене незачем).
    if (!sphere) sphere = std::make_shared<Mesh>(Mesh::CreateSphere(48, 64, /*keepCpuData=*/true));

    sage::render::LodComponent lodProto;
    if (withLod) sage::render::BuildAutoLods(lodProto, *sphere, 2);

    for (int i = 0; i < 14; ++i) {
        GameObject obj = scene->CreateObject("Sphere" + std::to_string(i));
        // Шаг по глубине растущий: так в кадре оказываются и крупные сферы, и
        // те, что упираются в порог отсечения по размеру.
        obj.GetTransform().Position = {(i % 2 ? 1.6f : -1.6f), 0.6f,
                                       -3.0f - 2.2f * (float)(i * i) / 3.0f};
        // Дальние сферы ещё и мельче — как реквизит в настоящей сцене. Без
        // этого порог отсечения по размеру недостижим: сфера радиуса полметра
        // остаётся крупнее трёх пикселей до самой дальней плоскости отсечения,
        // то есть проверить отсечение мелочи было бы не на чем.
        if (i >= 9) obj.GetTransform().Scale = glm::vec3(0.12f);
        obj.Renderer().Ref = MeshRef{MeshRef::Type::Sphere};
        obj.Renderer().MeshPtr = sphere;
        obj.Renderer().Color = {0.72f, 0.68f, 0.6f};
        if (withLod) scene->Registry().emplace<sage::render::LodComponent>(obj.Entity(), lodProto);
    }
    return scene;
}

const glm::vec3 kLodEye(0.0f, 2.0f, 4.0f);
const glm::vec3 kLodTarget(0.0f, 0.6f, -40.0f);

// Уровни детализации должны экономить И оставаться незаметными. Проверять
// только первое нельзя: «нарисовать всё кубиками» тоже экономит.
void TestLevelsOfDetail(FrameRenderer& r) {
    const glm::mat4 view = glm::lookAt(kLodEye, kLodTarget, glm::vec3(0, 1, 0));
    const glm::mat4 proj =
        glm::perspective(glm::radians(50.0f), (float)kW / (float)kH, 0.1f, 400.0f);

    std::shared_ptr<Mesh> sphere;
    auto render = [&](bool withLod, sage::ecs::RenderStats& stats) {
        std::unique_ptr<Scene> scene = BuildLodScene(sphere, withLod);
        Framebuffer fbo(kW, kH);
        const LightingEnvironment env = sage::ecs::CollectLighting(*scene);
        fbo.Bind();
        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
        device.Clear(true, true);
        stats = r.Batch.RenderColor(*scene, view, proj, kLodEye, env, ShadowBinding(), 0);
        fbo.Resolve();
        fbo.Bind();
        return Capture(kW, kH);
    };

    sage::ecs::RenderStats full{}, lod{};
    const Image imageFull = render(/*withLod=*/false, full);
    const Image imageLod = render(/*withLod=*/true, lod);

    std::printf("    треугольников: без уровней %lld, с уровнями %lld (%.1f%%); "
                "отсечено по размеру %d из %d\n",
                full.Triangles, lod.Triangles,
                full.Triangles > 0 ? 100.0 * (double)lod.Triangles / (double)full.Triangles : 0.0,
                lod.CulledTiny, lod.Total);

    // Сцена обязана быть тяжёлой: на десяти треугольниках экономить нечего, и
    // проверка ничего не значила бы.
    Check(full.Triangles > 50000, "сцена достаточно тяжёлая, чтобы на ней было что экономить");
    // Экономия должна быть КРУПНОЙ. Половина — не «побольше нуля», а порог, ниже
    // которого уровни детализации не окупают своей сложности.
    Check(lod.Triangles * 2 < full.Triangles, "уровни детализации срезают больше половины треугольников");
    // И отдельно — что отсечение мелочи вообще срабатывает.
    Check(lod.CulledTiny > 0, "объекты мельче порога не рисуются вовсе");

    // Незаметность. Считаем долю пикселей, изменившихся заметно: подмена
    // геометрии на дальних сферах обязана прятаться в единицы процентов кадра.
    long long changed = 0;
    for (size_t i = 0; i < imageFull.Pixels.size(); i += 3) {
        if (std::abs((int)imageFull.Pixels[i] - (int)imageLod.Pixels[i]) > 24) ++changed;
    }
    const double share = 100.0 * (double)changed / (double)(kW * kH);
    std::printf("    кадр изменился на %.2f%% пикселей\n", share);
    Check(share < 4.0, "подмена геометрии не видна: изменилось меньше 4% кадра");
}

// --- Отсечение перекрытием -------------------------------------------------

// Глухая стена вплотную к камере и толпа объектов за ней. Ровно тот случай,
// ради которого проверка перекрытия и существует: фрустум их не отсекает — они
// в поле зрения, — и без проверки все они честно рисуются в пиксели, которые
// тут же перезаписывает стена.
std::unique_ptr<Scene> BuildOccludedScene() {
    auto scene = std::make_unique<Scene>("OcclusionTest");
    scene->Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.3f, -0.9f, -0.2f));
    scene->Lighting.Sun.Intensity = 2.0f;
    scene->Lighting.AmbientStrength = 0.3f;
    scene->Lighting.Skybox.Enabled = false;

    auto primitive = [](GameObject obj, MeshRef::Type type, glm::vec3 color) {
        obj.Renderer().Ref = MeshRef{type};
        obj.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(type);
        obj.Renderer().Color = color;
    };

    GameObject wall = scene->CreateObject("Wall");
    wall.GetTransform().Position = {0.0f, 0.0f, -4.0f};
    wall.GetTransform().Scale = {40.0f, 40.0f, 0.5f};
    primitive(wall, MeshRef::Type::Cube, {0.55f, 0.53f, 0.5f});

    // Три куба ПЕРЕД стеной. Без них проверка «кадр не изменился» слепа:
    // единственным видимым объектом осталась бы сама стена, а она освобождена
    // от запросов (камера внутри её габаритов), и тест прошёл бы даже если
    // объявить закрытым вообще всё. Проверено — проходил.
    for (int i = 0; i < 3; ++i) {
        GameObject front = scene->CreateObject("Front" + std::to_string(i));
        front.GetTransform().Position = {-1.6f + 1.6f * (float)i, 0.0f, -2.0f};
        front.GetTransform().Scale = glm::vec3(0.7f);
        primitive(front, MeshRef::Type::Cube, {0.25f, 0.7f, 0.85f});
    }

    // 40 кубов ЗА стеной, в поле зрения камеры.
    for (int i = 0; i < 40; ++i) {
        GameObject box = scene->CreateObject("Hidden" + std::to_string(i));
        box.GetTransform().Position = {-6.0f + 0.6f * (float)(i % 20), -2.0f + 2.5f * (float)(i / 20),
                                       -8.0f - 0.4f * (float)(i % 7)};
        box.GetTransform().Scale = glm::vec3(0.5f);
        primitive(box, MeshRef::Type::Cube, {0.8f, 0.4f, 0.3f});
    }
    return scene;
}

// Проверка перекрытия обязана убрать закрытое И не тронуть открытое. Вторая
// половина не менее важна первой: «отсекает всё» — это не оптимизация, а
// пропавшая геометрия.
void TestOcclusionCulling(FrameRenderer& r) {
    std::unique_ptr<Scene> scene = BuildOccludedScene();
    const glm::vec3 eye(0.0f, 0.0f, 1.0f);
    const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f, 0.0f, -10.0f), glm::vec3(0, 1, 0));
    const glm::mat4 proj =
        glm::perspective(glm::radians(60.0f), (float)kW / (float)kH, 0.1f, 200.0f);

    Framebuffer fbo(kW, kH);
    const LightingEnvironment env = sage::ecs::CollectLighting(*scene);
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();

    auto frame = [&](sage::ecs::RenderStats& stats) {
        fbo.Bind();
        device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
        device.Clear(true, true);
        stats = r.Batch.RenderColor(*scene, view, proj, eye, env, ShadowBinding(), 0);
        r.Batch.RenderOcclusionProbes(proj * view, eye);
        stats = r.Batch.Stats(); // счётчики проверки перекрытия — только после прохода
        fbo.Resolve();
        fbo.Bind();
        return Capture(kW, kH);
    };

    // Без проверки: рисуется всё, что в поле зрения.
    r.Batch.SetOcclusionCulling(false);
    sage::ecs::RenderStats off{};
    const Image imageOff = frame(off);

    // С проверкой. Первый кадр выдаёт запросы, ответы приходят к следующему,
    // поэтому кадров нужно несколько — и это не подгонка под тест, а то самое
    // запаздывание на кадр, о котором сказано в RenderBatch.h.
    r.Batch.SetOcclusionCulling(true);
    sage::ecs::RenderStats on{};
    Image imageOn;
    for (int i = 0; i < 6; ++i) imageOn = frame(on);

    std::printf("    нарисовано объектов: без проверки %d, с проверкой %d "
                "(закрытыми признано %d, коробок проверено %d)\n",
                off.Drawn, on.Drawn, on.CulledOccluded, on.Probes);

    if (!device.SupportsOcclusionQueries()) {
        std::printf("    ПРОПУСК: драйвер не умеет запросов перекрытия\n");
        return;
    }

    // Сцена обязана быть «закрытой»: без этого проверять нечего.
    Check(off.Drawn >= 44, "сцена содержит стену, объекты перед ней и толпу за ней");
    // Большая часть спрятанного должна отсечься. Не всё: часть кубов торчит
    // из-за краёв стены, и коробка-заменитель заведомо больше объекта —
    // обе ошибки в безопасную сторону «нарисуем лишнее».
    Check(on.CulledOccluded > 25, "закрытые стеной объекты не рисуются");
    Check(on.Drawn < off.Drawn / 2, "отрисовка сократилась больше чем вдвое");
    // Видимое обязано остаться: кубы перед стеной никуда деться не могут.
    Check(on.Drawn >= 4, "объекты перед стеной продолжают рисоваться");

    // И главное: кадр не изменился. Если бы отсекалось лишнее, картинка
    // поехала бы — а «отсекает всё» это не оптимизация, а пропавшая геометрия.
    long long changed = 0;
    for (size_t i = 0; i < imageOff.Pixels.size(); i += 3) {
        if (std::abs((int)imageOff.Pixels[i] - (int)imageOn.Pixels[i]) > 12) ++changed;
    }
    const double share = 100.0 * (double)changed / (double)(kW * kH);
    std::printf("    кадр изменился на %.3f%% пикселей\n", share);
    Check(share < 0.5, "кадр не изменился: отсеклось только невидимое");
}

// --- Кэш ресурсов ----------------------------------------------------------

// Кэш обязан давать РОВНО ТУ ЖЕ модель, что и разбор исходника. Проверять
// только скорость бессмысленно: быстрый кэш, отдающий чуть другие вершины, —
// это тихая порча ассетов, которую заметят через месяц и не свяжут с кэшем.
void TestAssetCache() {
    // Путь можно подменить переменной окружения: так тот же замер гоняется на
    // СВОЕЙ модели, а не только на маленькой тестовой. Разница между ними
    // принципиальна — вес кэша определяется размером текстур, а он у боевого
    // ассета на порядок больше.
    std::string model =
#ifdef SAGE_TEST_MODEL
        SAGE_TEST_MODEL;
#else
        "assets/test_model.glb";
#endif
    if (const char* custom = std::getenv("SAGE_TEST_MODEL_PATH")) model = custom;
    {
        std::ifstream probe(model, std::ios::binary);
        if (!probe) {
            std::printf("  ПРОПУСК: нет %s\n", model.c_str());
            return;
        }
    }

    const std::string cacheDir = "test_asset_cache";
    std::error_code ec;
    std::filesystem::remove_all(cacheDir, ec);
    sage::assets::SetCacheDirectory(cacheDir);
    sage::assets::SetCacheEnabled(false);

    using clk = std::chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    // Эталон — разбор исходника при выключенном кэше.
    auto t0 = clk::now();
    std::unique_ptr<sage::render::SkinnedModel> direct = sage::render::SkinnedModel::Load(model);
    const double parseMs = ms(t0, clk::now());
    Check(direct != nullptr, "модель разобрана из исходника");
    if (!direct) return;

    sage::assets::SetCacheEnabled(true);
    sage::assets::ResetCacheStats();

    // Первый заход с включённым кэшем: кэша ещё нет — разбор и запись.
    t0 = clk::now();
    std::unique_ptr<sage::render::SkinnedModel> cold = sage::render::SkinnedModel::Load(model);
    const double coldMs = ms(t0, clk::now());
    Check(sage::assets::CacheStats().Misses == 1, "первый заход записал кэш");
    Check(std::filesystem::exists(sage::assets::CachePathFor(model)), "файл кэша создан");

    // Второй: чтение из кэша.
    t0 = clk::now();
    std::unique_ptr<sage::render::SkinnedModel> warm = sage::render::SkinnedModel::Load(model);
    const double warmMs = ms(t0, clk::now());
    Check(sage::assets::CacheStats().Hits == 1, "второй заход прочитал кэш");

    // --- Совпадение данных ---
    Check(warm->GetSkeleton().Count() == direct->GetSkeleton().Count(), "костей столько же");
    Check(warm->SubMeshCount() == direct->SubMeshCount(), "подмешей столько же");
    Check(warm->Clips().size() == direct->Clips().size(), "клипов столько же");

    bool skeletonSame = warm->GetSkeleton().Count() == direct->GetSkeleton().Count();
    for (int i = 0; skeletonSame && i < direct->GetSkeleton().Count(); ++i) {
        const auto& a = direct->GetSkeleton().Joints[(size_t)i];
        const auto& b = warm->GetSkeleton().Joints[(size_t)i];
        skeletonSame = a.Name == b.Name && a.Parent == b.Parent &&
                       glm::length(a.Translation - b.Translation) < 1e-6f &&
                       glm::length(a.Scale - b.Scale) < 1e-6f &&
                       std::abs(glm::dot(a.Rotation, b.Rotation)) > 1.0f - 1e-6f;
    }
    Check(skeletonSame, "скелет из кэша совпадает с разобранным до имени и позы каждой кости");

    bool clipsSame = warm->Clips().size() == direct->Clips().size();
    size_t keysChecked = 0;
    for (size_t c = 0; clipsSame && c < direct->Clips().size(); ++c) {
        const auto& a = direct->Clips()[c];
        const auto& b = warm->Clips()[c];
        clipsSame = a.Name == b.Name && std::abs(a.Duration - b.Duration) < 1e-6f &&
                    a.Channels.size() == b.Channels.size();
        for (size_t ch = 0; clipsSame && ch < a.Channels.size(); ++ch) {
            clipsSame = a.Channels[ch].Joint == b.Channels[ch].Joint &&
                        a.Channels[ch].Times.size() == b.Channels[ch].Times.size() &&
                        a.Channels[ch].Values.size() == b.Channels[ch].Values.size();
            for (size_t k = 0; clipsSame && k < a.Channels[ch].Values.size(); ++k) {
                clipsSame = glm::length(a.Channels[ch].Values[k] - b.Channels[ch].Values[k]) < 1e-6f;
                ++keysChecked;
            }
        }
    }
    Check(clipsSame, "клипы из кэша совпадают покадрово");
    Check(keysChecked > 0, "сравнение клипов действительно что-то сравнило");

    // --- Устаревание ---
    // Кэш обязан протухать при правке исходника, иначе правка модели молча не
    // доезжает до игры, и художник ищет причину где угодно, кроме кэша.
    const auto stamp = std::filesystem::last_write_time(model, ec);
    std::filesystem::last_write_time(model, stamp + std::chrono::seconds(120), ec);
    sage::assets::ResetCacheStats();
    std::unique_ptr<sage::render::SkinnedModel> after = sage::render::SkinnedModel::Load(model);
    Check(sage::assets::CacheStats().Hits == 0, "правка исходника обесценила кэш");
    Check(sage::assets::CacheStats().Misses == 1, "и заставила разобрать заново");
    std::filesystem::last_write_time(model, stamp, ec);

    // --- Порча ---
    // Битый кэш не должен ни ронять программу, ни давать неверную модель.
    {
        std::ofstream broken(sage::assets::CachePathFor(model), std::ios::binary | std::ios::trunc);
        broken << "это не кэш";
    }
    sage::assets::ResetCacheStats();
    std::unique_ptr<sage::render::SkinnedModel> repaired = sage::render::SkinnedModel::Load(model);
    Check(repaired != nullptr, "битый кэш не мешает загрузке");
    Check(repaired->SubMeshCount() == direct->SubMeshCount(), "и модель получается правильная");
    Check(sage::assets::CacheStats().Hits == 0, "битый кэш не засчитан как попадание");

    // Раскладываем «из кэша» на две части. Это не любопытство: чтение кэша мы
    // ускорили, а загрузку на видеокарту — нет, и без разделения непонятно,
    // осталось ли что улучшать в кэше или упёрлись в GPU.
    sage::render::ModelData probe;
    t0 = clk::now();
    sage::assets::ReadModelCache(model, probe);
    const double readMs = ms(t0, clk::now());
    const auto cacheSize = std::filesystem::file_size(sage::assets::CachePathFor(model), ec);

    std::printf("    разбор исходника %.1f мс, первый заход %.1f мс, из кэша %.1f мс (в %.1f раза)\n",
                parseMs, coldMs, warmMs, warmMs > 0.001 ? parseMs / warmMs : 0.0);
    std::printf("      в том числе чтение кэша %.1f мс (%.1f МиБ), загрузка на видеокарту %.1f мс\n",
                readMs, (double)cacheSize / (1024.0 * 1024.0), warmMs - readMs);
    Check(warmMs < parseMs, "загрузка из кэша быстрее разбора исходника");

    std::filesystem::remove_all(cacheDir, ec);
    sage::assets::SetCacheDirectory(".sage-cache");
}

// --- Небо: направление луча обязано совпадать с камерой ------------------------
//
// Вершинный шейдер неба нормализовал направление ДО интерполяции. Луч
// восстанавливается из точки на дальней плоскости, а растеризатор
// интерполирует varying ЛИНЕЙНО — линейно интерполировать можно только сами
// точки плоскости, они на ней и остаются. Нормаль же делит каждую вершину на
// СВОЮ длину, а у полноэкранного треугольника длины отличаются в разы (NDC
// идёт от -1 до 3). После такого деления промежуточные значения не
// соответствуют ни одному лучу камеры: горизонт уезжает и перекашивается, тем
// сильнее, чем шире кадр.
//
// Проверка — сравнение с АНАЛИТИЧЕСКИМ ответом, а не с эталонной картинкой:
// направление луча камеры считается точно, и правильный результат известен
// заранее. Такой тест не зависит ни от драйвера, ни от того, что кто-то
// однажды перезапишет эталон.
void TestSkyRayDirection() {
    SkyRenderer sky;
    constexpr int w = 256, h = 128;
    const glm::vec3 top(0.05f, 0.10f, 0.55f);
    const glm::vec3 horizon(1.0f, 0.55f, 0.15f);

    Framebuffer fbo(w, h);
    fbo.Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    device.Clear(true, true);

    // Широкий угол — там искажение максимально.
    const float fov = glm::radians(100.0f);
    const float aspect = (float)w / (float)h;
    const glm::mat4 proj = glm::perspective(fov, aspect, 0.1f, 500.0f);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, -1.0f),
                                       glm::vec3(0.0f, 1.0f, 0.0f));
    sky.Draw(view, proj, top, horizon);

    const Image img = Capture(w, h);
    device.BindDefaultFramebuffer();

    // Тот же расчёт, что обязан делать шейдер: луч пинхол-камеры и градиент.
    const float tanHalf = std::tan(fov * 0.5f);
    double worst = 0.0;
    double mean = 0.0;
    for (int y = 0; y < img.Height; ++y) {
        for (int x = 0; x < img.Width; ++x) {
            // Capture переворачивает строки, поэтому строка 0 — ВЕРХ кадра.
            const float ndcX = ((x + 0.5f) / w) * 2.0f - 1.0f;
            const float ndcY = 1.0f - ((y + 0.5f) / h) * 2.0f;
            const glm::vec3 dir =
                glm::normalize(glm::vec3(ndcX * tanHalf * aspect, ndcY * tanHalf, -1.0f));
            const float t = glm::clamp(dir.y, 0.0f, 1.0f);
            // Та же формула, что в шейдере, включая окно сглаживания у горизонта.
            const float e = glm::clamp(dir.y / 0.25f, 0.0f, 1.0f);
            const float soften = e * e * (3.0f - 2.0f * e);
            const glm::vec3 expect = glm::mix(horizon, top, std::pow(t, 0.5f) * soften);
            for (int c = 0; c < 3; ++c) {
                const int got = img.Pixels[((size_t)y * img.Width + x) * 3 + c];
                const double d = std::abs(got - expect[c] * 255.0);
                worst = std::max(worst, d);
                mean += d;
            }
        }
    }
    mean /= (double)img.Pixels.size();
    std::printf("       отклонение от аналитического неба: средн. %.2f, макс. %.2f (из 255)\n",
                mean, worst);
    // До правки максимум доходил до сотни: горизонт стоял не там, где смотрит
    // камера. Допуск — на округление в 8 бит и точность float в шейдере.
    Check(worst <= 8.0, "луч неба совпадает с лучом камеры");
}


// --- Тени прожектора и точечного света ---------------------------------------
//
// ЧТО ИМЕННО ПРОВЕРЯЕТСЯ. Не «картинка изменилась» — этого мало: проход теней
// меняет картинку и когда он просто затемняет всё подряд. Проверяется, что
// потемнение идёт ОТ ГЕОМЕТРИИ. Поэтому кадров четыре: с перекрытием и без,
// каждый — с тенями и без них. Тень обязана появиться там, где между лампой и
// полом стоит плита, и НЕ появиться там, где её убрали.
//
// Контрольная пара (без перекрытия) — главная часть теста. Без неё проверку
// прошла бы любая ошибка, дающая тень из ниоткуда: пустая плитка атласа,
// перепутанная грань куба, сбитая на пол-плитки выборка.
std::unique_ptr<Scene> MakeLampScene(LightComponent::Type kind, bool withBlocker) {
    auto scene = std::make_unique<Scene>("LampShadow");

    // Солнца нет: иначе его тень смешалась бы с ламповой, и разница между
    // кадрами перестала бы означать что-то одно. Засветка почти нулевая — по
    // той же причине.
    scene->Lighting.Sun.Intensity = 0.0f;
    scene->Lighting.SkyColor = {0.05f, 0.05f, 0.06f};
    scene->Lighting.GroundColor = {0.04f, 0.04f, 0.05f};
    scene->Lighting.AmbientStrength = 0.15f;
    scene->Lighting.Skybox.Enabled = false;

    auto primitive = [](GameObject obj, MeshRef::Type type, glm::vec3 color) {
        obj.Renderer().Ref = MeshRef{type};
        obj.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(type);
        obj.Renderer().Color = color;
    };

    GameObject ground = scene->CreateObject("Ground");
    ground.GetTransform().Scale = {14.0f, 1.0f, 14.0f};
    primitive(ground, MeshRef::Type::Plane, {0.62f, 0.62f, 0.64f});

    if (withBlocker) {
        GameObject blocker = scene->CreateObject("Blocker");
        blocker.GetTransform().Position = {0.0f, 2.0f, 0.0f};
        blocker.GetTransform().Scale = {1.6f, 0.2f, 1.6f};
        primitive(blocker, MeshRef::Type::Cube, {0.55f, 0.55f, 0.58f});
    }

    GameObject lamp = scene->CreateObject("Lamp");
    lamp.GetTransform().Position = {0.0f, 4.0f, 0.0f};
    lamp.GetTransform().Rotation = {-90.0f, 0.0f, 0.0f}; // «вперёд» строго вниз
    LightComponent& lc = lamp.Registry()->emplace<LightComponent>(lamp.Entity());
    lc.Kind = kind;
    lc.Color = {1.0f, 0.97f, 0.92f};
    lc.Intensity = 40.0f;
    lc.Range = 24.0f;
    lc.InnerConeDeg = 30.0f;
    lc.OuterConeDeg = 45.0f;
    return scene;
}

// atlas == nullptr — тот же кадр без теней ламп (база для сравнения).
Image RenderWithLampShadows(FrameRenderer& r, Scene& scene,
                            sage::render::LocalShadowAtlas* atlas) {
    Framebuffer sceneFbo(kW, kH);
    LightingEnvironment env = sage::ecs::CollectLighting(scene);

    if (atlas) {
        // Раздача мест ИЗМЕНЯЕТ env: каждому источнику проставляется его плитка.
        atlas->Prepare(env);
        for (int p = 0; p < atlas->PassCount(); ++p) {
            atlas->BeginPass(p);
            r.Batch.RenderDepth(scene, atlas->PassMatrix(p));
        }
        atlas->End(kW, kH);
    }

    ShadowBinding shadows(r.Shadow, /*enabled=*/false); // солнечных теней тут нет
    if (atlas) shadows.Local = atlas->Binding();

    sceneFbo.Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetClearColor(0.02f, 0.02f, 0.03f, 1.0f);
    device.Clear(true, true);
    const glm::mat4 view = TestView();
    r.Batch.RenderColor(scene, view, PerspectiveProj(), kEye, env, shadows, 0);
    sceneFbo.Resolve();
    sceneFbo.Bind();
    return Capture(kW, kH);
}

void TestLocalShadowsOfKind(FrameRenderer& r, LightComponent::Type kind, const char* label,
                            const char* dumpPrefix) {
    sage::render::LocalShadowAtlas atlas(1024, 256);
    Check(atlas.Valid(), "атлас локальных теней создан");

    std::unique_ptr<Scene> blocked = MakeLampScene(kind, true);
    const Image litBlocked = RenderWithLampShadows(r, *blocked, nullptr);
    const Image shadowedBlocked = RenderWithLampShadows(r, *blocked, &atlas);

    std::unique_ptr<Scene> open = MakeLampScene(kind, false);
    const Image litOpen = RenderWithLampShadows(r, *open, nullptr);
    const Image shadowedOpen = RenderWithLampShadows(r, *open, &atlas);

    if (std::getenv("SAGE_DUMP_LOCAL_SHADOWS")) {
        SavePng(std::string(dumpPrefix) + "_lit.png", litBlocked);
        SavePng(std::string(dumpPrefix) + "_shadowed.png", shadowedBlocked);
    }

    const long long darkened = NewlyShadowed(litBlocked, shadowedBlocked);
    const long long phantom = NewlyShadowed(litOpen, shadowedOpen);
    std::printf("    %s: затенено пикселей %lld, без перекрытия %lld\n", label, darkened, phantom);

    // Тень есть и она заметная. Порог в сотни пикселей, а не в единицы: плита
    // 1.6 на высоте 2 под лампой на высоте 4 даёт на полу пятно вдвое больше
    // себя, и оно занимает изрядную часть кадра.
    Check(darkened > 400, (std::string(label) + ": лампа отбрасывает тень").c_str());
    // А без перекрытия проход теней не смеет затемнить НИЧЕГО. Полсотни
    // пикселей допуска — на самозатенение по кромке пола у самого горизонта.
    Check(phantom < 50, (std::string(label) + ": без перекрытия тени не возникает").c_str());
}

void TestLocalShadows(FrameRenderer& r) {
    TestLocalShadowsOfKind(r, LightComponent::Type::Spot, "прожектор", "local_spot");
    // Точечный — отдельная механика: шесть граней куба, разложенных в тот же
    // атлас, и выбор грани по наибольшей компоненте вектора в шейдере.
    TestLocalShadowsOfKind(r, LightComponent::Type::Point, "точечный", "local_point");
}

// --- Соответствие RHI на НАСТОЯЩЕМ бэкенде ------------------------------------
//
// Тот же контракт, что sage_tests гоняет по Null, — но здесь есть контекст, и
// потому доступен пиксельный уровень: соглашения о системе координат
// (начало отсчёта ножниц, порядок строк при чтении) проверяются рисованием, а
// иначе не проверяются никак.
//
// Смысл в том, что набор ОДИН. Два бэкенда, прошедшие один и тот же набор, —
// это уже не «интерфейс совпадает с тем, что делает OpenGL», а обязательство,
// у которого есть исполнители.
void TestRhiConformance() {
    sage::rhi::ConformanceOptions options;
    options.Rasterizes = true;
    const sage::rhi::ConformanceResult result =
        sage::rhi::RunConformance(sage::rhi::GraphicsDevice::Get(), options);

    for (const std::string& failure : result.Failures) {
        std::printf("       %s\n", failure.c_str());
    }
    Check(result.Ok(), "OpenGL-бэкенд соответствует контракту RHI");
    Check(result.Checked >= 20, "набор соответствия отработал целиком");
}

int main(int argc, char** argv) {
    std::string referenceDir = "references";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--update") == 0) SetUpdateMode(true);
        else if (std::strcmp(argv[i], "--references") == 0 && i + 1 < argc) referenceDir = argv[++i];
    }
    SetReferenceDir(referenceDir);

    Log::Init("sage_render_tests.log");
    std::printf("SAGE Engine — эталонные кадры\n");
    std::printf("=============================\n");
    if (UpdateMode()) std::printf("РЕЖИМ ОБНОВЛЕНИЯ: эталоны будут перезаписаны\n");
    std::printf("Каталог эталонов: %s\n\n", referenceDir.c_str());

    // Окно нужно только ради графического контекста — на экране ему делать
    // нечего. Размер минимальный: весь рендер идёт в offscreen-буферы.
    Window::Params params;
    params.Hidden = true;
    params.Resizable = false;
    params.VSync = false;
    Window window(64, 64, "sage_render_tests", params);

    // Девайс поднимаем сами: Application нам не нужен (нет ни слоёв, ни цикла),
    // а загрузчик функций GL берём у GLFW — как это делает Application.
    std::unique_ptr<sage::rhi::GraphicsDevice> device =
        sage::rhi::GraphicsDevice::Create(sage::rhi::Backend::OpenGL);
    device->Init(reinterpret_cast<sage::rhi::ProcLoader>(glfwGetProcAddress));
    sage::rhi::GraphicsDevice::SetCurrent(device.get());

    {
        std::unique_ptr<Scene> scene = MakeScene();
        FrameRenderer renderer;
        TestScenePerspective(renderer, *scene);
        TestShadowsOffIsNotBlack(renderer, *scene);
        TestSceneOrthographic(renderer, *scene);
        TestNoPostFX(renderer, *scene);
        TestDepthOfField(renderer, *scene);
        TestFxaa(renderer, *scene);
        TestGrid(renderer, *scene);
        TestTransparentFaceOrder(renderer, *scene);
        TestEmissive(renderer, *scene);
        TestDecals(renderer);
        TestObjectMotionBlur(renderer);
        TestMsaa(renderer);
        TestMorphTargets(renderer);
        TestInverseKinematicsECS();
        TestReflections(renderer);
        TestReflectionProbe(renderer);
        TestReflectionSeams();
        TestPlanarReflectionMath();
        TestPlanarReflectionRender(renderer);
        TestShadowSoftness(renderer, *scene);
        TestShadowCascades(renderer);
        TestLocalShadows(renderer);
        TestLevelsOfDetail(renderer);
        TestOcclusionCulling(renderer);
        TestAssetCache();
        TestSkyRayDirection();
        TestRhiConformance();
    }

    // GPU-ресурсы освобождаем, пока контекст ещё жив: деструктор синглтона
    // сработал бы уже после разрушения окна.
    ResourceManager::Instance().Clear();
    sage::rhi::GraphicsDevice::SetCurrent(nullptr);

    std::printf("\n=============================\n");
    if (g_written > 0) {
        std::printf("Записано новых эталонов: %d — проверьте их глазами и закоммитьте.\n",
                    g_written);
    }
    std::printf("Пройдено: %d, провалено: %d\n", g_passed, g_failed);
    // Записанный впервые эталон — это не успех: сравнивать было не с чем.
    // В CI такое означает, что эталон забыли закоммитить.
    if (g_written > 0 && !UpdateMode()) {
        std::printf("ВНИМАНИЕ: часть эталонов отсутствовала — тест не считается пройденным.\n");
        return 2;
    }
    return g_failed == 0 ? 0 : 1;
}
