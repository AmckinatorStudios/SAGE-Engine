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
    // Ambient здесь задан ЯВНО и небу не подчиняется — значит «свои значения»
    // (см. LightingEnvironment::AmbientMode). Выключенное небо иначе забирает
    // с собой и окружающий свет, и проверять было бы нечего: чёрный кадр.
    scene->Lighting.AmbientMode = LightingEnvironment::AmbientSource::Custom;
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
                  const sage::render::PostChain* chain, int width, int height,
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

    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();

    // БЕЗ тракта сцена рисуется ПРЯМО в выход и с аппаратной гаммой — ровно так,
    // как идёт собранная игра (PlayerLayer: SetSRGBWrite(!usePost)).
    //
    // Это не деталь. Нарисовать без пост-обработки в HDR-буфер, а потом
    // скопировать его в выход означало бы показать сырой линейный цвет: кадр
    // вышел бы засвеченным, и эталон «без пост-обработки» сторожил бы путь,
    // которым движок НЕ ходит.
    Framebuffer& sceneTarget = chain ? sceneFbo : output;
    sceneTarget.Bind();
    device.SetSRGBWrite(chain == nullptr);
    device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
    device.Clear(true, true);
    r.Batch.RenderColor(scene, view, proj, kEye, env, ShadowBinding(r.Shadow, true), 0);
    // Скелетные модели рисуются отдельным проходом (свой шейдер со скиннингом),
    // как и в редакторе с инструментом.
    sage::anim::DrawAnimatedModels(scene, view, proj, kEye, env, ShadowBinding(r.Shadow, true));

    if (grid) r.Grid.Draw(view, proj, kEye, *grid);
    device.SetSRGBWrite(false);

    if (chain) {
        // Смаз движения опирается на историю кадров, а тест должен быть
        // детерминированным при любом порядке запуска — сбрасываем её явно.
        r.Fx.ResetHistory();
        r.Fx.Render(sceneTarget.ColorTexture(), sceneTarget.DepthTexture(), width, height, proj,
                    view, *chain, &output, 0, 0, width, height);
    }

    output.Bind();
    return Capture(width, height);
}

sage::render::PostChain BaseChain() {
    using namespace sage::render;
    PostChain chain;
    // Тракт эталонных кадров: затенение -> экспозиция -> свечение -> цвет ->
    // тон-маппинг -> виньетка. Раньше половина этого была ПАРАМЕТРАМИ
    // тон-маппинга (экспозиция, насыщенность, контраст, виньетка) — теперь это
    // отдельные звенья, каждое со своим этапом. Значения те же, что и были,
    // поэтому кадр меняется настолько, насколько изменился порядок операций, а
    // не настройки.
    //
    // Глубины резкости и смаза здесь нет (их включают отдельные проверки),
    // FXAA — тоже.
    AddEffect(chain, "ao");
    SetParam(chain, "ao", "radius", 0.5f);
    SetParam(chain, "ao", "strength", 1.0f);

    AddEffect(chain, "exposure");
    SetParam(chain, "exposure", "exposure", 0.07f); // ~1.05x, как было множителем

    AddEffect(chain, "bloom");
    SetParam(chain, "bloom", "threshold", 1.0f);
    SetParam(chain, "bloom", "intensity", 0.55f);

    AddEffect(chain, "color");
    SetParam(chain, "color", "saturation", 1.16f);
    SetParam(chain, "color", "contrast", 1.06f);

    AddEffect(chain, "tonemap");
    SetParam(chain, "tonemap", "mode", 2.0f); // ACES — та же кривая, что была
    SetParam(chain, "tonemap", "gamma", 2.2f);
    // Вывод — степенная гамма, как было до выбора вывода: эталоны сняты с
    // ней, а умолчание звена теперь sRGB.
    SetParam(chain, "tonemap", "output", 1.0f);

    AddEffect(chain, "vignette");
    SetParam(chain, "vignette", "intensity", 0.35f);
    return chain;
}

sage::render::PostEffect& AddEffect(sage::render::PostChain& chain, const char* kindId) {
    // Место в тракте выбирает движок по тому, что звено читает: тест не должен
    // знать про порядок HDR/LDR — иначе он проверял бы своё же знание.
    return sage::render::AddPostEffect(chain, kindId);
}

void RemoveEffect(sage::render::PostChain& chain, const char* kindId) {
    sage::render::RemovePostEffect(chain, kindId);
}

void SetParam(sage::render::PostChain& chain, const char* kindId, const char* param, float value) {
    for (sage::render::PostEffect& e : chain.Effects) {
        if (e.Kind != kindId) continue;
        if (sage::render::PostValue* p = e.Find(param)) p->V[0] = value;
        return;
    }
}

bool HasEffect(const sage::render::PostChain& chain, const char* kindId) {
    for (const sage::render::PostEffect& e : chain.Effects)
        if (e.Kind == kindId) return true;
    return false;
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
