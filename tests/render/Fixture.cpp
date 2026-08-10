// Тела общей оснастки эталонных кадров (см. Fixture.h).
#include "Fixture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/anim/AnimationSystem.h"
#include "sage/ecs/LightSystem.h"
#include "sage/render/ResourceManager.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Components.h"

namespace sage::rendertest {
namespace {

int g_passed = 0;
int g_failed = 0;
int g_written = 0;

// Допуски. Сознательно не нулевые: разные драйверы дают разные последние биты,
// и требовать побитового совпадения значило бы получить тест, который «падает
// сам по себе» и которому перестают верить.
constexpr double kMaxMeanDiff = 1.5;   // средняя разница по каналам, 0..255
constexpr double kMaxDiffFraction = 0.02; // не больше 2% заметно отличающихся пикселей

} // namespace

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

// grid — рисовать ли сетку поверх геометрии (nullptr — не рисовать). Сетка
// идёт ДО пост-обработки и после геометрии: ровно там же, где во вьюпорте
// инструмента, иначе тест проверял бы не тот путь.
// shadowRadius — полусторона ортобокса теней. Параметр, а не константа: от
// него напрямую зависит размер текселя, а значит и то, ВИДНО ли работу
// фильтра. На тесной коробке фильтровать почти нечего.
Image RenderFrame(FrameRenderer& r, Scene& scene, const glm::mat4& proj,
                  const sage::render::PostFXSettings& fx, int width, int height,
                  const sage::render::GridSettings* grid, float shadowRadius) {
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

// Счётчики наружу. Часть проверок печатает свою строку и не пользуется Report
// (у них не картинка, а число со своим смыслом), но в общий итог попасть
// обязана — иначе «пройдено: 114» перестаёт быть правдой.
void CountPass() { ++g_passed; }
void CountFail() { ++g_failed; }

int PassedCount() { return g_passed; }
int FailedCount() { return g_failed; }
int WrittenCount() { return g_written; }

} // namespace sage::rendertest
