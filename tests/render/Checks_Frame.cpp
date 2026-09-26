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

#include "sage/render/SkyDraw.h"

#include "sage/anim/AnimationSystem.h"
#include "sage/assets/AssetCache.h"
#include "sage/ecs/DecalSystem.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderBatch.h"
#include "sage/render/ScenePasses.h"
#include "sage/render/DebugDraw.h"
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
                                    RenderFrame(r, scene, PerspectiveProj(), BaseChain(), kW, kH)));
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

// НЕБО ВЫКЛЮЧЕНО, ИСТОЧНИКОВ НЕТ — КАДР ЧЁРНЫЙ.
//
// Ровно та жалоба, с которой это чинилось: «отключил небо, удалил всё
// освещение, а свет всё равно есть». Причина была в молчаливой подстановке:
// режим «от неба» при выключенном небе брал поля SkyColor/GroundColor — а они
// по умолчанию голубые и ненулевые, то есть сцену освещало нечто, чего нет ни
// в списке объектов, ни на небе.
//
// Проверяется СВОЙСТВО, а не эталон: при таком свете кадр обязан быть чёрным
// целиком, и любой ненулевой пиксель — это и есть «свет взялся ниоткуда».
void TestNoSkyNoLightIsBlack(FrameRenderer& r) {
    Scene dark("NoSkyNoLight");
    dark.Lighting.Sun.Intensity = 0.0f;             // солнца нет
    dark.Lighting.Skybox.Enabled = false;           // неба нет
    dark.Lighting.AmbientMode = LightingEnvironment::AmbientSource::FromSky;
    // Поля чужого режима намеренно ЯРКИЕ: если они всё же попадут в кадр,
    // проверка это увидит.
    dark.Lighting.SkyColor = glm::vec3(1.0f);
    dark.Lighting.GroundColor = glm::vec3(1.0f);
    dark.Lighting.AmbientStrength = 1.0f;

    GameObject cube = dark.CreateObject("Cube");
    cube.Renderer().Ref = MeshRef{MeshRef::Type::Cube, ""};
    cube.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
    cube.Renderer().Color = glm::vec3(1.0f);
    cube.GetTransform().Position = {0.0f, 0.0f, 0.0f};
    cube.GetTransform().Scale = glm::vec3(2.0f);

    Framebuffer sceneFbo(kW, kH);
    sceneFbo.Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    // Заливка — тем же правилом, что у редактора и игры: нет неба — чернота.
    const glm::vec3 clear = sage::render::SceneClearColor(dark.Lighting);
    device.SetClearColor(clear.r, clear.g, clear.b, 1.0f);
    device.Clear(true, true);
    const LightingEnvironment env = sage::ecs::CollectLighting(dark);
    r.Batch.RenderColor(dark, TestView(), PerspectiveProj(), kEye, env, ShadowBinding(), 0);
    const Image frame = Capture(kW, kH);
    device.BindDefaultFramebuffer();

    int brightest = 0;
    for (unsigned char px : frame.Pixels) brightest = std::max(brightest, (int)px);
    if (brightest <= 1) {
        CountPass();
        std::printf("[ ok ] %-28s ярчайший пиксель %d — свету взяться неоткуда\n",
                    "no_sky_no_light_black", brightest);
    } else {
        CountFail();
        std::printf("[FAIL] %-28s ярчайший пиксель %d — сцена освещена без единого источника\n",
                    "no_sky_no_light_black", brightest);
    }
}

// НЕБО ОДНИМ ЦВЕТОМ — ЭТО РОВНО ОДИН ЦВЕТ.
//
// Режим заведён для сцен, где небо — фон, а не предмет разговора (схема
// уровня, студийная подложка, стилизованная игра). Проверяется свойство, ради
// которого он и нужен: в кадре НЕТ градиента — цвет вверху кадра совпадает с
// цветом внизу, а «чужие» поля (цвет горизонта, закат, звёзды) на него не
// влияют. Процедурное небо здесь же служит противовесом: у него разница
// между верхом и низом обязана быть.
void TestSolidSky(FrameRenderer&) {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    SkyRenderer sky;

    auto render = [&](const LightingEnvironment& env) {
        Framebuffer fbo(kW, kH);
        fbo.Bind();
        device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        device.Clear(true, true);
        // Камера смотрит ГОРИЗОНТАЛЬНО: только так в кадр попадают и верх
        // неба, и горизонт, а значит есть чему отличаться.
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 1.0f, 0.0f),
                                           glm::vec3(0.0f, 1.0f, -1.0f),
                                           glm::vec3(0.0f, 1.0f, 0.0f));
        sage::render::DrawSceneSky(sky, env, view, PerspectiveProj());
        Image img = Capture(kW, kH);
        device.BindDefaultFramebuffer();
        return img;
    };
    // Средний цвет полосы кадра: сверху — зенит, снизу — горизонт.
    auto band = [](const Image& img, int fromRow, int toRow) {
        glm::vec3 sum(0.0f);
        int count = 0;
        for (int y = fromRow; y < toRow; ++y) {
            for (int x = 0; x < kW; ++x) {
                const size_t i = ((size_t)y * kW + (size_t)x) * 3;
                if (i + 2 >= img.Pixels.size()) continue;
                sum += glm::vec3(img.Pixels[i], img.Pixels[i + 1], img.Pixels[i + 2]);
                ++count;
            }
        }
        return count > 0 ? sum / (float)count : sum;
    };

    LightingEnvironment env;
    env.Skybox.Enabled = true;
    env.Skybox.Kind = SkyboxSettings::Source::Procedural;
    env.Skybox.TopColor = glm::vec3(0.10f, 0.20f, 0.70f);
    env.Skybox.HorizonColor = glm::vec3(0.90f, 0.80f, 0.40f);
    env.Skybox.DayNight = false;

    const Image gradient = render(env);
    const glm::vec3 gTop = band(gradient, 0, kH / 5);
    const glm::vec3 gBottom = band(gradient, kH * 4 / 5, kH);
    const float gradientSpread = glm::length(gTop - gBottom);

    env.Skybox.Kind = SkyboxSettings::Source::Solid;
    const Image solid = render(env);
    const glm::vec3 sTop = band(solid, 0, kH / 5);
    const glm::vec3 sBottom = band(solid, kH * 4 / 5, kH);
    const float solidSpread = glm::length(sTop - sBottom);

    std::printf("       разброс по кадру: градиент %.1f, одним цветом %.1f\n", gradientSpread,
                solidSpread);
    if (gradientSpread > 20.0f && solidSpread < 2.0f) {
        CountPass();
        std::printf("[ ok ] %-28s заливка ровная, градиент — нет\n", "sky_solid_colour");
    } else {
        CountFail();
        std::printf("[FAIL] %-28s заливка не ровная (%.1f) либо градиент пропал (%.1f)\n",
                    "sky_solid_colour", solidSpread, gradientSpread);
    }

    // И цвет — ТОТ САМЫЙ, что задан: синий канал заметно сильнее красного.
    if (sTop.b > sTop.r * 1.5f) {
        CountPass();
        std::printf("[ ok ] %-28s цвет неба — заданный (r %.0f, b %.0f)\n", "sky_solid_value",
                    sTop.r, sTop.b);
    } else {
        CountFail();
        std::printf("[FAIL] %-28s цвет неба не тот (r %.0f, b %.0f)\n", "sky_solid_value", sTop.r,
                    sTop.b);
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

    const Image ortho = RenderFrame(r, scene, proj, BaseChain(), kW, kH);
    Report("scene_orthographic", CompareWithReference("scene_orthographic", ortho));

    // Отдельная проверка смысла: орто-кадр обязан ОТЛИЧАТЬСЯ от перспективного.
    // Без неё тест прошёл бы и в случае, если ProjectionMatrix молча вернула
    // перспективу, — эталон просто записался бы дважды одинаковым.
    const Image persp = RenderFrame(r, scene, PerspectiveProj(), BaseChain(), kW, kH);
    long long sum = 0;
    for (size_t i = 0; i < ortho.Pixels.size(); ++i) {
        sum += std::abs((int)ortho.Pixels[i] - (int)persp.Pixels[i]);
    }
    const double mean = (double)sum / (double)ortho.Pixels.size();
    std::printf("       орто против перспективы: среднее расхождение %.2f\n", mean);
    Check(mean > 5.0, "орто-камера даёт другую картинку, чем перспектива");
}

void TestNoPostFX(FrameRenderer& r, Scene& scene) {
    // Пост-обработка ВЫКЛЮЧЕНА — отдельный путь кода, и он тоже должен давать
    // стабильную картинку. Проверяется отсутствием ТРАКТА (nullptr), а не полем
    // в настройках: поля Enabled не читал никто, и эталон этого кадра был снят
    // С пост-обработкой — то есть проверка сторожила копию обычного кадра.
    Report("scene_no_postfx",
           CompareWithReference("scene_no_postfx",
                                RenderFrame(r, scene, PerspectiveProj(), nullptr, kW, kH)));
}

// --- Качество размытия: гладкость и сохранность фокуса -----------------------
//
// Жалоба на глубину резкости звучит как «пикселизация и артефакты», но это ДВА
// разных дефекта, и меряются они по-разному.
//
// Первый — негладкое размытие. Прежний проход брал 24 выборки на диск радиусом
// до 12 пикселей: шаг около 2.4 пикселя, то есть выборок МЕНЬШЕ, чем пикселей в
// диске, а узор у всех пикселей одинаковый. Узор складывался в регулярную
// структуру, а источник не был предварительно отфильтрован, поэтому в размытие
// попадали отдельные детали, а не их средние. На ровном фоне (пол, стена) это
// читается как кольца и сетка. Меряется прямо: средний ПЕРЕПАД яркости там, где
// размытие сработало. Чем глаже размытие, тем он меньше.
//
// Второй — размытие, залезшее в фокус. Широкий переход между резким и размытым
// лечит кольца, но превращает кадр в общую мыльность; это самая частая расплата
// за такую правку, и сторожить надо обе стороны сразу.
//
// Замер идёт по САМОЙ КАРТИНКЕ, а не по внутренностям прохода: тест не должен
// знать, сколько там выборок и в каком разрешении считается сбор.
// --- Качество размытия: не создаёт ли оно собственных кромок -----------------
//
// Жалоба на глубину резкости звучит как «артефакты и пикселизация», и у этого
// ровно один измеримый смысл: размытие, которое само создаёт КРОМКИ. Их дают
// два места прежнего прохода:
//
//   • жёсткий порог «coc > 0.75» делил кадр на резкую и размытую половины с
//     разрывом производной — вокруг плоскости фокуса появлялся контур;
//   • сравнение ГЛУБИН с бинарным выбором веса (1.0 или smoothstep) давало
//     ступеньку на каждом силуэте;
//   • редкая выборка на большом радиусе (24 точки на диск радиусом 12, шаг 2.4
//     пикселя, узор одинаков во всех пикселях) оставляла в размытой части
//     регулярную структуру.
//
// Меряется это картой резкости: у каждого пикселя — насколько он выделяется на
// фоне своих соседей (размытие её опускает, резкость поднимает). Кромка,
// СОЗДАННАЯ размытием, видна как РАЗРЫВ этой карты между соседними пикселями.
// У гладкого перехода разрыв тем меньше, чем шире переход.
//
// Замер идёт по САМОЙ КАРТИНКЕ: тест не знает ни числа выборок, ни разрешения
// промежуточных буферов — иначе он сторожил бы устройство прохода, а не то, что
// видит человек.
struct DofQuality {
    size_t Blurred = 0;   // пикселей, которые размытие тронуло заметно
    double MeanTouched = 0.0; // среднее изменение пикселя, уровней
    double Jump999 = 0.0; // 99.9-й процентиль разрыва карты резкости
    double WorstJump = 0.0;
};

DofQuality MeasureDofQuality(const Image& dof, const Image& sharp) {
    DofQuality q;
    if (dof.Empty() || sharp.Empty() || dof.Pixels.size() != sharp.Pixels.size()) return q;
    const int w = dof.Width, h = dof.Height;
    const size_t s = (size_t)w * 3u;

    auto Luma = [](const unsigned char* p) {
        return 0.299 * (double)p[0] + 0.587 * (double)p[1] + 0.114 * (double)p[2];
    };
    // Насколько размытие тронуло конкретный пиксель: средняя разница по каналам.
    auto Touched = [&](int x, int y) {
        const size_t k = ((size_t)y * w + x) * 3u;
        return (std::abs((int)dof.Pixels[k] - (int)sharp.Pixels[k]) +
                std::abs((int)dof.Pixels[k + 1] - (int)sharp.Pixels[k + 1]) +
                std::abs((int)dof.Pixels[k + 2] - (int)sharp.Pixels[k + 2])) / 3.0;
    };

    std::vector<double> sharpness((size_t)w * h, 0.0);
    for (int y = 1; y + 1 < h; ++y) {
        for (int x = 1; x + 1 < w; ++x) {
            const unsigned char* c = dof.Pixels.data() + ((size_t)y * w + x) * 3u;
            double sum = 0.0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    sum += Luma(c + (size_t)dy * s + (size_t)dx * 3u);
            sharpness[(size_t)y * w + x] = std::abs(Luma(c) - sum / 9.0);
        }
    }

    std::vector<double> jumps;
    for (int y = 1; y + 1 < h; ++y) {
        for (int x = 1; x + 1 < w; ++x) {
            const double t = Touched(x, y);
            if (t > 20.0) ++q.Blurred;
            q.MeanTouched += t;
            const size_t i = (size_t)y * w + x;
            const double s0 = sharpness[i];
            const double d = std::max(std::max(std::abs(s0 - sharpness[i - 1]),
                                               std::abs(s0 - sharpness[i + 1])),
                                      std::max(std::abs(s0 - sharpness[i - w]),
                                               std::abs(s0 - sharpness[i + w])));
            q.WorstJump = std::max(q.WorstJump, d);
            jumps.push_back(d);
        }
    }
    if (!jumps.empty()) {
        q.MeanTouched /= (double)jumps.size();
        std::sort(jumps.begin(), jumps.end());
        q.Jump999 = jumps[(size_t)((double)(jumps.size() - 1) * 0.999)];
    }
    return q;
}

void TestDepthOfField(FrameRenderer& r, Scene& scene) {
    sage::render::PostChain fx = BaseChain();
    AddEffect(fx, "dof");
    SetParam(fx, "dof", "focus", 6.0f); // примерно на сфере
    SetParam(fx, "dof", "aperture", 0.8f);
    // Длинный объектив: кадр проверки мал (круг нерезкости считается в долях
    // высоты кадра), и штатный объектив размывал бы его на пару пикселей.
    SetParam(fx, "dof", "focalLength", 250.0f);
    SetParam(fx, "dof", "maxRadius", 12.0f);
    const Image dof = RenderFrame(r, scene, PerspectiveProj(), fx, kW, kH);
    Report("scene_dof", CompareWithReference("scene_dof", dof));

    const Image sharp = RenderFrame(r, scene, PerspectiveProj(), BaseChain(), kW, kH);
    long long sum = 0;
    for (size_t i = 0; i < dof.Pixels.size(); ++i) {
        sum += std::abs((int)dof.Pixels[i] - (int)sharp.Pixels[i]);
    }
    const double mean = (double)sum / (double)dof.Pixels.size();
    std::printf("       глубина резкости изменила кадр на %.2f в среднем\n", mean);
    Check(mean > 1.0, "глубина резкости реально влияет на картинку");

    const DofQuality q = MeasureDofQuality(dof, sharp);
    std::printf("       радиус 12: размытых %zu, разрыв карты резкости 99.9%% %.1f худший %.1f\n",
                q.Blurred, q.Jump999, q.WorstJump);
    // Маска обязана быть непустой: «разрывов нет» на кадре, где размытия нет
    // вовсе, выполняется само собой, и такая проверка ничего не сторожит.
    Check(q.Blurred > 1000, "в кадре есть заметно размытые участки");

    // Замер на УВЕЛИЧЕННОМ радиусе — здесь и живёт дефект.
    //
    // На штатном радиусе 12 диск ещё покрыт достаточно плотно, а на ровном полу
    // и стене редкая выборка почти не ошибается: все выборки возвращают один и
    // тот же цвет. Ошибается она там, где внутри диска есть ПЕРЕПАД, и тем
    // сильнее, чем больше диск, — поэтому свойство меряется на радиусе 28, то
    // есть ровно в том случае, который человек получает, потянув ползунок
    // радиуса вверх. Замер прежнего прохода здесь: 99.9% 11.3, худший 20.1;
    // новый даёт 4.8 и 9.4. Пороги стоят между ними, с запасом в обе стороны.
    SetParam(fx, "dof", "maxRadius", 28.0f);
    const Image wide = RenderFrame(r, scene, PerspectiveProj(), fx, kW, kH);
    const DofQuality qw = MeasureDofQuality(wide, sharp);
    std::printf("       радиус 28: размытых %zu, разрыв карты резкости 99.9%% %.1f худший %.1f\n",
                qw.Blurred, qw.Jump999, qw.WorstJump);
    // «Шире» — это больше изменения по кадру в целом, а не больше пикселей с
    // сильным перепадом: широкое размытие размазывает перепад тоньше, и
    // счётчик «изменился больше чем на 20» от ширины как раз падает.
    std::printf("       среднее изменение: радиус 12 — %.2f, радиус 28 — %.2f\n", q.MeanTouched,
                qw.MeanTouched);
    Check(qw.MeanTouched > q.MeanTouched * 1.1, "на большом радиусе размытие заметно шире");
    Check(qw.Jump999 < 7.0, "размытие не создаёт разрывов карты резкости");
    Check(qw.WorstJump < 16.0, "худший разрыв карты резкости в пределах допуска");
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
    // Ambient здесь задан ЯВНО и небу не подчиняется — значит «свои значения»
    // (см. LightingEnvironment::AmbientMode). Выключенное небо иначе забирает
    // с собой и окружающий свет, и проверять было бы нечего: чёрный кадр.
    scene->Lighting.AmbientMode = LightingEnvironment::AmbientSource::Custom;
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

    sage::render::PostChain fx = BaseChain();
    // Свечение размыло бы кромку, затенение добавило бы к измеряемому перепаду
    // своё, а виньетка — перепад яркости к краям. Сглаживание в эталонном тракте
    // и так выключено (см. BaseChain).
    RemoveEffect(fx, "bloom");
    RemoveEffect(fx, "ao");
    SetParam(fx, "tonemap", "vignette", 0.0f);

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
    // Ambient здесь задан ЯВНО и небу не подчиняется — значит «свои значения»
    // (см. LightingEnvironment::AmbientMode). Выключенное небо иначе забирает
    // с собой и окружающий свет, и проверять было бы нечего: чёрный кадр.
    scene->Lighting.AmbientMode = LightingEnvironment::AmbientSource::Custom;
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

    sage::render::PostChain fx = BaseChain();
    AddEffect(fx, "motionblur");
    SetParam(fx, "motionblur", "amount", 1.0f);
    // Свечение размазало бы кромку и смазало границу измерения, а затенение
    // добавило бы к ней свой перепад.
    RemoveEffect(fx, "bloom");
    RemoveEffect(fx, "ao");

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
    sage::render::PostChain sharpFx = fx;
    RemoveEffect(sharpFx, "motionblur");
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
    sage::render::PostChain fx = BaseChain();
    AddEffect(fx, "fxaa");
    const Image aa = RenderFrame(r, scene, PerspectiveProj(), fx, kW, kH);
    Report("scene_fxaa", CompareWithReference("scene_fxaa", aa));

    const Image raw = RenderFrame(r, scene, PerspectiveProj(), BaseChain(), kW, kH);

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
        // Свет — СВОИМИ значениями: неба в сцене нет, а «от неба» без неба это
        // темнота (см. LightingEnvironment::ResolveAmbient). На чёрном кадре
        // проверять прозрачность нечем.
        local.Lighting.AmbientMode = LightingEnvironment::AmbientSource::Custom;
        GameObject cube = local.CreateObject("Glass");
        MeshRendererComponent& mr = cube.Renderer();
        mr.Ref = MeshRef{MeshRef::Type::Cube, ""};
        mr.MeshPtr = std::make_shared<Mesh>(md.Vertices, md.Indices);
        // Полупрозрачность — материалом: своей непрозрачности у экземпляра
        // больше нет, она вся живёт в .sagemat.
        auto glass = std::make_shared<Material>();
        glass->Albedo = glm::vec3(0.8f, 0.25f, 0.25f);
        glass->Opacity = 0.45f;   // именно здесь порядок граней и решает
        mr.MaterialPtr = glass;
        cube.GetTransform().Position = glm::vec3(0.0f, 0.0f, 0.0f);
        cube.GetTransform().Scale = glm::vec3(2.0f);
        return RenderFrame(r, local, PerspectiveProj(), BaseChain(), kW, kH);
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
    opaqueScene.Lighting.AmbientMode = LightingEnvironment::AmbientSource::Custom;
    GameObject solid = opaqueScene.CreateObject("Solid");
    MeshRendererComponent& smr = solid.Renderer();
    smr.Ref = MeshRef{MeshRef::Type::Cube, ""};
    smr.MeshPtr = std::make_shared<Mesh>(data.Vertices, data.Indices);
    auto solidMat = std::make_shared<Material>();
    solidMat->Albedo = glm::vec3(0.8f, 0.25f, 0.25f);
    solidMat->Opacity = 1.0f;
    smr.MaterialPtr = solidMat;
    solid.GetTransform().Scale = glm::vec3(2.0f);
    const Image opaque = RenderFrame(r, opaqueScene, PerspectiveProj(), BaseChain(), kW, kH);
    long long diff = 0;
    for (size_t i = 0; i < a.Pixels.size() && i < opaque.Pixels.size(); ++i)
        diff += std::abs((int)a.Pixels[i] - (int)opaque.Pixels[i]);
    const double opaqueMean = (double)diff / (double)std::max<size_t>(a.Pixels.size(), 1);
    std::printf("       прозрачный против непрозрачного: %.2f\n", opaqueMean);
    // Порог с запасом, но НЕ на грани: если бы непрозрачность игнорировалась,
    // кадры совпали бы в ноль, а здесь разница около двух единиц яркости.
    // Прежние 2.0 были подобраны под старую кривую света (контраст считался
    // после гаммы и растягивал тёмное); теперь контраст живёт в линейном кадре
    // вокруг средне-серого, и та же разница выходит чуть меньше.
    Check(opaqueMean > 1.0, "прозрачность действительно применяется");
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
        return RenderFrame(r, local, PerspectiveProj(), BaseChain(), kW, kH);
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
        // Свет — явной строкой: сцена сама собой больше не светится (см.
        // LightingEnvironment::Sun), а наклейку надо чем-то осветить.
        local.Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f));
        local.Lighting.Sun.Intensity = 1.6f;
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
        sage::render::PostChain fx = BaseChain();
        RemoveEffect(fx, "bloom");  // ореол размыл бы границу, которую меряем
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

// --- Сетка не рвёт гизмо на своей плоскости ---------------------------------
//
// В редакторе после сетки рисуются оси выделенного объекта и прочие гизмо. Ось
// X объекта в начале координат лежит ровно на оси X сетки, и пока сетка писала
// глубину (да ещё со сдвигом к камере), линия проигрывала ей тест: вместо
// чистой оси объекта проступала приглушённая ось сетки, а сама линия рвалась.
// Проверка: зелёная линия по оси X поверх сетки видна, пока сетка глубину не
// пишет, и пропадает, если пишет (старое поведение редактора).
void TestGridKeepsGizmoLinesOnItsPlane(FrameRenderer& r) {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    const glm::mat4 view = TestView();
    const glm::mat4 proj = PerspectiveProj();
    auto draw = [&](bool writeDepth) {
        Framebuffer out(kW, kH);
        out.Bind();
        device.SetSRGBWrite(false);
        device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
        device.Clear(true, true);
        device.SetDepthTest(true);
        device.SetDepthWrite(true);
        sage::render::GridSettings gs;
        gs.CellSize = 1.0f;
        gs.WriteDepth = writeDepth;
        r.Grid.Draw(view, proj, kEye, gs);
        DebugDraw dd;
        dd.Line({-40.0f, 0.0f, 0.0f}, {40.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
        dd.Flush(view, proj);
        const Image im = Capture(kW, kH);
        long long green = 0;
        for (size_t i = 0; i + 2 < im.Pixels.size(); i += 3) {
            const int rr = im.Pixels[i], gg = im.Pixels[i + 1], bb = im.Pixels[i + 2];
            if (gg > 200 && rr < 90 && bb < 90) ++green;
        }
        return green;
    };
    const long long editor = draw(false);
    const long long occluding = draw(true);
    std::printf("       пикселей оси поверх сетки: без записи глубины %lld, с записью %lld\n",
                editor, occluding);
    Check(editor > 100, "ось на плоскости сетки видна целиком, сетка её не рвёт");
    Check(editor > occluding * 2, "именно запись глубины сеткой прятала ось");
}

// --- Заливка гизмо: объём по нормалям и прозрачность --------------------------
//
// Режим «заливка» у гизмо коллайдера обязан давать ОБЪЁМ: видимые грани
// коробки разной яркости (иначе это плоское пятно, по которому перед от бока не
// отличить), и оставаться полупрозрачным (иначе закрывает модель, ради которой
// коллайдер и смотрят).
void TestSolidGizmoShading(FrameRenderer&) {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    const glm::mat4 view = glm::lookAt(glm::vec3(3.0f, 2.5f, 4.0f), glm::vec3(0.0f), glm::vec3(0, 1, 0));
    const glm::mat4 proj = PerspectiveProj();
    auto draw = [&](float alpha) {
        Framebuffer out(kW, kH);
        out.Bind();
        device.SetSRGBWrite(false);
        device.SetClearColor(0.0f, 0.0f, 1.0f, 1.0f);   // синий фон — видно, просвечивает ли он
        device.Clear(true, true);
        device.SetDepthTest(true);
        DebugDraw dd;
        dd.SolidBox(glm::scale(glm::mat4(1.0f), glm::vec3(2.0f)), glm::vec4(0.2f, 1.0f, 0.3f, alpha));
        dd.Flush(view, proj);
        return Capture(kW, kH);
    };
    const Image opaque = draw(1.0f);
    const Image faint = draw(0.3f);
    // Яркости зелёного у закрашенных пикселей: у разных граней они обязаны
    // заметно разойтись.
    int lo = 255, hi = 0;
    long long covered = 0, blueThrough = 0;
    for (size_t i = 0; i + 2 < opaque.Pixels.size(); i += 3) {
        const int g = opaque.Pixels[i + 1], b = opaque.Pixels[i + 2];
        if (g < 20 || b > 200) continue;   // фон
        ++covered;
        lo = std::min(lo, g);
        hi = std::max(hi, g);
        if (faint.Pixels[i + 2] > 80) ++blueThrough;
    }
    std::printf("       заливка: покрыто %lld пикселей, зелёный %d..%d, фон просвечивает у %lld\n",
                covered, lo, hi, blueThrough);
    Check(covered > 2000, "залитая форма видна");
    Check(hi - lo > 40, "грани затенены по нормалям по-разному — виден объём");
    Check(blueThrough > covered * 9 / 10, "полупрозрачная заливка не закрывает то, что за ней");
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

    const Image plain = RenderFrame(r, scene, proj, BaseChain(), kW, kH);
    const Image withInfinite = RenderFrame(r, scene, proj, BaseChain(), kW, kH, &infinite);
    const Image withRadius = RenderFrame(r, scene, proj, BaseChain(), kW, kH, &radius);

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
    Check(meanDiff(RenderFrame(r, scene, proj, BaseChain(), kW, kH, &off), plain) < 0.001,
          "выключенная сетка не рисуется");

    // Прозрачность обязана влиять монотонно: половинная сетка ближе к пустому
    // кадру, чем полная.
    sage::render::GridSettings faint = infinite;
    faint.Opacity = 0.35f;
    const double faintVsPlain =
        meanDiff(RenderFrame(r, scene, proj, BaseChain(), kW, kH, &faint), plain);
    std::printf("       полупрозрачная сетка против пустого кадра: %.2f\n", faintVsPlain);
    Check(faintVsPlain > 0.05 && faintVsPlain < infiniteVsPlain,
          "прозрачность сетки ослабляет её, но не убирает");

    // Шаг клетки обязан менять картинку: без этого настройка была бы мёртвой.
    sage::render::GridSettings coarse = infinite;
    coarse.CellSize = 4.0f;
    Check(meanDiff(RenderFrame(r, scene, proj, BaseChain(), kW, kH, &coarse), withInfinite) > 0.5,
          "шаг клетки меняет сетку");
}

} // namespace


// --- Обычный вид с пост-обработкой не может быть чёрным ---------------------
//
// Ровно тот отказ, с которым пришли: во вьюпорте ВСЕ ОБЪЕКТЫ ЧЁРНЫЕ, а
// отладочные режимы работают. Разгадка в том, что отладочные режимы рисуются
// БЕЗ пост-обработки — то есть чёрным был не шейдинг, а цепочка эффектов.
// Проверок на «кадр без поста не чёрный» было две, а на кадр С ПОСТОМ — ни
// одной, хотя именно его видит человек по умолчанию.
void TestShadedWithPostIsNotBlack(FrameRenderer& r, Scene& scene) {
    Framebuffer sceneFbo(kW, kH);
    Framebuffer outFbo(kW, kH);
    const LightingEnvironment env = sage::ecs::CollectLighting(scene);
    const glm::mat4 view = TestView();
    const glm::mat4 proj = PerspectiveProj();

    sceneFbo.Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
    device.Clear(true, true);
    r.Batch.RenderColor(scene, view, proj, kEye, env, ShadowBinding(), 0);
    sceneFbo.Resolve();

    sage::render::PostChain fx = BaseChain();
    r.Fx.Render(sceneFbo.ColorTexture(), sceneFbo.DepthTexture(), kW, kH, proj, view, fx, &outFbo,
                0, 0, kW, kH);
    outFbo.Resolve();
    outFbo.Bind();
    const Image frame = Capture(kW, kH);
    device.BindDefaultFramebuffer();

    double sum = 0.0;
    for (unsigned char c : frame.Pixels) sum += c;
    const double mean = frame.Pixels.empty() ? 0.0 : sum / (double)frame.Pixels.size();
    if (mean > 24.0) {
        CountPass();
        std::printf("[ ok ] %-28s среднее %.1f (кадр с постом освещён)\n", "post_not_black", mean);
    } else {
        CountFail();
        std::printf("[FAIL] %-28s среднее %.1f — кадр С ПОСТ-ОБРАБОТКОЙ ЧЁРНЫЙ\n",
                    "post_not_black", mean);
    }
}

// --- Самопроверка цепочки эффектов ------------------------------------------
//
// Тот же вопрос, но задаваемый самим движком на чужой машине: цепочку прогоняют
// по заведомо серому кадру и смотрят, не пропала ли картинка. Здесь проверяется,
// что проверка НЕ ЛЖЁТ — на исправной машине она обязана говорить «работает»,
// иначе редактор молча отключит эффекты у всех.
// --- Цвет вывода и кривые: ровная заливка заданной яркости через тракт -------
//
// Кадр — сплошной HDR-цвет (очистка буфера сцены), поэтому меряется ровно то,
// что делает тракт, без геометрии и света.
float FlatThroughChain(FrameRenderer& r, float hdr, const sage::render::PostChain& chain,
                       bool resetHistory = true) {
    const int w = 64, h = 64;
    Framebuffer sceneFbo(w, h), output(w, h);
    sceneFbo.Bind();
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    device.SetClearColor(hdr, hdr, hdr, 1.0f);
    device.Clear(true, true);
    if (resetHistory) r.Fx.ResetHistory();
    const glm::mat4 id(1.0f);
    r.Fx.Render(sceneFbo.ColorTexture(), sceneFbo.DepthTexture(), w, h, id, id, chain, &output, 0, 0, w, h);
    output.Bind();
    const Image img = Capture(w, h);
    double sum = 0.0;
    for (unsigned char v : img.Pixels) sum += v;
    return img.Pixels.empty() ? 0.0f : (float)(sum / (double)img.Pixels.size() / 255.0);
}

sage::render::PostChain ToneOnly(float mode, float output) {
    sage::render::PostChain chain;
    AddEffect(chain, "tonemap");
    SetParam(chain, "tonemap", "mode", mode);
    SetParam(chain, "tonemap", "output", output);
    return chain;
}

void TestOutputEncodingAndCurves(FrameRenderer& r) {
    // Средне-серый 18 %: в sRGB он около 118/255, линейный вывод оставляет
    // его 46/255. Выбор «sRGB / линейный» обязан менять кадр именно так.
    const float srgb = FlatThroughChain(r, 0.18f, ToneOnly(0.0f, 0.0f));
    const float linear = FlatThroughChain(r, 0.18f, ToneOnly(0.0f, 2.0f));
    const float gamma = FlatThroughChain(r, 0.18f, ToneOnly(0.0f, 1.0f));
    std::printf("       серый 0.18: sRGB %.3f, гамма %.3f, линейный %.3f\n", srgb, gamma, linear);
    Check(std::abs(srgb - 0.461f) < 0.02f, "вывод sRGB кодирует серый по стандарту");
    Check(std::abs(linear - 0.18f) < 0.02f, "линейный вывод оставляет значение как есть");
    Check(std::abs(gamma - 0.459f) < 0.02f, "вывод «гамма» — степенная кривая 2.2");

    // Кинематографическая кривая держит средне-серый на месте (InMatch = OutMatch =
    // 0.18) и сжимает света: 16x ярче серого — всё ещё не белый в клип.
    const float ueGrey = FlatThroughChain(r, 0.18f, ToneOnly(4.0f, 2.0f));
    const float ueBright = FlatThroughChain(r, 2.88f, ToneOnly(4.0f, 2.0f));
    const float agxGrey = FlatThroughChain(r, 0.18f, ToneOnly(5.0f, 0.0f));
    const float neutralGrey = FlatThroughChain(r, 0.18f, ToneOnly(6.0f, 2.0f));
    std::printf("       Cinematic: серый %.3f, x16 %.3f; AgX серый %.3f; Neutral серый %.3f\n",
                ueGrey, ueBright, agxGrey, neutralGrey);
    Check(std::abs(ueGrey - 0.18f) < 0.04f, "кинематографическая кривая держит средне-серый на месте");
    Check(ueBright > 0.6f && ueBright < 0.99f, "кинематографическая кривая сжимает света, а не обрезает");
    Check(agxGrey > 0.3f && agxGrey < 0.7f, "AgX даёт среднюю яркость для серого");
    Check(std::abs(neutralGrey - 0.14f) < 0.04f, "Neutral не трогает цвета ниже плеча");
}

// --- Автоэкспозиция: глаз привыкает к свету сцены ---------------------------
void TestAutoExposure(FrameRenderer& r) {
    sage::render::PostChain chain = ToneOnly(4.0f, 0.0f);
    AddEffect(chain, "autoexposure");
    // Без привыкания тёмная и светлая сцены различаются в 64 раза по свету.
    const float darkRaw = FlatThroughChain(r, 0.02f, ToneOnly(4.0f, 0.0f));
    const float brightRaw = FlatThroughChain(r, 1.28f, ToneOnly(4.0f, 0.0f));
    const float dark = FlatThroughChain(r, 0.02f, chain);
    const float bright = FlatThroughChain(r, 1.28f, chain);
    std::printf("       без адаптации: %.3f / %.3f, с адаптацией: %.3f / %.3f\n", darkRaw, brightRaw,
                dark, bright);
    Check(std::abs(dark - bright) < 0.05f, "автоэкспозиция приводит тёмную и светлую сцены к одной яркости");
    Check(brightRaw - darkRaw > 0.4f, "без неё сцены заметно разные (проверка не пустая)");

    // Привыкание ПЛАВНОЕ: после смены сцены первый кадр ещё помнит прошлую.
    FlatThroughChain(r, 1.28f, chain);                         // привыкли к свету
    const float firstDark = FlatThroughChain(r, 0.02f, chain, /*resetHistory=*/false);
    std::printf("       первый кадр в темноте после света: %.3f (привыкший глаз: %.3f)\n",
                firstDark, dark);
    Check(firstDark < dark - 0.05f, "глаз привыкает к темноте не мгновенно");
}

// --- Автоэкспозиция не выжигает кадр с чёрным небом --------------------------
//
// Жалоба: лампа на полу, небо чёрное — и весь освещённый пол выгорел в белое,
// а свечение обвело каждый предмет каймой. Замер брал простое среднее, и
// пустота (70 % кадра) тянула его вниз: глаз вытягивал кадр до предела.
// Здесь кадр — 70 % чёрного и 30 % серого 0.5: освещённая часть обязана выйти
// такой же, как если бы весь кадр был серым 0.5, а не белой.
void TestAutoExposureIgnoresVoid(FrameRenderer& r) {
    const int w = 64, h = 64;
    auto frameWithLit = [&](float litShare) {
        std::vector<unsigned char> px((size_t)w * h * 4, 0);
        const int litRows = (int)(h * litShare);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                unsigned char* p = &px[((size_t)y * w + x) * 4];
                const bool lit = y < litRows;
                p[0] = p[1] = p[2] = lit ? 128 : 0;
                p[3] = 255;
            }
        sage::rhi::Texture2DDesc d;
        d.Width = w; d.Height = h; d.Channels = 4;
        d.FilterMode = sage::rhi::Filter::Nearest;
        d.WrapMode = sage::rhi::Wrap::ClampEdge;
        d.GenerateMipmaps = false;
        return sage::rhi::GraphicsDevice::Get().CreateTexture2D(d, px.data());
    };
    sage::render::PostChain chain = ToneOnly(0.0f, 2.0f);   // обрезка, линейный вывод
    AddEffect(chain, "autoexposure");
    auto litValue = [&](float share) {
        auto tex = frameWithLit(share);
        Framebuffer output(w, h);
        r.Fx.ResetHistory();
        const glm::mat4 id(1.0f);
        r.Fx.Render(tex->Handle(), sage::rhi::TextureHandle{}, w, h, id, id, chain, &output, 0, 0, w, h);
        output.Bind();
        const Image img = Capture(w, h);
        // Строки Capture идут сверху вниз, а освещённые строки текстуры — снизу
        // (строка 0 текстуры — низ кадра): берём середину нижней полосы.
        const int y = h - 1 - (int)(h * share * 0.5f);
        return img.Pixels[((size_t)y * w + w / 2) * 3] / 255.0f;
    };
    const float full = litValue(1.0f);
    const float partial = litValue(0.3f);
    std::printf("       серый 0.5: весь кадр %.3f, 30 %% кадра среди чёрного %.3f\n", full, partial);
    Check(partial < 0.95f, "освещённое среди чёрного неба не выгорает в белое");
    Check(std::abs(partial - full) < 0.12f, "чёрная пустота не меняет экспозицию освещённого");
}

// --- Смаз камеры по небу — и с буфером скоростей ---------------------------
//
// В редакторе и в игре смаз не работал вовсе: буфер скоростей никто не
// рисовал. Теперь его рисует VelocityBuffer вида — и тут легко потерять
// другое: у неба нет геометрии, в буфере скоростей там ноль, и небо при
// повороте камеры осталось бы резким. Пиксели без геометрии помечены, и
// для них звено смаза берёт движение камеры по глубине.
//
// Кадр — вертикальные полосы на «бесконечной» глубине (1.0), геометрии нет.
// Камера поворачивается на 4°: полосы обязаны размазаться.
void TestCameraMotionBlurOverSky(FrameRenderer& r) {
    const int w = 128, h = 64;
    std::vector<unsigned char> px((size_t)w * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned char* p = &px[((size_t)y * w + x) * 4];
            p[0] = p[1] = p[2] = ((x / 4) % 2) ? 230 : 20;
        }
    sage::rhi::Texture2DDesc d;
    d.Width = w; d.Height = h; d.Channels = 4;
    d.FilterMode = sage::rhi::Filter::Nearest;
    d.WrapMode = sage::rhi::Wrap::ClampEdge;
    d.GenerateMipmaps = false;
    auto stripes = sage::rhi::GraphicsDevice::Get().CreateTexture2D(d, px.data());

    Framebuffer depthOnly(w, h);   // глубина 1.0 — «бесконечность», как у неба
    depthOnly.Bind();
    sage::rhi::GraphicsDevice::Get().SetClearColor(0, 0, 0, 1);
    sage::rhi::GraphicsDevice::Get().Clear(true, true);

    // Пустая сцена: в буфер скоростей не попадёт ни одного объекта.
    Scene empty("Sky");
    sage::ecs::RenderBatch batch;
    const LightingEnvironment env = sage::ecs::CollectLighting(empty);
    batch.RenderColor(empty, glm::mat4(1.0f), glm::mat4(1.0f), glm::vec3(0.0f), env,
                      ShadowBinding(), 0);

    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), (float)w / h, 0.1f, 100.0f);
    const glm::mat4 view1 = glm::lookAt(glm::vec3(0.0f), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
    const glm::mat4 view2 = glm::rotate(glm::mat4(1.0f), glm::radians(4.0f), glm::vec3(0, 1, 0)) * view1;

    sage::render::PostChain chain = ToneOnly(0.0f, 2.0f);
    AddEffect(chain, "motionblur");
    SetParam(chain, "motionblur", "amount", 1.0f);
    sage::render::VelocityBuffer velocity;
    auto frame = [&](const glm::mat4& view, const sage::render::PostChain& c) {
        const sage::rhi::TextureHandle vel = velocity.Render(batch, view, proj, w, h);
        Framebuffer out(w, h);
        r.Fx.Render(stripes->Handle(), depthOnly.DepthTexture(), w, h, proj, view, c, &out, 0, 0,
                    w, h, vel);
        out.Bind();
        return Capture(w, h);
    };
    r.Fx.ResetHistory();
    velocity.Reset();
    frame(view1, chain);                       // история: прошлый кадр — view1
    const Image blurred = frame(view2, chain);  // повернулись
    // Контраст полос: у резких — 210 уровней, у смазанных заметно меньше.
    auto contrast = [&](const Image& img) {
        int lo = 255, hi = 0;
        const int y = h / 2;
        for (int x = w / 4; x < w * 3 / 4; ++x) {
            const int v = img.Pixels[((size_t)y * w + x) * 3];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        return hi - lo;
    };
    const int c = contrast(blurred);
    std::printf("       полосы неба при повороте камеры: контраст %d (резкие — 210)\n", c);
    Check(c < 150, "небо без геометрии смазывается при повороте камеры (с буфером скоростей)");
}

// --- Глубина резкости: передний план расплывается ПОВЕРХ резкого фона --------
//
// Жалоба «ужасное качество, куча артефактов»: размытый ближний предмет
// обрывался резкой кромкой по своему силуэту, потому что пиксель фона рядом с
// ним резкий и смешивание шло по СВОЕЙ глубине пикселя. В оптике же передний
// план расплывается за свой силуэт. Сцена: белый светящийся куб вблизи,
// тёмная стена в фокусе. Пиксели стены у кромки куба обязаны посветлеть, а
// стена вдали от куба — остаться какой была (без ореолов).
void TestDepthOfFieldNearSpreads(FrameRenderer& r) {
    const int w = 160, h = 120;
    auto scene = std::make_unique<Scene>("DofNear");
    scene->Lighting.AmbientMode = LightingEnvironment::AmbientSource::Custom;
    scene->Lighting.AmbientStrength = 0.0f;
    scene->Lighting.Sun.Intensity = 0.0f;
    scene->Lighting.Skybox.Enabled = false;
    auto box = [&](const char* name, glm::vec3 pos, glm::vec3 size, glm::vec3 glow) {
        GameObject o = scene->CreateObject(name);
        o.GetTransform().Position = pos;
        o.GetTransform().Scale = size;
        o.Renderer().Ref = MeshRef{MeshRef::Type::Cube};
        o.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
        auto m = std::make_shared<Material>();
        m->Albedo = {0.0f, 0.0f, 0.0f};
        m->Emissive = glow;
        o.Renderer().MaterialPtr = m;
    };
    box("Wall", {0.0f, 0.0f, -10.0f}, {40.0f, 40.0f, 0.2f}, {0.08f, 0.08f, 0.08f});
    box("Near", {-0.5f, 0.0f, -1.5f}, {1.0f, 3.0f, 0.2f}, {1.0f, 1.0f, 1.0f});   // правая кромка — x = 0

    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
    const glm::mat4 proj = glm::perspective(glm::radians(50.0f), (float)w / h, 0.1f, 100.0f);
    auto render = [&](bool dof) {
        Framebuffer sceneFbo(w, h), out(w, h);
        sceneFbo.Bind();
        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        device.Clear(true, true);
        const LightingEnvironment env = sage::ecs::CollectLighting(*scene);
        r.Batch.RenderColor(*scene, view, proj, glm::vec3(0.0f), env, ShadowBinding(), 0);
        sage::render::PostChain chain = ToneOnly(0.0f, 2.0f);
        if (dof) {
            AddEffect(chain, "dof");
            SetParam(chain, "dof", "focus", 10.0f);
            SetParam(chain, "dof", "aperture", 1.4f);
            SetParam(chain, "dof", "focalLength", 150.0f);
            SetParam(chain, "dof", "maxRadius", 16.0f);
        }
        r.Fx.ResetHistory();
        r.Fx.Render(sceneFbo.ColorTexture(), sceneFbo.DepthTexture(), w, h, proj, view, chain,
                    &out, 0, 0, w, h);
        out.Bind();
        return Capture(w, h);
    };
    const Image sharp = render(false);
    const Image blurred = render(true);
    // Кромка куба — по резкому кадру: первый тёмный столбец средней строки.
    const int y = h / 2;
    auto at = [&](const Image& img, int x) { return (int)img.Pixels[((size_t)y * w + x) * 3]; };
    int edge = -1;
    for (int x = 1; x < w; ++x)
        if (at(sharp, x - 1) > 128 && at(sharp, x) < 64) { edge = x; break; }
    Check(edge > 0 && edge < w - 40, "кромка переднего предмета найдена");
    if (edge <= 0 || edge >= w - 40) return;
    const int nearBg = at(blurred, edge + 3), farBg = at(blurred, edge + 35), bg = at(sharp, edge + 35);
    std::printf("       фон у кромки %d, фон вдали %d (резкий фон %d)\n", nearBg, farBg, bg);
    Check(nearBg > farBg + 25, "размытый передний план расплывается поверх резкого фона");
    Check(std::abs(farBg - bg) <= 4, "фон в фокусе вдали от предмета не тронут (нет ореолов)");
}

// --- Каждое звено работает, выключенное — молчит ---------------------------
//
// Жалоба: «не все эффекты отключаются и не все работают». Проверяется
// каталог целиком: у каждого звена есть кадр, где оно заметно меняет
// картинку, а выключенное звено (галка снята, звено в тракте осталось) не
// меняет ничего. Смаз, адаптация глаза и блик проверяются своими тестами:
// им нужно движение, время и солнце в кадре.
void TestEveryEffectToggles(FrameRenderer& r, Scene& scene) {
    const sage::render::PostChain base = ToneOnly(2.0f, 0.0f);
    const Image plain = RenderFrame(r, scene, PerspectiveProj(), base, kW, kH);
    auto meanDiff = [](const Image& a, const Image& b) {
        long long sum = 0;
        for (size_t i = 0; i < a.Pixels.size(); ++i) sum += std::abs((int)a.Pixels[i] - (int)b.Pixels[i]);
        return (double)sum / (double)a.Pixels.size();
    };
    for (const sage::render::PostEffectKind* kind : sage::render::PostEffectCatalog::Instance().All()) {
        const std::string& id = kind->Id;
        if (id == "motionblur" || id == "autoexposure" || id == "lensflare" || id == "tonemap") continue;
        sage::render::PostChain on = base;
        AddEffect(on, id.c_str());
        // Звенья, которые по умолчанию нейтральны, настраиваются на заметное.
        if (id == "exposure") SetParam(on, id.c_str(), "exposure", 1.0f);
        if (id == "color") SetParam(on, id.c_str(), "saturation", 0.0f);
        // Свечение — только того, что ярче порога; в этой сцене ярче 1.0 нет
        // ничего, и при штатном пороге звену законно нечего делать.
        if (id == "bloom") SetParam(on, id.c_str(), "threshold", 0.2f);
        if (id == "grading") {
            for (sage::render::PostEffect& e : on.Effects)
                if (e.Kind == id)
                    if (sage::render::PostValue* v = e.Find("shadows")) { v->V[0] = 2.0f; v->V[2] = 0.3f; }
        }
        if (id == "dof") {
            SetParam(on, id.c_str(), "focus", 1.0f);
            SetParam(on, id.c_str(), "focalLength", 200.0f);
        }
        const Image withIt = RenderFrame(r, scene, PerspectiveProj(), on, kW, kH);
        sage::render::PostChain off = on;
        for (sage::render::PostEffect& e : off.Effects)
            if (e.Kind == id) e.Enabled = false;
        const Image withOff = RenderFrame(r, scene, PerspectiveProj(), off, kW, kH);
        const double dOn = meanDiff(withIt, plain), dOff = meanDiff(withOff, plain);
        std::printf("       %-12s включено %.3f, выключено %.3f\n", id.c_str(), dOn, dOff);
        Check(dOn > 0.02, ("звено «" + id + "» меняет кадр").c_str());
        Check(dOff < 0.001, ("выключенное звено «" + id + "» кадр не трогает").c_str());
    }
}

// --- Высотный туман: плотнее у земли, реже вверху ---------------------------
//
// Высокая колонна вдали и туман ядовито-пурпурного цвета: у основания колонна
// обязана уйти в пурпур сильнее, чем у вершины. Линейный туман их не различает.
void TestHeightFog(FrameRenderer& r) {
    auto run = [&](FogSettings::Mode mode, float& bottom, float& top) {
        auto scene = std::make_unique<Scene>("Fog");
        scene->Lighting.Sun.Direction = glm::normalize(glm::vec3(-0.3f, -0.8f, -0.5f));
        scene->Lighting.Sun.Intensity = 1.0f;
        scene->Lighting.AmbientMode = LightingEnvironment::AmbientSource::Custom;
        scene->Lighting.AmbientStrength = 0.2f;
        scene->Lighting.Skybox.Enabled = false;
        FogSettings& f = scene->Lighting.Fog;
        f.Enabled = true;
        f.Kind = mode;
        f.Color = {1.0f, 0.0f, 1.0f};
        f.Start = 0.0f;
        f.End = 30.0f;
        f.Density = 0.25f;
        f.HeightFalloff = 0.8f;
        f.BaseHeight = 0.0f;
        f.SunScatter = 0.0f;
        GameObject pillar = scene->CreateObject("Pillar");
        pillar.GetTransform().Position = {0.0f, 4.0f, -12.0f};
        pillar.GetTransform().Scale = {2.0f, 8.0f, 2.0f};
        pillar.Renderer().Ref = MeshRef{MeshRef::Type::Cube};
        pillar.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
        pillar.Renderer().Color = {0.1f, 0.8f, 0.1f};   // зелёная
        const int w = 96, h = 96;
        Framebuffer out(w, h);
        out.Bind();
        auto& device = sage::rhi::GraphicsDevice::Get();
        device.SetSRGBWrite(true);
        device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        device.Clear(true, true);
        const glm::vec3 eye(0.0f, 4.0f, 10.0f);
        const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f, 4.0f, -12.0f), glm::vec3(0, 1, 0));
        const glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
        const LightingEnvironment env = sage::ecs::CollectLighting(*scene);
        r.Batch.RenderColor(*scene, view, proj, eye, env, ShadowBinding(), 0);
        device.SetSRGBWrite(false);
        const Image img = Capture(w, h);
        // Доля пурпура: (R+B)/2 - G в пятне у низа и у верха колонны.
        auto magenta = [&](float fy) {
            double s = 0.0; int n = 0;
            for (int y = (int)(fy * h) - 2; y <= (int)(fy * h) + 2; ++y)
                for (int x = w / 2 - 2; x <= w / 2 + 2; ++x) {
                    const size_t i = ((size_t)y * w + x) * 3;
                    s += (img.Pixels[i] + img.Pixels[i + 2]) * 0.5 - img.Pixels[i + 1];
                    ++n;
                }
            return (float)(s / n / 255.0);
        };
        bottom = magenta(0.66f);   // колонна занимает примерно 0.28..0.72 высоты кадра
        top = magenta(0.34f);
    };
    float hb = 0, ht = 0, lb = 0, lt = 0;
    run(FogSettings::Mode::ExponentialHeight, hb, ht);
    run(FogSettings::Mode::Linear, lb, lt);
    std::printf("       туман у низа/верха колонны: высотный %.3f/%.3f, линейный %.3f/%.3f\n", hb, ht, lb, lt);
    Check(hb > ht + 0.15f, "высотный туман плотнее у земли, чем наверху");
    Check(std::abs(lb - lt) < 0.08f, "линейный туман от высоты не зависит (проверка не пустая)");
}

void TestPostSelfCheck(FrameRenderer& r) {
    const sage::render::PostFX::SelfCheck& check = r.Fx.CheckPipeline();
    if (check.Ran && check.Ok) {
        CountPass();
        std::printf("[ ok ] %-28s цепочка эффектов признала себя рабочей\n", "post_self_check");
    } else {
        CountFail();
        std::printf("[FAIL] %-28s самопроверка поста не прошла: %s\n", "post_self_check",
                    check.Reason.empty() ? "(без причины)" : check.Reason.c_str());
    }
}

void RunFrameChecks(FrameRenderer& r, Scene& scene) {
    TestScenePerspective(r, scene);
    TestShadowsOffIsNotBlack(r, scene);
    TestNoSkyNoLightIsBlack(r);
    TestSolidSky(r);
    TestShadedWithPostIsNotBlack(r, scene);
    TestPostSelfCheck(r);
    TestOutputEncodingAndCurves(r);
    TestAutoExposure(r);
    TestAutoExposureIgnoresVoid(r);
    TestHeightFog(r);
    TestSceneOrthographic(r, scene);
    TestNoPostFX(r, scene);
    TestDepthOfField(r, scene);
    TestDepthOfFieldNearSpreads(r);
    TestEveryEffectToggles(r, scene);
    TestFxaa(r, scene);
    TestTransparentFaceOrder(r, scene);
    TestEmissive(r, scene);
    TestGrid(r, scene);
    TestGridKeepsGizmoLinesOnItsPlane(r);
    TestSolidGizmoShading(r);
    TestDecals(r);
    TestObjectMotionBlur(r);
    TestCameraMotionBlurOverSky(r);
    TestMsaa(r);
}

} // namespace sage::rendertest
