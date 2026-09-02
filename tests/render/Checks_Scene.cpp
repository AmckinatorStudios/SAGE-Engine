// ---------------------------------------------------------------------------
// Эталонные кадры — сцена и подсистемы: детализация, отсечение, кэш, небо, RHI.
//
// Проверки, которым нужен графический контекст, но не нужен эталонный кадр:
// уровни детализации, отсечение перекрытием, кэш ассетов, направление луча неба
// и соответствие бэкенда контракту RHI.
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
#include "sage/render/ScenePasses.h"
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

// Сцена для проверки «ответ принадлежит виду»: стена и РЕДКАЯ цепочка кубов за
// ней. Редкая намеренно — кубы не должны закрывать друг друга, иначе камера,
// которой открыто всё, честно отсечёт половину сама, и проверка перестанет
// отличать свои ответы от чужих.
std::unique_ptr<Scene> BuildTwoViewScene() {
    auto scene = std::make_unique<Scene>("TwoViewOcclusion");
    scene->Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.3f, -0.9f, -0.2f));
    scene->Lighting.Sun.Intensity = 2.0f;
    scene->Lighting.AmbientStrength = 0.3f;
    scene->Lighting.Skybox.Enabled = false;

    auto primitive = [](GameObject obj, glm::vec3 color) {
        obj.Renderer().Ref = MeshRef{MeshRef::Type::Cube};
        obj.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
        obj.Renderer().Color = color;
    };

    GameObject wall = scene->CreateObject("Wall");
    wall.GetTransform().Position = {0.0f, 0.0f, -4.0f};
    wall.GetTransform().Scale = {40.0f, 40.0f, 0.5f};
    primitive(wall, {0.55f, 0.53f, 0.5f});

    for (int i = 0; i < 5; ++i) {
        GameObject box = scene->CreateObject("Behind" + std::to_string(i));
        box.GetTransform().Position = {-5.0f + 2.5f * (float)i, 0.0f, -20.0f};
        box.GetTransform().Scale = glm::vec3(0.6f);
        primitive(box, {0.8f, 0.4f, 0.3f});
    }
    return scene;
}

// Проверка перекрытия ведётся ОТДЕЛЬНО ДЛЯ КАЖДОГО ВИДА.
//
// ЧТО ЭТО ЗА ДЕФЕКТ. «Объект закрыт» — свойство пары «объект и точка, откуда
// смотрят», а не самого объекта. Батч у редактора один, а видов за кадр
// несколько: вьюпорт, окна раскладки, панель Game. Пока ответы лежали в одной
// таблице, их переписывал ПОСЛЕДНИЙ вид кадра — то есть игровая камера, — и
// следующий кадр вьюпорта выбрасывал всё, чего не видно ЕЙ. Снаружи это
// выглядит как «в редакторе вьюпорт чёрный, а окно игры в порядке»: геометрия
// из вьюпорта пропадает, остаётся почти чёрная заливка фона, а панель Game
// показывает ровно то, по чьим ответам всё и отсеклось.
//
// Проверяется тем же способом, каким дефект и виден: две камеры смотрят на одну
// сцену через ОДИН батч в одном кадре. Игровой стена закрывает кубы, вид сверху
// смотрит на них с открытой стороны. Второй обязан рисовать их все.
void TestOcclusionIsPerView() {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    if (!device.SupportsOcclusionQueries()) {
        std::printf("    ПРОПУСК: драйвер не умеет запросов перекрытия\n");
        return;
    }

    std::unique_ptr<Scene> scene = BuildTwoViewScene();
    const LightingEnvironment env = sage::ecs::CollectLighting(*scene);
    const glm::mat4 proj =
        glm::perspective(glm::radians(60.0f), (float)kW / (float)kH, 0.1f, 200.0f);
    // Батч СВОЙ: общий несёт ответы прошлых проверок, а номера сущностей у
    // новой сцены начинаются с нуля и совпадают с чужими.
    sage::ecs::RenderBatch batch;

    // Камера ИГРЫ: перед стеной, кубы за ней закрыты целиком.
    const glm::vec3 gameEye(0.0f, 0.0f, 1.0f);
    const glm::mat4 gameView =
        glm::lookAt(gameEye, glm::vec3(0.0f, 0.0f, -10.0f), glm::vec3(0, 1, 0));
    // Камера ВЬЮПОРТА: сверху над кубами — между ней и ними нет ничего.
    const glm::vec3 editorEye(0.0f, 9.0f, -20.0f);
    const glm::mat4 editorView =
        glm::lookAt(editorEye, glm::vec3(0.0f, 0.0f, -20.0f), glm::vec3(0, 0, -1));

    Framebuffer fbo(kW, kH);
    auto pass = [&](const glm::mat4& view, const glm::vec3& eye, int viewId) {
        fbo.Bind();
        device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
        device.Clear(true, true);
        sage::render::SceneColorInput input;
        input.View = view;
        input.Proj = proj;
        input.ViewPos = eye;
        input.Env = &env;
        input.OcclusionCulling = true;
        input.ViewId = viewId;
        sage::render::RenderSceneColor(*scene, batch, input);
        // Чтение кадра — не ради пикселей, а ради синхронизации: без него
        // программный растеризатор не доводит запросы до готовности, и ответы
        // не приходят никогда.
        fbo.Resolve();
        fbo.Bind();
        Capture(kW, kH);
        return batch.Stats();
    };

    // Порядок как в редакторе: сначала вьюпорт, следом панель Game. Ответы
    // игровой камеры ложатся последними — их-то вьюпорт и подхватывал.
    sage::ecs::RenderStats editorStats{};
    sage::ecs::RenderStats gameStats{};
    for (int i = 0; i < 6; ++i) {
        editorStats = pass(editorView, editorEye, /*viewId=*/0);
        gameStats = pass(gameView, gameEye, /*viewId=*/1);
    }

    std::printf("    вьюпорт: нарисовано %d (закрытыми признано %d); "
                "игра: нарисовано %d (закрытыми признано %d)\n",
                editorStats.Drawn, editorStats.CulledOccluded, gameStats.Drawn,
                gameStats.CulledOccluded);

    // Игровая камера обязана отсечь всё, что за стеной, — иначе проверять нечего.
    Check(gameStats.CulledOccluded >= 5, "игровая камера отсекает закрытое стеной");
    // А вид сверху — не потерять НИЧЕГО: перед ним открыто всё.
    Check(editorStats.CulledOccluded == 0,
          "вьюпорт не теряет геометрию из-за ответов чужой камеры");
    Check(editorStats.Drawn >= 5, "вьюпорт рисует всю видимую ему сцену");
}

// Проход теней не смотрит на ответы о перекрытии.
//
// Сбор для карты теней идёт из точки СВЕТА, а ответы получены из точки камеры.
// Объект, спрятанный за стеной от зрителя, солнце всё так же освещает, и его
// тень вполне может лежать на видимом месте. Пока проверка применялась и здесь,
// включённое отсечение перекрытием молча выедало тени.
void TestOcclusionDoesNotEatShadows() {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    if (!device.SupportsOcclusionQueries()) {
        std::printf("    ПРОПУСК: драйвер не умеет запросов перекрытия\n");
        return;
    }

    std::unique_ptr<Scene> scene = BuildOccludedScene();
    const LightingEnvironment env = sage::ecs::CollectLighting(*scene);
    const glm::vec3 eye(0.0f, 0.0f, 1.0f);
    const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f, 0.0f, -10.0f), glm::vec3(0, 1, 0));
    const glm::mat4 proj =
        glm::perspective(glm::radians(60.0f), (float)kW / (float)kH, 0.1f, 200.0f);

    Framebuffer fbo(kW, kH);
    sage::ecs::RenderBatch batch;
    ShadowMap shadow(1024);
    // Несколько кадров: ответы о перекрытии приходят со сдвигом на кадр.
    int shadowDrawn = 0;
    for (int i = 0; i < 6; ++i) {
        fbo.Bind();
        device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
        device.Clear(true, true);
        sage::render::SceneColorInput input;
        input.View = view;
        input.Proj = proj;
        input.ViewPos = eye;
        input.Env = &env;
        input.OcclusionCulling = true;
        sage::render::RenderSceneColor(*scene, batch, input);
        fbo.Resolve();
        fbo.Bind();
        Capture(kW, kH);   // синхронизация: без неё ответы запросов не созревают

        // Проход глубины света — тем же хелпером, каким его зовёт кадр.
        ShadowMap::CameraView cv;
        cv.Position = eye;
        cv.Forward = glm::vec3(0.0f, 0.0f, -1.0f);
        cv.ShadowDistance = 60.0f;
        shadow.FitSingle(env.Sun.Direction, cv);
        sage::render::RenderShadowDepth(shadow, *scene, batch, kW, kH);
        shadowDrawn = batch.Stats().Drawn;
    }

    std::printf("    в карту теней попало объектов: %d\n", shadowDrawn);
    // Сцена — стена, три куба перед ней и сорок за ней: тень отбрасывают все.
    Check(shadowDrawn >= 44, "закрытые от камеры объекты продолжают отбрасывать тень");
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

} // namespace

void RunSceneChecks(FrameRenderer& r) {
    TestLevelsOfDetail(r);
    TestOcclusionCulling(r);
    TestOcclusionIsPerView();
    TestOcclusionDoesNotEatShadows();
    TestAssetCache();
    TestSkyRayDirection();
    TestRhiConformance();
}

} // namespace sage::rendertest
