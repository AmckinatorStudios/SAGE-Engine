// ---------------------------------------------------------------------------
// Эталонные кадры — тени: мягкость кромки, каскады, лампы.
//
// Тени вынесены отдельно, потому что проверяются они ЧИСЛАМИ, а не картинкой:
// размер текселя каскада, доля плавных пикселей на кромке, площадь тени вдали.
// Эталонный кадр на такое не годится — он поймает изменение, но не скажет, что
// именно стало хуже.
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

} // namespace

void RunShadowChecks(FrameRenderer& r, Scene& scene) {
    TestShadowSoftness(r, scene);
    TestShadowCascades(r);
    TestLocalShadows(r);
}

} // namespace sage::rendertest
