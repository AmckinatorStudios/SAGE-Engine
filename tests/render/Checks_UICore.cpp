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
#include <memory>

#include "sage/render/Framebuffer.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/ui/UIFramework.h"
#include "sage/ui/UIRenderer.h"
#include "sage/ui/render/UIEngineResources.h"
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
}

} // namespace sage::rendertest
