// ---------------------------------------------------------------------------
// Устойчивость рендера: то, что ломается НЕ В ПЕРВОМ КАДРЕ.
//
// ЗАЧЕМ ОТДЕЛЬНЫЙ НАБОР. Все остальные проверки снимают ОДИН кадр и сравнивают
// его с эталоном. Такой набор по построению не видит целого класса поломок —
// тех, что копятся: кадр номер один правильный, кадр номер тысяча тоже
// «правильный», а между ними картинка успела уехать, видеопамять — утечь, а
// после третьего запуска Play объекты — пропасть. Именно так выглядит жалоба
// «сначала всё работает, а потом рендер без причины деградирует»: одиночным
// кадром её не воспроизвести, потому что дефект не в кадре, а в НАКОПЛЕНИИ.
//
// Здесь проверяется ровно это накопление, и проверяется числами, а не глазом:
//
//   • дрейф — кадр после сотен повторов обязан совпадать с первым;
//   • утечки GPU-объектов — после циклов «создали проход, порисовали, снесли»
//     живых объектов столько же, сколько было (см. sage/rhi/ResourceLedger.h);
//   • замена сцены (Play → Stop, откат правки) не оставляет рендеру состояния
//     от прошлой сцены;
//   • разрешения и смена размера окна — кадр не разъезжается и не портится;
//   • многократный проход теней/отражений не сдвигает картинку.
//
// Тесты этого файла НЕ ХРАНЯТ эталонов: они сравнивают кадр С САМИМ СОБОЙ,
// снятым иначе (раньше, в другом порядке, после другого числа повторов).
// Поэтому они одинаково значимы на любой видеокарте — разница драйверов
// сокращается, ведь обе стороны сравнения сняты на одной и той же машине.
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderBatch.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/GridRenderer.h"
#include "sage/render/Particle.h"
#include "sage/render/ParticleSystem.h"
#include "sage/render/PostFX.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/ShadowMap.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/rhi/ResourceLedger.h"
#include "sage/ui/UIRenderer.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace sage::rendertest {
namespace {

using sage::rhi::ResourceCounts;
using sage::rhi::ResourceLedger;

// Максимум расхождения двух кадров ОДНОЙ машины по одному каналу. Не ноль:
// кадр включает пост-обработку, а порядок операций с плавающей точкой может
// отличаться между запусками шейдера (разные блоки растеризации). Но два
// одинаково снятых кадра обязаны совпадать практически побитно — единица из
// 255 это «последний бит», а не «похоже».
constexpr int kSelfDiffTolerance = 1;

struct Diff {
    int Max = 0;
    double Mean = 0.0;
    double Fraction = 0.0; // доля пикселей, отличающихся больше допуска
};

Diff CompareImages(const Image& a, const Image& b) {
    Diff d;
    if (a.Pixels.size() != b.Pixels.size() || a.Pixels.empty()) {
        d.Max = 255;
        d.Mean = 255.0;
        d.Fraction = 1.0;
        return d;
    }
    long long sum = 0;
    long long worse = 0;
    for (size_t i = 0; i < a.Pixels.size(); ++i) {
        const int diff = std::abs((int)a.Pixels[i] - (int)b.Pixels[i]);
        sum += diff;
        d.Max = std::max(d.Max, diff);
        if (diff > kSelfDiffTolerance) ++worse;
    }
    d.Mean = (double)sum / (double)a.Pixels.size();
    d.Fraction = (double)worse / (double)a.Pixels.size();
    return d;
}

double MeanLuma(const Image& img) {
    if (img.Pixels.empty()) return 0.0;
    long long sum = 0;
    for (unsigned char v : img.Pixels) sum += v;
    return (double)sum / (double)img.Pixels.size();
}

// --- 1. Дрейф кадра --------------------------------------------------------
//
// Сцена неподвижна, настройки неизменны — значит, и кадр обязан быть
// неизменным, сколько бы раз его ни рисовали. Всё, что копится между кадрами
// (история пост-обработки, ответы о перекрытии, кэш мировых матриц, состояние
// GL, оставленное проходом), проявляется здесь как расхождение с первым кадром.
//
// Двести кадров, а не пять: часть накоплений имеет период (перепроверка
// закрытых объектов раз в четыре кадра, ping-pong буферов), и короткий прогон
// прошёл бы мимо фазы, в которой ошибка видна.
void TestNoFrameDrift(FrameRenderer& r, Scene& scene) {
    const glm::mat4 proj = PerspectiveProj();
    const sage::render::PostFXSettings fx = BaseSettings();

    const Image first = RenderFrame(r, scene, proj, fx, kW, kH);
    Image worstFrame;
    Diff worst;
    int worstIndex = 0;

    constexpr int kFrames = 200;
    for (int i = 1; i <= kFrames; ++i) {
        const Image current = RenderFrame(r, scene, proj, fx, kW, kH);
        const Diff d = CompareImages(first, current);
        if (d.Max > worst.Max) {
            worst = d;
            worstFrame = current;
            worstIndex = i;
        }
    }

    if (worst.Max <= kSelfDiffTolerance) {
        CountPass();
        std::printf("[ ok ] %-28s %d кадров подряд совпали с первым (худший канал %d)\n",
                    "soak_no_drift", kFrames, worst.Max);
    } else {
        CountFail();
        std::printf("[FAIL] %-28s кадр %d разошёлся с первым: худший канал %d, среднее %.3f, "
                    "отличается %.2f%% пикселей — картинка КОПИТ изменения\n",
                    "soak_no_drift", worstIndex, worst.Max, worst.Mean, worst.Fraction * 100.0);
        std::printf("       яркость: первый кадр %.2f, худший %.2f\n", MeanLuma(first),
                    MeanLuma(worstFrame));
    }
}

// --- 2. Утечка GPU-объектов на повторных проходах ---------------------------
//
// Полный проход кадра создаёт буферы, шейдеры и запросы. Часть из них живёт
// столько же, сколько программа (встроенные шейдеры — намеренно), но число
// живых объектов обязано ВЫЙТИ НА ПОЛКУ: после первых кадров новые появляться
// не должны. Растущий счёт — это и есть та самая деградация, которую человек
// видит как «через полчаса всё замирает».
void TestNoResourceGrowth(FrameRenderer& r, Scene& scene) {
    const glm::mat4 proj = PerspectiveProj();
    const sage::render::PostFXSettings fx = BaseSettings();

    // Прогрев: первые кадры создают ленивые шейдеры и буферы — это не утечка.
    for (int i = 0; i < 5; ++i) RenderFrame(r, scene, proj, fx, kW, kH);

    const ResourceCounts before = ResourceLedger::Snapshot();
    for (int i = 0; i < 30; ++i) RenderFrame(r, scene, proj, fx, kW, kH);
    const ResourceCounts after = ResourceLedger::Snapshot();

    const std::string diff = after.DiffFrom(before);
    if (diff.empty()) {
        CountPass();
        std::printf("[ ok ] %-28s 30 кадров не добавили ни одного GPU-объекта (%s)\n",
                    "soak_no_gpu_growth", after.ToString().c_str());
    } else {
        CountFail();
        std::printf("[FAIL] %-28s за 30 кадров прибавилось: %s — рендер ТЕЧЁТ покадрово\n",
                    "soak_no_gpu_growth", diff.c_str());
    }
}

// --- 3. Play → Stop → Play: цикл жизни ресурсов -----------------------------
//
// Редактор на каждый Stop ЗАМЕНЯЕТ сцену восстановленной из снапшота, а вместе
// с ней — все её меши и текстуры. Если хоть один GPU-объект переживает свой
// цикл, десяток запусков подряд оставляет за собой десяток невидимых копий
// сцены в видеопамяти. Симптом на стороне человека — «после нескольких
// запусков редактор начинает тормозить и портить картинку».
//
// Здесь этот цикл воспроизводится напрямую: сцена и проход создаются и
// уничтожаются целиком, между циклами кадр рисуется по-настоящему.
void TestPlayStopCyclesDoNotLeak() {
    const glm::mat4 proj = PerspectiveProj();
    const sage::render::PostFXSettings fx = BaseSettings();

    // Один прогрев вне замера: ленивые статические шейдеры создаются один раз
    // на процесс и в цикле уже не появятся.
    {
        std::unique_ptr<Scene> warm = MakeScene();
        FrameRenderer renderer;
        RenderFrame(renderer, *warm, proj, fx, kW, kH);
    }

    const ResourceCounts before = ResourceLedger::Snapshot();
    constexpr int kCycles = 8;
    for (int i = 0; i < kCycles; ++i) {
        std::unique_ptr<Scene> scene = MakeScene();
        FrameRenderer renderer;
        RenderFrame(renderer, *scene, proj, fx, kW, kH);
        RenderFrame(renderer, *scene, proj, fx, kW, kH);
    }
    const ResourceCounts after = ResourceLedger::Snapshot();

    const std::string diff = after.DiffFrom(before);
    if (diff.empty()) {
        CountPass();
        std::printf("[ ok ] %-28s %d циклов «создали проход — порисовали — снесли» "
                    "без остатка\n", "play_stop_no_leak", kCycles);
    } else {
        CountFail();
        std::printf("[FAIL] %-28s после %d циклов осталось: %s — GPU-объекты переживают "
                    "свой цикл\n", "play_stop_no_leak", kCycles, diff.c_str());
    }
}

// --- 4. Замена сцены не тянет за собой состояние прошлой --------------------
//
// Между сценами у рендера остаётся состояние, привязанное к СУЩНОСТЯМ: ответы
// проверки перекрытия, мировые матрицы прошлого кадра. Новая сцена получает те
// же номера сущностей (entt переиспользует их с нуля), и старые ответы
// применяются к другим объектам: «этот закрыт стеной» — про стену, которой уже
// нет, и объект не рисуется.
//
// Проверка: кадр сцены Б после сцены А обязан совпасть с кадром сцены Б,
// снятым на чистом проходе.
void TestSceneSwapKeepsNoState() {
    const glm::mat4 proj = PerspectiveProj();
    const sage::render::PostFXSettings fx = BaseSettings();

    // Эталон: чистый проход, ничего до него не рисовал.
    Image clean;
    {
        std::unique_ptr<Scene> scene = MakeScene();
        FrameRenderer renderer;
        renderer.Batch.SetOcclusionCulling(true, 0);
        for (int i = 0; i < 6; ++i) clean = RenderFrame(renderer, *scene, proj, fx, kW, kH);
    }

    // Тот же кадр, но проходу перед этим показали ДРУГУЮ сцену — плотно
    // заставленную, чтобы проверка перекрытия успела объявить часть объектов
    // закрытыми, а история матриц заполнилась.
    Image afterSwap;
    {
        FrameRenderer renderer;
        renderer.Batch.SetOcclusionCulling(true, 0);
        {
            std::unique_ptr<Scene> crowd = MakeScene();
            // Стена вплотную к камере: за ней всё остальное заведомо закрыто.
            GameObject wall = crowd->CreateObject("Wall");
            wall.GetTransform().Position = {2.0f, 2.0f, 3.0f};
            wall.GetTransform().Scale = {6.0f, 6.0f, 0.2f};
            wall.Renderer().Ref = MeshRef{MeshRef::Type::Cube};
            wall.Renderer().MeshPtr =
                ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
            for (int i = 0; i < 8; ++i) RenderFrame(renderer, *crowd, proj, fx, kW, kH);
        }
        std::unique_ptr<Scene> scene = MakeScene();
        for (int i = 0; i < 6; ++i) afterSwap = RenderFrame(renderer, *scene, proj, fx, kW, kH);
    }

    const Diff d = CompareImages(clean, afterSwap);
    if (d.Max <= kSelfDiffTolerance) {
        CountPass();
        std::printf("[ ok ] %-28s кадр новой сцены не зависит от предыдущей\n",
                    "scene_swap_no_state");
    } else {
        CountFail();
        std::printf("[FAIL] %-28s кадр новой сцены отличается от чистого: худший канал %d, "
                    "отличается %.2f%% — рендер тащит состояние прошлой сцены\n",
                    "scene_swap_no_state", d.Max, d.Fraction * 100.0);
        std::printf("       яркость: чистый %.2f, после замены %.2f\n", MeanLuma(clean),
                    MeanLuma(afterSwap));
    }
}

// --- 5. Разрешения ----------------------------------------------------------
//
// Кадр обязан рисоваться в любом разрешении, включая нечётные и вырожденно
// маленькие, и обязан оставаться ТЕМ ЖЕ кадром: масштаб меняется, содержимое —
// нет. Проверяем по средней яркости и по положению центра масс освещённых
// пикселей — если бы кадр растянуло или сдвинуло вьюпортом, центр уехал бы.
void TestResolutions(FrameRenderer& r, Scene& scene) {
    struct Case { int W, H; const char* Name; };
    // 16:9 от 720p до 4K плюс нечётные размеры: панель редактора почти никогда
    // не имеет «круглого» размера, и именно на нечётных вылезают ошибки
    // округления половинных буферов (bloom считается в w/2 x h/2).
    const Case cases[] = {
        {1280, 720, "1280x720"},   {1920, 1080, "1920x1080"}, {2560, 1440, "2560x1440"},
        {3840, 2160, "3840x2160"}, {641, 361, "641x361"},     {17, 9, "17x9"},
        {1, 1, "1x1"},
    };

    // Опорные величины снимаем на 16:9 — с ними и сравниваем остальные 16:9.
    double refLuma = 0.0;
    double refCx = 0.0, refCy = 0.0;
    bool haveRef = false;
    bool ok = true;
    std::string worstNote;

    for (const Case& c : cases) {
        const float aspect = (float)c.W / (float)std::max(c.H, 1);
        CameraComponent cam;
        cam.Fov = 50.0f;
        cam.NearClip = 0.1f;
        cam.FarClip = 100.0f;
        const Image img = RenderFrame(r, scene, cam.ProjectionMatrix(aspect), BaseSettings(),
                                      c.W, c.H);
        if (img.Width != c.W || img.Height != c.H || img.Pixels.empty()) {
            ok = false;
            worstNote = std::string(c.Name) + ": кадр не снялся";
            continue;
        }

        // Чёрный кадр — тоже отказ: «не упало» не значит «нарисовалось».
        const double luma = MeanLuma(img);
        if (luma < 1.0) {
            ok = false;
            worstNote = std::string(c.Name) + ": кадр чёрный";
            continue;
        }

        // Центр масс яркости в ДОЛЯХ кадра: он не зависит от разрешения, если
        // картинка та же самая.
        double sum = 0.0, cx = 0.0, cy = 0.0;
        for (int y = 0; y < img.Height; ++y) {
            for (int x = 0; x < img.Width; ++x) {
                const size_t i = ((size_t)y * img.Width + x) * 3;
                const double v = img.Pixels[i] + img.Pixels[i + 1] + img.Pixels[i + 2];
                sum += v;
                cx += v * ((double)x + 0.5) / img.Width;
                cy += v * ((double)y + 0.5) / img.Height;
            }
        }
        if (sum > 0.0) { cx /= sum; cy /= sum; }

        // Сравниваем только 16:9 между собой: у 17x9 и 1x1 другая пропорция, и
        // требовать от них того же центра было бы требованием к арифметике, а
        // не к рендеру. Для них проверка — «снялось и не чёрное».
        const bool wide = std::abs(aspect - 16.0f / 9.0f) < 0.02f;
        if (!wide) continue;
        if (!haveRef) {
            refLuma = luma;
            refCx = cx;
            refCy = cy;
            haveRef = true;
            continue;
        }
        // Допуски: яркость меняется от разного числа пикселей на кромках
        // (сглаживание), центр — почти нет.
        if (std::abs(luma - refLuma) > 6.0 || std::abs(cx - refCx) > 0.01 ||
            std::abs(cy - refCy) > 0.01) {
            ok = false;
            char note[256];
            std::snprintf(note, sizeof(note),
                          "%s: яркость %.1f против %.1f, центр (%.3f, %.3f) против (%.3f, %.3f)",
                          c.Name, luma, refLuma, cx, cy, refCx, refCy);
            worstNote = note;
        }
    }

    if (ok) {
        CountPass();
        std::printf("[ ok ] %-28s 720p..4K, нечётные и 1x1 — кадр тот же самый\n",
                    "resolution_sweep");
    } else {
        CountFail();
        std::printf("[FAIL] %-28s %s\n", "resolution_sweep", worstNote.c_str());
    }
}

// --- 6. Смена размера буфера --------------------------------------------------
//
// Перетаскивание границы панели меняет размер буферов десятки раз в секунду.
// Кадр после череды смен обязан совпадать с кадром буфера, созданного этим
// размером сразу: если Resize оставит вложение прежнего размера (частый случай
// с многосэмпловой парой), картинка окажется обрезанной или растянутой — а
// «сразу после перетаскивания» этого не видно, потому что следующий кадр
// перерисует уже правильно.
void TestResizeChurn(FrameRenderer& r, Scene& scene) {
    const int w = 320, h = 240;
    const float aspect = (float)w / (float)h;
    CameraComponent cam;
    cam.Fov = 50.0f;
    cam.NearClip = 0.1f;
    cam.FarClip = 100.0f;
    const glm::mat4 proj = cam.ProjectionMatrix(aspect);
    const sage::render::PostFXSettings fx = BaseSettings();

    const Image fresh = RenderFrame(r, scene, proj, fx, w, h);

    // Череда размеров, включая больший и меньший: и рост, и сжатие хранилища.
    const int sizes[][2] = {{200, 150}, {640, 480}, {321, 241}, {96, 72}, {800, 600}};
    Image afterChurn;
    {
        Framebuffer sceneFbo(w, h);
        Framebuffer output(w, h);
        sage::render::PostFX post;
        const LightingEnvironment env = sage::ecs::CollectLighting(scene);
        const glm::mat4 view = TestView();
        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();

        for (const auto& s : sizes) {
            sceneFbo.Resize(s[0], s[1]);
            output.Resize(s[0], s[1]);
            sceneFbo.Bind();
            device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
            device.Clear(true, true);
            r.Batch.RenderColor(scene, view, proj, kEye, env, ShadowBinding(), 0);
            sceneFbo.Resolve();
            post.Render(sceneFbo.ColorTexture(), sceneFbo.DepthTexture(), s[0], s[1], proj, view,
                        fx, &output, 0, 0, s[0], s[1]);
        }

        // Возвращаемся к исходному размеру и снимаем кадр ЕЩЁ РАЗ.
        sceneFbo.Resize(w, h);
        output.Resize(w, h);
        sceneFbo.Bind();
        device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
        device.Clear(true, true);
        r.Shadow.SetLightMatrix(env.Sun.Direction, glm::vec3(0.0f), 12.0f);
        r.Shadow.BeginRender();
        r.Batch.RenderDepth(scene, r.Shadow.LightMatrix());
        r.Shadow.EndRender(w, h);
        sceneFbo.Bind();
        device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
        device.Clear(true, true);
        r.Batch.RenderColor(scene, view, proj, kEye, env, ShadowBinding(r.Shadow, true), 0);
        sceneFbo.Resolve();
        post.ResetHistory();
        post.Render(sceneFbo.ColorTexture(), sceneFbo.DepthTexture(), w, h, proj, view, fx,
                    &output, 0, 0, w, h);
        output.Bind();
        afterChurn = Capture(w, h);
        device.BindDefaultFramebuffer();
    }

    const Diff d = CompareImages(fresh, afterChurn);
    if (d.Max <= kSelfDiffTolerance) {
        CountPass();
        std::printf("[ ok ] %-28s после пяти смен размера кадр тот же\n", "resize_churn");
    } else {
        CountFail();
        std::printf("[FAIL] %-28s после смен размера кадр изменился: худший канал %d, "
                    "отличается %.2f%%\n", "resize_churn", d.Max, d.Fraction * 100.0);
    }
}

// --- 7. MSAA: сглаживание не портит содержимое кадра ------------------------
//
// Многосэмпловый буфер обязан давать ТУ ЖЕ картинку, только с гладкими
// кромками. Ошибка в разрешении (Resolve) выглядит либо как пустой кадр, либо
// как кадр, съехавший по яркости, — и то и другое ловится сравнением средних:
// разница обязана быть маленькой, а сам кадр — непустым.
void TestMsaaMatchesSingleSample(Scene& scene) {
    const int w = kW, h = kH;
    const glm::mat4 proj = PerspectiveProj();
    const glm::mat4 view = TestView();
    const LightingEnvironment env = sage::ecs::CollectLighting(scene);
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    sage::ecs::RenderBatch batch;

    auto shot = [&](int samples) {
        Framebuffer fbo(w, h, samples);
        fbo.Bind();
        device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
        device.Clear(true, true);
        batch.RenderColor(scene, view, proj, kEye, env, ShadowBinding(), 0);
        fbo.Resolve();
        // Читаем через разрешённую текстуру: именно её видит пост-обработка.
        Framebuffer copy(w, h);
        sage::render::PostFX post;
        sage::render::PostFXSettings fx = BaseSettings();
        fx.BloomEnabled = false;
        fx.AOEnabled = false;
        post.Render(fbo.ColorTexture(), fbo.DepthTexture(), w, h, proj, view, fx, &copy, 0, 0, w,
                    h);
        copy.Bind();
        Image img = Capture(w, h);
        device.BindDefaultFramebuffer();
        return img;
    };

    const Image single = shot(1);
    const Image multi = shot(4);
    const double a = MeanLuma(single), b = MeanLuma(multi);

    if (b > 1.0 && std::abs(a - b) < 4.0) {
        CountPass();
        std::printf("[ ok ] %-28s MSAA даёт ту же картинку (яркость %.2f против %.2f)\n",
                    "msaa_matches_single", a, b);
    } else {
        CountFail();
        std::printf("[FAIL] %-28s MSAA изменил кадр: яркость %.2f против %.2f "
                    "(пустой кадр или неверное разрешение многосэмплового буфера)\n",
                    "msaa_matches_single", a, b);
    }
}



// --- 8. Проход возвращает состояние конвейера -------------------------------
//
// ЧТО ЭТО ЛОВИТ. Смешивание — состояние, которое поднимают под себя частицы,
// билборды и интерфейс. Забытая строка «снять за собой» не ломает ни сборку, ни
// тот кадр, где она забыта: она доживает до цепочки пост-обработки, а та пишет
// в буферы, ЖИВУЩИЕ МЕЖДУ КАДРАМИ. Полноэкранный проход, обязанный заменить
// содержимое, начинает подмешиваться к прошлому кадру — и картинка темнеет с
// каждым кадром без всякой причины со стороны сцены.
//
// Именно это и было: кадр за кадром 105 -> 87 -> 78 -> 75 по средней яркости.
// Поэтому проверок две, и обе нужны: первая — что проход убирает за собой,
// вторая — что цепочка пост-обработки не зависит от того, убрал ли.
void TestPassesRestoreBlendState(FrameRenderer& r, Scene& scene) {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    const glm::mat4 proj = PerspectiveProj();
    const glm::mat4 view = TestView();
    const LightingEnvironment env = sage::ecs::CollectLighting(scene);

    struct PassCase { const char* Name; bool LeftOn; };
    std::vector<PassCase> bad;

    auto check = [&](const char* name, const std::function<void()>& pass) {
        device.SetBlend(false);
        pass();
        if (device.BlendEnabled()) bad.push_back({name, true});
    };

    Framebuffer fbo(kW, kH);
    fbo.Bind();
    device.SetClearColor(0.05f, 0.06f, 0.08f, 1.0f);
    device.Clear(true, true);

    check("сцена", [&] {
        r.Batch.RenderColor(scene, view, proj, kEye, env, ShadowBinding(), 0);
    });

    // Частицы: одна живая частица — этого достаточно, чтобы проход дошёл до
    // включения смешивания (пустая система выходит раньше и ничего не трогает).
    check("частицы", [&] {
        ParticleSystem particles;
        particles.Burst(ParticleEmitterConfig{}, glm::vec3(0.0f, 1.0f, 0.0f), 4);
        particles.Update(0.016f);
        particles.DrawFromView(view, proj);
    });

    check("интерфейс", [&] {
        UIRenderer ui;
        ui.Begin(kW, kH);
        ui.Rect(10.0f, 10.0f, 40.0f, 20.0f, glm::vec4(1.0f, 0.5f, 0.2f, 1.0f));
        ui.End();
    });

    check("сетка", [&] {
        sage::render::GridSettings grid;
        r.Grid.Draw(view, proj, kEye, grid);
    });

    device.BindDefaultFramebuffer();
    device.SetBlend(false);

    if (bad.empty()) {
        CountPass();
        std::printf("[ ok ] %-28s каждый проход снял смешивание за собой\n",
                    "passes_restore_blend");
    } else {
        CountFail();
        std::string names;
        for (const PassCase& c : bad) {
            if (!names.empty()) names += ", ";
            names += c.Name;
        }
        std::printf("[FAIL] %-28s смешивание осталось включённым после: %s — состояние "
                    "утечёт в пост-обработку и кадр начнёт темнеть\n",
                    "passes_restore_blend", names.c_str());
    }
}

// --- 9. Пост-обработка не зависит от чужого состояния -----------------------
//
// Вторая половина той же защиты. Даже если какой-то проход (сегодняшний или
// завтрашний) оставит смешивание включённым, цепочка обязана дать ТОТ ЖЕ кадр:
// она выставляет нужное ей состояние сама.
void TestPostFxIgnoresInheritedState(FrameRenderer& r, Scene& scene) {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    const glm::mat4 proj = PerspectiveProj();
    const sage::render::PostFXSettings fx = BaseSettings();

    device.SetBlend(false);
    const Image clean = RenderFrame(r, scene, proj, fx, kW, kH);

    // Нарочно оставляем состояние «грязным» и рисуем несколько кадров подряд:
    // накопление в буферах затенения и свечения проявляется не в первом.
    Image dirty;
    for (int i = 0; i < 4; ++i) {
        device.SetBlend(true);
        dirty = RenderFrame(r, scene, proj, fx, kW, kH);
    }
    device.SetBlend(false);

    const Diff d = CompareImages(clean, dirty);
    if (d.Max <= kSelfDiffTolerance) {
        CountPass();
        std::printf("[ ok ] %-28s включённое до неё смешивание кадр не меняет\n",
                    "postfx_ignores_state");
    } else {
        CountFail();
        std::printf("[FAIL] %-28s кадр зависит от чужого состояния: худший канал %d, "
                    "яркость %.2f против %.2f — цепочка подмешивает вместо замены\n",
                    "postfx_ignores_state", d.Max, MeanLuma(clean), MeanLuma(dirty));
    }
}

} // namespace

void RunStabilityChecks(FrameRenderer& r, Scene& scene) {
    std::printf("\n--- Устойчивость: дрейф, утечки, разрешения ---\n");
    TestNoFrameDrift(r, scene);
    TestNoResourceGrowth(r, scene);
    TestPlayStopCyclesDoNotLeak();
    TestSceneSwapKeepsNoState();
    TestResolutions(r, scene);
    TestResizeChurn(r, scene);
    TestMsaaMatchesSingleSample(scene);
    TestPassesRestoreBlendState(r, scene);
    TestPostFxIgnoresInheritedState(r, scene);
}

} // namespace sage::rendertest
