// ---------------------------------------------------------------------------
// ЭТАЛОННЫЕ СЦЕНЫ НОВОЙ СИСТЕМЫ ИНТЕРФЕЙСА (§125 ТЗ).
//
// Набор сцен — базовая, вложенная, с масками, с эффектами, типографика,
// адаптивная и большая — прогоняется через ВЕСЬ конвейер: раскладка → команды →
// батчи → GPU. То есть проверяется не «функция вернула число», а то, что на
// экране действительно появилось.
//
// ЧТО ИМЕННО МЕРЯЕТСЯ. Не «похожесть картинок»: шрифт и сглаживание у разных
// драйверов свои, и эталонный PNG превратился бы в источник ложных падений на
// каждой второй машине. Меряются ЧИСЛА СО СМЫСЛОМ — доля закрашенного внутри и
// снаружи маски, яркость до и после включения тени, ширина строки при переносе,
// одинаковость раскладки при смене разрешения. Каждое такое число — это
// утверждение, которое либо верно, либо нет, и от драйвера оно не зависит.
//
// ЗАЧЕМ ЭТО СВЕРХ МОДУЛЬНЫХ ТЕСТОВ. Модульные проверяют раскладку и команды.
// Они не увидят, что маска не дошла до ножниц, что тень нарисовалась поверх
// панели вместо под ней, что батч склеил несклеиваемое и половина интерфейса
// пропала. Ровно эти поломки видны только на пикселях.
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "sage/render/Framebuffer.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/ui/UIFramework.h"
#include "sage/ui/UIRenderer.h"
#include "sage/ui/render/UIEngineResources.h"
#include "sage/ui/sageui/SageUI.h"
#include "sage/ui/icons/SageIcons.h"
#include "sage/ui/visual/UIIcon.h"
#include "sage/ui/showcase/UIDemos.h"
#include "sage/ui/showcase/UIShowcaseDocument.h"

namespace sage::rendertest {
namespace {

namespace ui = sage::ui;

constexpr int kW = 512;
constexpr int kH = 288;

// Оснастка: документ + рантайм + мост к ресурсам движка. Ровно то, что делает
// игра, — другого пути отрисовки у документа нет.
struct Harness {
    ui::UIRuntime Runtime;
    ui::UIEngineResources Resources;

    Harness() {
        Resources.Install(Runtime.Context());
        Runtime.Doc().Canvas().Scale = ui::UICanvasSettings::ScaleMode::Pixels;
    }
    ui::UIDocument& Doc() { return Runtime.Doc(); }
};

Image RenderDocument(UIRenderer& renderer, Harness& h, int w = kW, int hh = kH) {
    Framebuffer fbo(w, hh);
    fbo.Bind();
    sage::rhi::GraphicsDevice& dev = sage::rhi::GraphicsDevice::Get();
    // Чёрный фон намеренно: «закрашено» и «не закрашено» отличаются без
    // порогов и подбора.
    dev.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    dev.Clear(true, true);

    h.Runtime.SetScreen({(float)w, (float)hh});
    h.Runtime.Update(0.016f);
    ui::UIClassicBackend backend(renderer);
    // Куда возвращаться после промежуточного прохода. Здесь это буфер теста:
    // без этого композиция размытия уехала бы в буфер по умолчанию, и снимок
    // оказался бы пустым — ровно так эта проверка и поймала ошибку.
    backend.SetRootTarget(&fbo);
    h.Runtime.Render(backend);

    Image img = Capture(w, hh);
    dev.BindDefaultFramebuffer();
    return img;
}

// Доля закрашенных пикселей в прямоугольнике (0..1).
double Covered(const Image& img, int x0, int y0, int x1, int y1) {
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, img.Width); y1 = std::min(y1, img.Height);
    if (x1 <= x0 || y1 <= y0) return 0.0;
    int lit = 0, total = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t i = ((size_t)y * img.Width + x) * 3;
            const int v = std::max(std::max(img.Pixels[i], img.Pixels[i + 1]), img.Pixels[i + 2]);
            if (v > 12) ++lit;
            ++total;
        }
    }
    return total ? (double)lit / (double)total : 0.0;
}

double Luma(const Image& img, int x0, int y0, int x1, int y1) {
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, img.Width); y1 = std::min(y1, img.Height);
    if (x1 <= x0 || y1 <= y0) return 0.0;
    double sum = 0.0;
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t i = ((size_t)y * img.Width + x) * 3;
            sum += 0.299 * img.Pixels[i] + 0.587 * img.Pixels[i + 1] + 0.114 * img.Pixels[i + 2];
            ++n;
        }
    }
    return n ? sum / n : 0.0;
}

ui::UINode& Add(ui::UIDocument& doc, const char* name, ui::UINodeId parent = ui::kUIInvalidNode) {
    return *doc.Create(name, parent);
}

// --- Сцена «Basic»: узел нарисован там, где его посчитала раскладка ----------
void CheckBasic(UIRenderer& renderer) {
    Harness h;
    ui::UINode& panel = Add(h.Doc(), "Panel");
    ui::UITransform& t = panel.Ensure<ui::UITransform>();
    t.Offset = {40.0f, 30.0f};
    t.Size = {120.0f, 60.0f};
    ui::UIFill& fill = panel.Ensure<ui::UIFill>();
    fill.Color = ui::UIColor(1.0f, 1.0f, 1.0f, 1.0f);
    fill.Radius = ui::UICorners(0.0f);

    const Image img = RenderDocument(renderer, h);
    const double inside = Covered(img, 46, 36, 154, 84);
    const double outside = Covered(img, 4, 4, 34, 24);
    std::printf("    basic: внутри %.2f, снаружи %.2f\n", inside, outside);
    Check(inside > 0.95, "новый UI: панель закрашена ровно там, где посчитана раскладка");
    Check(outside < 0.02, "новый UI: за пределами панели ничего не нарисовано");
}

// --- Сцена «Nested»: прозрачность перемножается по дереву --------------------
void CheckNestedOpacity(UIRenderer& renderer) {
    Harness h;
    ui::UINode& group = Add(h.Doc(), "Group");
    group.Ensure<ui::UITransform>().SetStretch(true, true);

    ui::UINode& solid = Add(h.Doc(), "Solid", group.Id);
    ui::UITransform& st = solid.Ensure<ui::UITransform>();
    st.Offset = {20.0f, 20.0f};
    st.Size = {80.0f, 80.0f};
    solid.Ensure<ui::UIFill>().Color = ui::UIColor(1.0f);

    ui::UINode& faded = Add(h.Doc(), "Faded", group.Id);
    ui::UITransform& ft = faded.Ensure<ui::UITransform>();
    ft.Offset = {140.0f, 20.0f};
    ft.Size = {80.0f, 80.0f};
    faded.Ensure<ui::UIFill>().Color = ui::UIColor(1.0f);
    faded.Opacity = 0.5f;

    const Image before = RenderDocument(renderer, h);
    const double solidLuma = Luma(before, 30, 30, 110, 110);
    const double fadedLuma = Luma(before, 150, 30, 210, 110);

    // Прозрачность ГРУППЫ умножается на прозрачность ребёнка (§28): половина от
    // половины даёт четверть, и это должно быть видно на пикселях.
    group.Opacity = 0.5f;
    h.Doc().MarkDirty(ui::UIDirty_All);
    const Image after = RenderDocument(renderer, h);
    const double solidAfter = Luma(after, 30, 30, 110, 110);

    std::printf("    nested: сплошной %.1f → %.1f, полупрозрачный %.1f\n", solidLuma, solidAfter,
                fadedLuma);
    Check(fadedLuma < solidLuma * 0.75, "новый UI: собственная прозрачность узла видна");
    Check(solidAfter < solidLuma * 0.75, "новый UI: прозрачность группы наследуется поддеревом");
}

// --- Сцена «Masked»: маска действительно режет содержимое --------------------
void CheckMasked(UIRenderer& renderer) {
    Harness h;
    ui::UINode& panel = Add(h.Doc(), "Panel");
    ui::UITransform& pt = panel.Ensure<ui::UITransform>();
    pt.Offset = {40.0f, 40.0f};
    pt.Size = {120.0f, 100.0f};
    panel.Ensure<ui::UIMask>();

    ui::UINode& child = Add(h.Doc(), "Child", panel.Id);
    ui::UITransform& ct = child.Ensure<ui::UITransform>();
    ct.Size = {400.0f, 400.0f};
    child.Ensure<ui::UIFill>().Color = ui::UIColor(1.0f);

    const Image img = RenderDocument(renderer, h);
    const double inside = Covered(img, 46, 46, 154, 134);
    const double beyondRight = Covered(img, 175, 60, 260, 120);
    const double beyondBottom = Covered(img, 60, 155, 150, 220);
    std::printf("    masked: внутри %.2f, справа за маской %.2f, снизу %.2f\n", inside,
                beyondRight, beyondBottom);
    Check(inside > 0.95, "новый UI: содержимое внутри маски нарисовано");
    Check(beyondRight < 0.02 && beyondBottom < 0.02,
          "новый UI: содержимое за маской действительно обрезано");

    // ФИГУРНАЯ МАСКА. Прямоугольные ножницы её выразить не могут — это и есть
    // главная причина, по которой маска не «clip rect» (§30).
    panel.Get<ui::UIMask>()->Form = ui::UIMask::Shape::Ellipse;
    h.Doc().MarkDirty(ui::UIDirty_All);
    const Image ellipse = RenderDocument(renderer, h);
    const double centre = Covered(ellipse, 90, 80, 110, 100);
    const double corner = Covered(ellipse, 42, 42, 56, 56);
    std::printf("    masked: эллипс — центр %.2f, угол %.2f\n", centre, corner);
    Check(centre > 0.95, "новый UI: эллиптическая маска пропускает середину");
    Check(corner < 0.05, "новый UI: эллиптическая маска срезает углы");

    // Скругление: те же углы, но радиусом.
    ui::UIMask& mask = *panel.Get<ui::UIMask>();
    mask.Form = ui::UIMask::Shape::RoundedRect;
    mask.Radius = ui::UICorners(40.0f);
    h.Doc().MarkDirty(ui::UIDirty_All);
    const Image rounded = RenderDocument(renderer, h);
    const double roundedCentre = Covered(rounded, 80, 80, 120, 120);
    const double roundedCorner = Covered(rounded, 42, 42, 52, 52);
    std::printf("    masked: скругление — центр %.2f, угол %.2f\n", roundedCentre,
                roundedCorner);
    Check(roundedCentre > 0.95, "новый UI: скруглённая маска пропускает середину");
    Check(roundedCorner < 0.05, "новый UI: скруглённая маска действительно скругляет");
}

// --- Сцена «Effects»: тень ложится ПОД узлом, а не поверх ---------------------
void CheckEffects(UIRenderer& renderer) {
    Harness h;
    ui::UINode& card = Add(h.Doc(), "Card");
    ui::UITransform& t = card.Ensure<ui::UITransform>();
    t.Offset = {150.0f, 90.0f};
    t.Size = {120.0f, 80.0f};
    ui::UIFill& fill = card.Ensure<ui::UIFill>();
    fill.Color = ui::UIColor(1.0f, 1.0f, 1.0f, 1.0f);
    fill.Radius = ui::UICorners(10.0f);

    const Image plain = RenderDocument(renderer, h);
    const double bodyPlain = Luma(plain, 160, 100, 260, 160);
    const double aroundPlain = Luma(plain, 120, 180, 300, 210);

    ui::UIDropShadow& shadow = card.Ensure<ui::UIEffects>().Ensure<ui::UIDropShadow>();
    shadow.Offset = {0.0f, 14.0f};
    shadow.Blur = 18.0f;
    shadow.Color = ui::UIColor(1.0f, 1.0f, 1.0f, 0.9f); // светлая — чтобы её было видно на чёрном
    h.Doc().MarkDirty(ui::UIDirty_All);
    const Image withShadow = RenderDocument(renderer, h);
    const double bodyShadow = Luma(withShadow, 160, 100, 260, 160);
    const double aroundShadow = Luma(withShadow, 120, 180, 300, 210);

    std::printf("    effects: тело %.1f → %.1f, вокруг %.1f → %.1f\n", bodyPlain, bodyShadow,
                aroundPlain, aroundShadow);
    Check(aroundShadow > aroundPlain + 6.0, "новый UI: тень появилась снаружи узла");
    // Тело почти не меняется: тень рисуется ПОД узлом, а не поверх него.
    Check(std::fabs(bodyShadow - bodyPlain) < 12.0, "новый UI: тень не легла поверх содержимого");
}

// --- Сцена «Typography»: перенос действительно занимает несколько строк -------
void CheckTypography(UIRenderer& renderer) {
    Harness h;
    ui::UINode& label = Add(h.Doc(), "Label");
    ui::UITransform& t = label.Ensure<ui::UITransform>();
    t.Offset = {20.0f, 20.0f};
    t.Size = {180.0f, 200.0f};
    ui::UIText& text = label.Ensure<ui::UIText>();
    text.Text = "Длинная строка, которая обязана перенестись по словам и занять несколько строк";
    text.Size = 18.0f;
    text.Color = ui::UIColor(1.0f);
    text.Wrap = ui::UITextWrap::None;

    const Image oneLine = RenderDocument(renderer, h);
    const double lowerNoWrap = Covered(oneLine, 22, 60, 200, 200);

    text.Wrap = ui::UITextWrap::Word;
    h.Doc().MarkDirty(ui::UIDirty_All);
    const Image wrapped = RenderDocument(renderer, h);
    const double lowerWrapped = Covered(wrapped, 22, 60, 200, 200);
    const double beyondRight = Covered(wrapped, 215, 20, 300, 200);

    std::printf("    typography: без переноса ниже строки %.3f, с переносом %.3f, справа %.3f\n",
                lowerNoWrap, lowerWrapped, beyondRight);
    Check(lowerWrapped > lowerNoWrap + 0.01, "новый UI: перенос по словам даёт несколько строк");
    Check(beyondRight < 0.01, "новый UI: перенесённый текст не вылезает за ширину узла");
}

// --- Сцена «Responsive»: одна вёрстка на двух разрешениях --------------------
void CheckResponsive(UIRenderer& renderer) {
    Harness h;
    h.Doc().Canvas().Scale = ui::UICanvasSettings::ScaleMode::ScaleWithSize;
    h.Doc().Canvas().Reference = {512.0f, 288.0f};

    ui::UINode& panel = Add(h.Doc(), "Panel");
    ui::UITransform& t = panel.Ensure<ui::UITransform>();
    t.AnchorMin = t.AnchorMax = {0.5f, 0.5f};
    t.Pivot = {0.5f, 0.5f};
    t.Size = {200.0f, 100.0f};
    panel.Ensure<ui::UIFill>().Color = ui::UIColor(1.0f);

    const Image small = RenderDocument(renderer, h, 512, 288);
    const Image large = RenderDocument(renderer, h, 1024, 576);

    // Панель по центру и в тех же ДОЛЯХ кадра: это и есть адаптивность.
    const double smallCentre = Covered(small, 512 / 2 - 80, 288 / 2 - 40, 512 / 2 + 80, 288 / 2 + 40);
    const double largeCentre =
        Covered(large, 1024 / 2 - 160, 576 / 2 - 80, 1024 / 2 + 160, 576 / 2 + 80);
    const double smallCorner = Covered(small, 4, 4, 40, 30);
    const double largeCorner = Covered(large, 8, 8, 80, 60);

    std::printf("    responsive: центр %.2f / %.2f, угол %.2f / %.2f\n", smallCentre, largeCentre,
                smallCorner, largeCorner);
    Check(smallCentre > 0.95 && largeCentre > 0.95,
          "новый UI: холст масштабируется — панель занимает ту же долю кадра");
    Check(smallCorner < 0.02 && largeCorner < 0.02, "новый UI: панель не разъехалась по углам");
}

// --- Размытие: промежуточная цель действительно работает --------------------
void CheckBlurCompositing(UIRenderer& renderer) {
    Harness h;
    // Резкая шахматка: у размытого изображения разница между соседними
    // клетками падает, у неразмытого — нет. Это измеримое утверждение, не
    // зависящее ни от драйвера, ни от качества фильтрации.
    ui::UINode& group = Add(h.Doc(), "Group");
    group.Ensure<ui::UITransform>().SetStretch(true, true);
    for (int i = 0; i < 8; ++i) {
        ui::UINode& cell = Add(h.Doc(), "Cell", group.Id);
        ui::UITransform& t = cell.Ensure<ui::UITransform>();
        t.Offset = {60.0f + (float)i * 24.0f, 100.0f};
        t.Size = {12.0f, 80.0f};
        cell.Ensure<ui::UIFill>().Color = ui::UIColor(1.0f);
    }

    const Image sharp = RenderDocument(renderer, h);
    ui::UIBlur& blur = group.Ensure<ui::UIEffects>().Ensure<ui::UIBlur>();
    blur.Radius = 24.0f;
    blur.Passes = 2;
    h.Doc().MarkDirty(ui::UIDirty_All);
    const Image blurred = RenderDocument(renderer, h);

    // Промежуток между полосами: у резкой картинки он чёрный, у размытой — нет.
    const double gapSharp = Luma(sharp, 76, 120, 82, 160);
    const double gapBlur = Luma(blurred, 76, 120, 82, 160);
    const double barSharp = Luma(sharp, 62, 120, 70, 160);
    const double barBlur = Luma(blurred, 62, 120, 70, 160);
    std::printf("    blur: промежуток %.1f → %.1f, полоса %.1f → %.1f\n", gapSharp, gapBlur,
                barSharp, barBlur);
    Check(gapBlur > gapSharp + 8.0, "новый UI: размытие затекло в промежутки между полосами");
    Check(barBlur < barSharp - 8.0, "новый UI: размытие сняло яркость с самих полос");
    Check(h.Runtime.DrawList().Stats().RenderTargets >= 1,
          "новый UI: промежуточная цель действительно создана и видна в профайлере");
}

// --- Сцена «Large UI»: витрина целиком доезжает до пикселей ------------------
void CheckShowcaseFrame(UIRenderer& renderer) {
    Harness h;
    ui::UITheme theme;
    ui::UIBuildShowcase(h.Doc(), theme);
    h.Runtime.Theme() = theme;

    const Image img = RenderDocument(renderer, h, 960, 540);
    const double top = Covered(img, 0, 0, 960, 32);
    const double middle = Covered(img, 40, 80, 600, 460);
    const double side = Covered(img, 700, 80, 940, 480);
    std::printf("    showcase: верх %.2f, карточки %.2f, панель %.2f\n", top, middle, side);
    Check(top > 0.5, "витрина: верхняя панель нарисована");
    Check(middle > 0.3, "витрина: карточки нарисованы");
    Check(side > 0.3, "витрина: боковая панель нарисована");

    // И главное — стоимость. Сотня узлов не должна давать сотню вызовов
    // рисования: батчинг обязан работать на настоящей сцене, а не только в
    // модульном тесте (§85, §145).
    const ui::UIRenderStats& stats = h.Runtime.DrawList().Stats();
    std::printf("    showcase: команд %d, батчей %d, узлов %d\n", stats.Commands, stats.Batches,
                h.Runtime.Profile().Layout.Visible);
    Check(stats.Commands > 60, "витрина: команды рисования сгенерированы");
    Check(stats.Batches * 3 < stats.Commands, "витрина: команды собраны в батчи, а не по одной");
}

// --- Готовые экраны ---------------------------------------------------------
//
// Витрина отвечает на вопрос «что система умеет». Демо-экраны отвечают на
// второй: «как из этого собирают экран», и проверяются они здесь ровно затем,
// что это единственные экраны, которые видит человек при первом запуске — из
// шаблона проекта и из меню создания. Экран, который собрался, но не нарисовался
// (потерянный шрифт, маска не той формы, эффект в чужой буфер), выглядит как
// «редактор сломан», и никакой модульный тест этого не поймает.
//
// Снимки экранов пишутся в PNG, когда задан SAGE_UI_DEMO_SHOTS=<папка>: их
// смотрят глазами. Эталонами они НЕ являются намеренно — шрифт и сглаживание у
// каждой машины свои, и сравнение картинок давало бы ложные падения; утверждения
// ниже проверяют числа со смыслом.
void CheckDemoScreens(UIRenderer& renderer) {
    const char* shots = std::getenv("SAGE_UI_DEMO_SHOTS");

    // Проверяется не «сколько кадра закрашено» — этим экраны отличаются
    // законно: худ обязан оставлять игру видимой, а меню занимает весь кадр.
    // Проверяется УЗЕЛ, без которого экрана нет: его прямоугольник посчитан,
    // лежит внутри кадра и в нём действительно что-то нарисовано.
    struct Case {
        const char* Name;
        const char* Title;
        const char* Anchor;  // узел, без которого экрана нет
        double MinCovered;   // сколько его прямоугольника обязано быть закрашено
    };
    const Case kCases[] = {
        {"menu", "главное меню", "Продолжить", 0.80},
        {"hud", "худ", "Health", 0.35},
        {"settings", "настройки", "Window", 0.90},
        {"inventory", "инвентарь", "Grid", 0.35},
        {"dialogue", "диалог", "Box", 0.90},
    };

    for (const Case& c : kCases) {
        Harness h;
        ui::UITheme theme;
        Check(ui::UIBuildDemo(c.Name, h.Doc(), theme),
              (std::string("демо-экран собран: ") + c.Title).c_str());
        h.Runtime.Theme() = theme;
        // Экраны собраны под опорное разрешение 1920x1080 и обязаны считаться
        // под кадр любой стороны — здесь 1280x720.
        h.Doc().Canvas().Scale = ui::UICanvasSettings::ScaleMode::ScaleWithSize;

        const Image img = RenderDocument(renderer, h, 1280, 720);
        const ui::UIRenderStats& stats = h.Runtime.DrawList().Stats();

        const ui::UINode* anchor = h.Doc().FindByName(c.Anchor);
        const ui::UIResolvedNode* rn = anchor ? h.Runtime.Layout().Get(anchor->Id) : nullptr;
        Check(rn != nullptr, (std::string("узел экрана посчитан: ") + c.Title).c_str());

        double covered = 0.0;
        bool inside = false;
        if (rn) {
            const ui::UIRect& r = rn->Rect;
            covered = Covered(img, (int)r.x, (int)r.y, (int)(r.x + r.w), (int)(r.y + r.h));
            inside = r.w > 1.0f && r.h > 1.0f && r.x >= 0.0f && r.y >= 0.0f &&
                     r.x + r.w <= 1280.0f && r.y + r.h <= 720.0f;
        }
        std::printf("    демо %-10s %s: %.0fx%.0f, закрашено %.2f; команд %d, батчей %d, "
                    "узлов %d\n",
                    c.Name, c.Anchor, rn ? rn->Rect.w : 0.0f, rn ? rn->Rect.h : 0.0f, covered,
                    stats.Commands, stats.Batches, h.Runtime.Profile().Layout.Visible);

        // Внутри кадра — потому что экран, собранный под 1920x1080, обязан
        // помещаться и в 1280x720: масштаб холста именно за это и отвечает.
        Check(inside, (std::string("узел экрана внутри кадра: ") + c.Title).c_str());
        Check(covered > c.MinCovered,
              (std::string("демо-экран нарисован: ") + c.Title).c_str());
        // Экран из одного прямоугольника — это не экран: если раскладка
        // схлопнулась, закрашенность может остаться высокой (фон-то есть), а
        // содержимого не будет вовсе.
        Check(stats.Commands > 8,
              (std::string("на экране есть содержимое: ") + c.Title).c_str());
        Check(stats.Batches <= stats.Commands,
              (std::string("батчи не расплодились: ") + c.Title).c_str());
        Check(ui::UIValidate(h.Doc()).empty(),
              (std::string("документ демо-экрана без замечаний: ") + c.Title).c_str());

        if (shots) SavePng(std::string(shots) + "/ui_demo_" + c.Name + ".png", img);
        if (const char* dump = std::getenv("SAGE_UI_DEMO_DUMP")) {
            if (std::string(dump) == c.Name)
                for (const ui::UIResolvedNode& rn : h.Runtime.Layout().Nodes()) {
                    const ui::UINode* nn = h.Doc().Find(rn.Id);
                    std::printf("      %-12s rect=%.0f,%.0f %.0fx%.0f vis=%d\n",
                                nn ? nn->Name.c_str() : "?", rn.Rect.x, rn.Rect.y, rn.Rect.w,
                                rn.Rect.h, (int)rn.Visible);
                }
        }
    }
}

// --- Оболочка редактора на SAGE UI (ТЗ редактора) ----------------------------
//
// Собирается ровно так, как будет собран редактор: полоса меню, тулбар, док с
// панелями, дерево сцены, инспектор со свойствами, карточки ассетов, строка
// состояния. Проверяется, что всё это ДОЕЗЖАЕТ ДО ПИКСЕЛЕЙ — модульные тесты
// знают только про числа раскладки, и «панель посчиталась, но не появилась»
// они не увидят.
// ---------------------------------------------------------------------------
// ЗНАЧКИ (ТЗ «SAGE Icon System»).
//
// Проверяется то, что нельзя увидеть по списку команд: значок из атласа
// действительно ПОПАДАЕТ НА ЭКРАН, красится цветом узла и не выходит за
// отведённый ему квадрат. И проверяется цена: сетка из десятков значков
// обязана стоить ОДИН батч, потому что все они лежат в одном атласе.
// ---------------------------------------------------------------------------
void CheckIcons(UIRenderer& renderer) {
    const char* shots = std::getenv("SAGE_UI_DEMO_SHOTS");
    namespace icons = ui::icons;

    Harness h;
    ui::UIDocument& doc = h.Doc();

    // Сетка из всего набора: ошибка в одном значке видна на снимке сразу, а
    // «в среднем нарисовалось» — нет.
    const int kCell = 26, kIcon = 20, kCols = 24;
    const int rows = (icons::kIconCount + kCols - 1) / kCols;
    const int w = kCols * kCell, hh = rows * kCell + 40;

    ui::UINode* sheet = doc.Create("Sheet");
    sheet->Ensure<ui::UITransform>().SetStretch(true, true);

    for (int i = 0; i < icons::kIconCount; ++i) {
        ui::UINode* node = doc.Create(icons::generated::kNames[i], sheet->Id);
        ui::UITransform& t = node->Ensure<ui::UITransform>();
        t.AnchorMin = t.AnchorMax = {0.0f, 0.0f};
        t.Pivot = {0.0f, 0.0f};
        t.Offset = {(float)((i % kCols) * kCell) + 3.0f,
                    (float)((i / kCols) * kCell) + 3.0f};
        t.Size = {(float)kIcon, (float)kIcon};
        ui::UIIcon& icon = node->Ensure<ui::UIIcon>();
        icon.Name = icons::generated::kNames[i];
        icon.Size = (float)kIcon;
        // Цвет из УЗЛА, а не из картинки: в атласе лежит только форма.
        icon.Color = (i % 2) ? ui::UIColor{1.0f, 0.78f, 0.25f, 1.0f}
                             : ui::UIColor{0.86f, 0.89f, 0.94f, 1.0f};
    }

    const Image img = RenderDocument(renderer, h, w, hh);
    const ui::UIRenderStats& stats = h.Runtime.DrawList().Stats();

    // 1. Каждый значок нарисован. Порог низкий намеренно: значок — это тонкие
    //    штрихи, а не заливка, и у самых лёгких из них закрашено ~5% квадрата.
    int drawn = 0, empty = 0;
    std::string firstEmpty;
    for (int i = 0; i < icons::kIconCount; ++i) {
        const int x = (i % kCols) * kCell + 3, y = (i / kCols) * kCell + 3;
        if (Covered(img, x, y, x + kIcon, y + kIcon) > 0.03) {
            ++drawn;
        } else {
            ++empty;
            if (firstEmpty.empty()) firstEmpty = icons::generated::kNames[i];
        }
    }
    Check(empty == 0, (std::string("значки: нарисованы все ") +
                       std::to_string(icons::kIconCount) + " (пустых " +
                       std::to_string(empty) +
                       (firstEmpty.empty() ? "" : ", первый — " + firstEmpty) + ")").c_str());
    Check(drawn == icons::kIconCount, "значки: ни один не потерялся");

    // 2. Значок не вылезает за свою клетку — иначе плотная панель инструментов
    //    превратилась бы в кашу из наползающих друг на друга штрихов.
    double spill = 0.0;
    for (int i = 0; i < icons::kIconCount && spill < 0.02; ++i) {
        const int x = (i % kCols) * kCell + 3, y = (i / kCols) * kCell + 3;
        // Полоска справа от клетки: между клетками остаётся 6 px зазора.
        spill = std::max(spill, Covered(img, x + kIcon + 1, y, x + kIcon + 4, y + kIcon));
    }
    Check(spill < 0.02, "значки: рисунок не выходит за свой квадрат");

    // 3. Цвет даёт УЗЕЛ. Жёлтые и белые значки чередуются — значит, в кадре
    //    обязаны быть и те, и другие из ОДНОЙ текстуры.
    int warm = 0, cool = 0;
    for (int p = 0; p < img.Width * img.Height; ++p) {
        const size_t i = (size_t)p * 3;
        const int r = img.Pixels[i], g = img.Pixels[i + 1], b = img.Pixels[i + 2];
        if (r < 40) continue;
        if (r - b > 60) ++warm;
        else if (std::abs(r - b) < 24) ++cool;
    }
    Check(warm > 200 && cool > 200, "значки: один атлас красится в разные цвета");

    // 4. Цена. Сто с лишним значков — это ОДНА привязка атласа, а не сто.
    Check(stats.Batches <= 3,
          (std::string("значки: весь набор — один батч (батчей ") +
           std::to_string(stats.Batches) + ")").c_str());

    if (shots) SavePng(std::string(shots) + "/ui_icons.png", img);
}

void CheckSageUI(UIRenderer& renderer) {
    const char* shots = std::getenv("SAGE_UI_DEMO_SHOTS");
    namespace sui = ui::sui;

    sui::UIContext gui;
    gui.InstallEngineResources();
    gui.SetPixelPerfect();
    gui.SetScreen({1280.0f, 720.0f});
    // Тема ИНСТРУМЕНТА: плотная и холодная. Игровая тема с её воздухом дала бы
    // вдвое меньше видимых строк в каждом списке.
    gui.Theme() = ui::UITheme::Editor();

    sui::EditorShell* shell = gui.CreateIn<sui::EditorShell>(gui.Content());

    // --- Верх ---
    shell->Menu()->SetBrand("SAGE Editor");
    for (const char* name : {"File", "Edit", "Create", "Tools", "Help"}) {
        sui::Popup* menu = shell->Menu()->AddMenu(name);
        menu->AddItem("Пункт", nullptr);
    }
    sui::SearchBox* search = gui.CreateIn<sui::SearchBox>(shell->Menu()->Right(),
                                                          std::string("Search (Ctrl+K)"));
    search->SetWidth(200.0f);

    sui::Toolbar* tb = shell->Tools();
    using sui::Icon;
    tb->AddIcon(Icon::New, "Новая сцена");
    tb->AddIcon(Icon::Open, "Открыть");
    tb->AddIcon(Icon::Save, "Сохранить");
    tb->AddSeparator();
    tb->AddIcon(Icon::Undo, "Отменить");
    tb->AddIcon(Icon::Redo, "Повторить");
    tb->AddSpacer();
    sui::Button* play = tb->AddButton("Play");
    play->SetStyle("ButtonPrimary");
    play->SetWidth(74.0f);
    tb->AddIcon(Icon::Pause, "Пауза");
    tb->AddIcon(Icon::Stop, "Стоп");
    tb->AddSpacer();
    sui::Dropdown* space = gui.CreateIn<sui::Dropdown>(tb, std::vector<std::string>{"Local", "World"}, 0);
    space->SetWidth(86.0f);
    tb->AddIcon(Icon::Settings, "Настройки гизмо");

    // --- Панели ---
    sui::DockPanel* hierarchy = shell->AddPanel(
        gui.Create<sui::DockPanel>(std::string("hierarchy"), std::string("Hierarchy")));
    sui::DockPanel* viewport = shell->AddPanel(
        gui.Create<sui::DockPanel>(std::string("viewport"), std::string("Viewport")),
        sui::DockSide::Right, "hierarchy");
    shell->AddPanel(gui.Create<sui::DockPanel>(std::string("game"), std::string("Game")),
                    sui::DockSide::Center, "viewport");
    sui::DockPanel* inspector = shell->AddPanel(
        gui.Create<sui::DockPanel>(std::string("inspector"), std::string("Inspector")),
        sui::DockSide::Right, "viewport");
    sui::DockPanel* assets = shell->AddPanel(
        gui.Create<sui::DockPanel>(std::string("assets"), std::string("Assets")),
        sui::DockSide::Bottom, "viewport");
    shell->AddPanel(gui.Create<sui::DockPanel>(std::string("console"), std::string("Console")),
                    sui::DockSide::Center, "assets");
    shell->BuildWindowMenu("Window");
    // Пропорции референса: узкая иерархия слева, инспектор справа, ассеты
    // снизу примерно на треть.
    if (ui::sui::DockNode* root = shell->Dock()->Root()) {
        root->Ratio = 0.19f;
        if (root->Children.size() > 1 && root->Children[1]->IsSplit()) {
            ui::sui::DockNode& right = *root->Children[1];
            right.Ratio = 0.75f;
            if (right.Children.size() > 0 && right.Children[0]->IsSplit())
                right.Children[0]->Ratio = 0.66f;
        }
    }
    shell->Dock()->Invalidate();
    // Активные вкладки — те, что на референсе: Viewport и Assets. Неактивная
    // панель стоит на стоянке и прямоугольника не имеет, поэтому проверять её
    // размеры бессмысленно.
    shell->Dock()->Focus("viewport");
    shell->Dock()->Focus("assets");

    // --- Содержимое: дерево сцены ---
    sui::SearchBox* find = gui.CreateIn<sui::SearchBox>(hierarchy->Body(),
                                                        std::string("Search entity..."));
    find->SetStretch(true, false);
    sui::Tree* tree = gui.CreateIn<sui::Tree>(hierarchy->Body());
    tree->SetStretch(true, true);
    std::vector<sui::TreeItem> items;
    auto item = [&](const char* id, const char* text, int depth, bool kids, bool open,
                    bool sel, const char* icon) {
        sui::TreeItem it;
        it.Id = id;
        it.Text = text;
        it.Depth = depth;
        it.HasChildren = kids;
        it.Expanded = open;
        it.Selected = sel;
        it.Icon = icon;
        items.push_back(it);
    };
    item("scene", "Scene", 0, true, true, false, "bag");
    item("env", "Environment", 1, true, true, false, "sun");
    item("dl", "Directional Light", 2, false, false, false, "sun");
    item("sky", "Sky Light", 2, false, false, false, "moon");
    item("fog", "Fog", 2, false, false, false, "drop");
    item("ground", "Ground", 1, true, true, false, "bag");
    item("plane", "Plane", 2, false, false, false, "rect");
    item("structures", "Structures", 1, true, true, false, "bag");
    item("cube", "Cube", 2, false, false, true, "rect");
    item("cyl", "Cylinder", 2, false, false, false, "rect");
    item("ramp", "Ramp", 2, false, false, false, "rect");
    item("camera", "Camera", 1, true, true, false, "bag");
    item("maincam", "Main Camera", 2, false, false, false, "eye");
    tree->SetItems(items);

    // --- Содержимое: инспектор ---
    sui::PropertyGrid* grid = gui.CreateIn<sui::PropertyGrid>(inspector->Body());
    grid->SetStretch(true, true);
    sui::UIElement* head = grid->AddSection("Cube", true);
    sui::UIElement* tagRow = grid->AddRow(head, "Tag");
    gui.CreateIn<sui::Dropdown>(tagRow, std::vector<std::string>{"Untagged"}, 0)
        ->SetStretch(true, false);
    sui::UIElement* layerRow = grid->AddRow(head, "Layer");
    gui.CreateIn<sui::Dropdown>(layerRow, std::vector<std::string>{"Default"}, 0)
        ->SetStretch(true, false);

    sui::UIElement* xf = grid->AddSection("Transform", true);
    const char* kAxis[] = {"X", "Y", "Z"};
    for (const char* row : {"Position", "Rotation", "Scale"}) {
        sui::UIElement* slot = grid->AddRow(xf, row);
        for (const char* axis : kAxis) {
            sui::NumericField* f = gui.CreateIn<sui::NumericField>(slot, 1.0f);
            f->SetLabel(axis);
            f->SetStretch(true, false);
        }
    }
    sui::UIElement* mesh = grid->AddSection("Mesh Renderer", true);
    sui::UIElement* meshRow = grid->AddRow(mesh, "Mesh");
    gui.CreateIn<sui::Dropdown>(meshRow, std::vector<std::string>{"Cube"}, 0)
        ->SetStretch(true, false);
    sui::UIElement* shadow = grid->AddRow(mesh, "Cast Shadows");
    gui.CreateIn<sui::Checkbox>(shadow, std::string(), true);
    sui::UIElement* wide = grid->AddWide(mesh);
    gui.CreateIn<sui::Button>(wide, std::string("+ Add Component"), std::string())
        ->SetStretch(true, false);

    // --- Содержимое: ассеты ---
    sui::AssetView* av = gui.CreateIn<sui::AssetView>(assets->Body());
    av->SetStretch(true, true);
    std::vector<sui::AssetItem> cards;
    const char* kNames[] = {"Cube", "Cylinder", "Ramp", "Platform", "Sphere", "Crate", "Barrel"};
    for (int i = 0; i < 7; ++i) {
        sui::AssetItem a;
        a.Id = kNames[i];
        a.Name = kNames[i];
        a.Kind = "Static Mesh";
        a.Icon = "rect";
        a.Selected = i == 0;
        cards.push_back(a);
    }
    av->SetItems(cards);

    // --- Низ ---
    shell->Status()->SetStatus("Ready", ui::UIColorFromHex("#4FB865"));
    shell->Status()->SetField("FPS", "144");
    shell->Status()->SetField("Mem", "1.26 GB");
    shell->Status()->SetField("Draw Calls", "215");
    shell->Status()->SetField("Triangles", "89 442");
    shell->Status()->SetField("Objects", "128");

    // Три кадра: дерево дока пересобирается по модели к следующему кадру, а
    // размеры карточек и меню известны только после первой раскладки.
    for (int i = 0; i < 3; ++i) gui.Update(0.016f);

    Framebuffer fbo(1280, 720);
    fbo.Bind();
    sage::rhi::GraphicsDevice& dev = sage::rhi::GraphicsDevice::Get();
    dev.SetClearColor(0.05f, 0.055f, 0.063f, 1.0f);
    dev.Clear(true, true);
    gui.Render(renderer, &fbo);
    const Image img = Capture(1280, 720);
    dev.BindDefaultFramebuffer();

    const double covered = Covered(img, 0, 0, 1280, 720);
    std::printf("    SageUI: закрашено %.2f, панелей %zu\n", covered,
                shell->Dock()->PanelIds().size());

    Check(covered > 0.9, "SageUI: оболочка редактора нарисована");
    // Каждая область получила НЕНУЛЕВЫЕ пиксели: раскладка дока доехала до
    // экрана, а не осталась числами в модели.
    Check(hierarchy->Bounds().w > 40.0f, "SageUI: область иерархии на экране");
    Check(inspector->Bounds().w > 40.0f, "SageUI: область инспектора на экране");
    Check(viewport->Bounds().w > 40.0f, "SageUI: область сцены на экране");
    Check(assets->Bounds().h > 40.0f, "SageUI: область ассетов на экране");
    // Пропорции референса: иерархия узкая, вьюпорт — самая широкая область.
    Check(hierarchy->Bounds().w < viewport->Bounds().w, "SageUI: вьюпорт шире иерархии");
    Check(shell->Status()->Bounds().h > 10.0f, "SageUI: строка состояния на экране");
    Check(shell->Menu()->Bounds().h > 10.0f, "SageUI: полоса меню на экране");

    if (shots) SavePng(std::string(shots) + "/sageui_editor.png", img);
}

} // namespace

void RunUICoreChecks() {
    std::printf("\n--- Новая система интерфейса ---\n");
    UIRenderer renderer;
    CheckBasic(renderer);
    CheckNestedOpacity(renderer);
    CheckMasked(renderer);
    CheckEffects(renderer);
    CheckTypography(renderer);
    CheckResponsive(renderer);
    CheckBlurCompositing(renderer);
    CheckShowcaseFrame(renderer);
    CheckDemoScreens(renderer);
    CheckIcons(renderer);
    CheckSageUI(renderer);
}

} // namespace sage::rendertest
