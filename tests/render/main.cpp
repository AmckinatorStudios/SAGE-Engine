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
#include "sage/render/PostFX.h"
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
};

Image RenderFrame(FrameRenderer& r, Scene& scene, const glm::mat4& proj,
                  const sage::render::PostFXSettings& fx, int width, int height) {
    Framebuffer sceneFbo(width, height);
    Framebuffer output(width, height);

    const LightingEnvironment env = sage::ecs::CollectLighting(scene);
    const glm::mat4 view = TestView();

    // Тени: бокс света центрируем на сцене, радиус с запасом на пол.
    r.Shadow.SetLightMatrix(env.Sun.Direction, glm::vec3(0.0f, 0.0f, 0.0f), 12.0f);
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
        TestMorphTargets(renderer);
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
