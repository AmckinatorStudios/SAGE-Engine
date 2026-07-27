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
#include "sage/render/PostFX.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "sage/assets/AssetCache.h"
#include "sage/render/SkinnedModel.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/ShadowMap.h"
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
    r.Batch.RenderColor(scene, view, proj, kEye, env, r.Shadow.LightMatrix(),
                        r.Shadow.DepthTexture(), true, 0);
    // Скелетные модели рисуются отдельным проходом (свой шейдер со скиннингом),
    // как и в редакторе с инструментом.
    sage::anim::DrawAnimatedModels(scene, view, proj, kEye, env, r.Shadow.LightMatrix(),
                                   r.Shadow.DepthTexture(), true);

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
        r.Batch.RenderColor(*scene, view, proj, kEye, env, glm::mat4(1.0f), 0, false, 1);
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
        r.Batch.RenderColor(*scene, view, proj, kEye, env, r.Shadow.LightMatrix(),
                            r.Shadow.DepthTexture(), true, 0);

        unsigned int velTex = 0;
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
        r.Batch.RenderColor(*scene, view, proj, kEye, env, r.Shadow.LightMatrix(),
                            r.Shadow.DepthTexture(), true, 0);
        r.Fx.Render(sceneFbo.ColorTexture(), sceneFbo.DepthTexture(), kW, kH, proj, view, sharpFx,
                    &output, 0, 0, kW, kH, 0);
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
        TestSceneOrthographic(renderer, *scene);
        TestNoPostFX(renderer, *scene);
        TestDepthOfField(renderer, *scene);
        TestFxaa(renderer, *scene);
        TestGrid(renderer, *scene);
        TestObjectMotionBlur(renderer);
        TestMsaa(renderer);
        TestMorphTargets(renderer);
        TestShadowSoftness(renderer, *scene);
        TestAssetCache();
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
