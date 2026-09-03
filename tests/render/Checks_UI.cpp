// ---------------------------------------------------------------------------
// Эталонные кадры — ИНТЕРФЕЙС ИГРЫ.
//
// ЗАЧЕМ ИМЕННО КАРТИНКА. Правило новой системы частей — «часть рисует себя в
// прямоугольнике СВОЕГО элемента и не двигает соседей» — проверяемо данными
// только наполовину: тесты видят, что подпись переехала в ребёнка, а вот
// закрасила ли подложка галку целиком, наехал ли значок на текст и осталась ли
// дорожка ползунка тонкой полосой — видно только на пикселях. Ровно эти три
// поломки и были возможны при переходе, и ровно они не роняют ни один
// модульный тест.
//
// Меряются не «похожие картинки», а ЧИСЛА С СМЫСЛОМ: доля закрашенного,
// яркость по колонкам, разница между двумя кадрами. Эталон здесь был бы хуже:
// шрифт и сглаживание у разных драйверов свои, а «галка не закрасила ряд» —
// утверждение, которое от драйвера не зависит.
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include <cmath>
#include <cstdio>
#include <memory>

#include "sage/render/Framebuffer.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIDemos.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UIRenderer.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/components/Interact.h"
#include "sage/ui/components/Layout.h"
#include "sage/ui/components/Visual.h"

namespace sage::rendertest {
namespace {

// Ставит элементу прямоугольник в левом верхнем углу кадра. Вызывается и ПОСЛЕ
// заготовки: заготовка приносит свой прямоугольник (кнопка 200x52 по центру), и
// мерить доли в нём значило бы гадать, где он оказался.
void PlaceTopLeft(Scene& scene, GameObject e, glm::vec2 size);

constexpr int kUiW = 480;
constexpr int kUiH = 270;

// Кадр с интерфейсом сцены на чёрном фоне: фон намеренно чёрный, чтобы
// «закрашено» и «не закрашено» отличались без порогов и подбора.
Image RenderUI(UIRenderer& ui, Scene& scene, int w = kUiW, int h = kUiH) {
    Framebuffer fbo(w, h);
    fbo.Bind();
    sage::rhi::GraphicsDevice& dev = sage::rhi::GraphicsDevice::Get();
    dev.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    dev.Clear(true, true);
    ui.Begin(w, h);
    sage::ui::DrawSceneUI(scene, ui, w, h);
    ui.End();
    Image img = Capture(w, h);
    dev.BindDefaultFramebuffer();
    return img;
}

// Средняя яркость прямоугольника кадра, 0..255.
double Luma(const Image& img, int x0, int y0, int x1, int y1) {
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, img.Width); y1 = std::min(y1, img.Height);
    if (x1 <= x0 || y1 <= y0) return 0.0;
    double sum = 0.0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t i = ((size_t)y * img.Width + x) * 3;
            sum += 0.299 * img.Pixels[i] + 0.587 * img.Pixels[i + 1] + 0.114 * img.Pixels[i + 2];
        }
    }
    return sum / ((x1 - x0) * (y1 - y0));
}

// Доля заметно закрашенных пикселей прямоугольника.
double Covered(const Image& img, int x0, int y0, int x1, int y1) {
    x0 = std::max(x0, 0); y0 = std::max(y0, 0);
    x1 = std::min(x1, img.Width); y1 = std::min(y1, img.Height);
    if (x1 <= x0 || y1 <= y0) return 0.0;
    int lit = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t i = ((size_t)y * img.Width + x) * 3;
            if (img.Pixels[i] > 12 || img.Pixels[i + 1] > 12 || img.Pixels[i + 2] > 12) ++lit;
        }
    }
    return (double)lit / ((x1 - x0) * (y1 - y0));
}

// Элемент в левом верхнем углу кадра, чтобы прямоугольник кадра и
// прямоугольник элемента совпадали: так измеренные доли читаются прямо, без
// пересчёта координат.
GameObject Screen(Scene& scene, const char* name, glm::vec2 size) {
    GameObject e = scene.CreateObject(name);
    scene.Registry().emplace<sage::ui::Transform>(e.Entity(), sage::ui::Transform{});
    PlaceTopLeft(scene, e, size);
    return e;
}

void PlaceTopLeft(Scene& scene, GameObject e, glm::vec2 size) {
    sage::ui::Transform& t = scene.Registry().get<sage::ui::Transform>(e.Entity());
    t.Anchor = UIAnchor::TopLeft;
    t.Mode = sage::ui::Transform::Stretch::None;
    t.Offset = {0.0f, 0.0f};
    t.Size = size;
}

// --- Галка не закрашивает свой ряд -----------------------------------------
//
// Старая отрисовка при виде «галка» подменяла заливку элемента квадратиком, и
// перенос этого правила в независимые части грозил ровно обратным: подложка
// красит элемент целиком, а квадратик ложится сверху. Ряд 200x36 превратился
// бы в сплошную плашку, на которой подпись не видна.
void CheckToggleDoesNotPaintTheRow(UIRenderer& ui) {
    Scene scene("toggle");
    GameObject e = Screen(scene, "Check", {(float)kUiW, 36.0f});
    Check(sage::ui::ApplyPreset(scene, e.Entity(), "Checkbox"), "заготовка применилась");
    PlaceTopLeft(scene, e, {(float)kUiW, 36.0f});
    sage::ui::Range& r = scene.Registry().get<sage::ui::Range>(e.Entity());
    r.Value = 1.0f;   // включена: галочка внутри квадратика
    const Image img = RenderUI(ui, scene);

    // Квадратик — слева, стороной в высоту элемента; подпись-ребёнок стоит за
    // ним (заготовка сдвигает её на 44), остальной ряд обязан быть пуст.
    const double box = Covered(img, 2, 2, 34, 34);
    const double caption = Covered(img, 40, 2, 200, 34);
    const double rest = Covered(img, 220, 2, kUiW - 4, 34);
    std::printf("    галка: квадратик %.2f, подпись %.3f, хвост ряда %.3f\n", box, caption, rest);
    Check(box > 0.5, "галка: квадратик закрашен");
    // Подпись — тонкие штрихи букв, никак не плашка во всю высоту.
    Check(caption > 0.01 && caption < 0.35, "галка: подпись справа от квадратика видна");
    // А дальше подписи — чистый фон: подложка ряд не закрашивает.
    Check(rest < 0.005, "галка: ряд НЕ закрашен подложкой");
}

// --- Значок не наезжает на текст -------------------------------------------
//
// Значок рисуется по центру СВОЕГО элемента. Если бы он остался «квадратиком у
// левого края родителя», подпись-ребёнок, у которой своё поле слева, легла бы
// прямо на него.
void CheckIconAndTextDoNotOverlap(UIRenderer& ui) {
    Scene scene("icon");
    GameObject row = Screen(scene, "Row", {(float)kUiW, 40.0f});

    GameObject iconObj = scene.CreateObject("Icon");
    sage::ui::Transform it;
    it.Anchor = UIAnchor::CenterLeft;
    it.Offset = {4.0f, 0.0f};
    it.Size = {32.0f, 32.0f};
    scene.Registry().emplace<sage::ui::Transform>(iconObj.Entity(), it);
    sage::ui::Icon icon;
    icon.Name = "heart";
    icon.Color = {1.0f, 0.3f, 0.3f, 1.0f};
    scene.Registry().emplace<sage::ui::Icon>(iconObj.Entity(), icon);
    scene.SetParent(iconObj.Entity(), row.Entity());

    GameObject textObj = scene.CreateObject("Text");
    sage::ui::Transform tt;
    tt.Anchor = UIAnchor::TopLeft;
    tt.Mode = sage::ui::Transform::Stretch::Both;
    tt.Margin = {44.0f, 0.0f, 0.0f, 0.0f};
    scene.Registry().emplace<sage::ui::Transform>(textObj.Entity(), tt);
    sage::ui::Label label;
    label.Text = "100 / 100";
    label.Scale = 1.6f;
    label.Horizontal = sage::ui::Label::Align::Start;
    scene.Registry().emplace<sage::ui::Label>(textObj.Entity(), label);
    scene.SetParent(textObj.Entity(), row.Entity());

    const Image img = RenderUI(ui, scene);
    // Значок красный, текст белый: канал зелёного отделяет одно от другого без
    // догадок о форме букв.
    // Закрашенностью, а не средней яркостью: штрихи букв занимают малую долю
    // строки, и средняя яркость подписи заведомо низкая при любом шрифте.
    const double iconCover = Covered(img, 6, 6, 36, 34);
    const double gapCover = Covered(img, 38, 4, 43, 36);
    const double textCover = Covered(img, 46, 4, 200, 36);
    std::printf("    значок+текст: значок %.2f, зазор %.2f, текст %.2f\n", iconCover, gapCover,
                textCover);
    Check(iconCover > 0.15, "значок нарисован");
    Check(textCover > 0.02, "подпись нарисована");
    // Полоса между ними пуста: значит, ничего никуда не наехало.
    Check(gapCover < 0.02, "между значком и подписью пусто");
}

// --- Ползунок остаётся полосой, а не плашкой --------------------------------
void CheckSliderStaysThin(UIRenderer& ui) {
    Scene scene("slider");
    GameObject e = Screen(scene, "Volume", {(float)kUiW, 40.0f});
    Check(sage::ui::ApplyPreset(scene, e.Entity(), "Slider"), "заготовка применилась");
    PlaceTopLeft(scene, e, {(float)kUiW, 40.0f});
    const Image img = RenderUI(ui, scene);

    const double middle = Covered(img, 4, 16, kUiW - 4, 24);
    const double top = Covered(img, 60, 1, kUiW - 60, 8);
    std::printf("    ползунок: дорожка %.2f, верх элемента %.2f\n", middle, top);
    Check(middle > 0.8, "ползунок: дорожка по центру закрашена");
    Check(top < 0.1, "ползунок: верх элемента НЕ закрашен (это не плашка)");
}

// --- Цвет живёт у самой части ------------------------------------------------
//
// Перекрасить ползунок теперь можно его собственным полем. Раньше для этого
// на элемент вешали пустую полосу — правило, которое ниоткуда не следует.
void CheckRangeColourIsItsOwn(UIRenderer& ui) {
    Scene scene("colour");
    GameObject e = Screen(scene, "Volume", {(float)kUiW, 40.0f});
    Check(sage::ui::ApplyPreset(scene, e.Entity(), "Slider"), "заготовка применилась");
    PlaceTopLeft(scene, e, {(float)kUiW, 40.0f});
    sage::ui::Range& r = scene.Registry().get<sage::ui::Range>(e.Entity());
    r.Value = 1.0f;                              // заполнено целиком
    r.AccentColor = {0.1f, 0.1f, 0.1f, 1.0f};    // тёмный акцент
    const Image dark = RenderUI(ui, scene);
    r.AccentColor = {0.2f, 1.0f, 0.4f, 1.0f};    // яркий акцент
    const Image bright = RenderUI(ui, scene);

    const double a = Luma(dark, 4, 16, kUiW - 4, 24);
    const double b = Luma(bright, 4, 16, kUiW - 4, 24);
    std::printf("    ползунок: тёмный акцент %.1f, яркий %.1f\n", a, b);
    Check(b > a + 20.0, "цвет ползунка меняется его собственным полем");
}

// --- Демо-худ рисуется и не сваливается в кучу -------------------------------
void CheckHudDemoDraws(UIRenderer& ui) {
    Scene scene("hud");
    Check(sage::ui::BuildDemo(scene, "hud") >= 0, "демо-худ собрался");
    // Кадр в ОПОРНОМ разрешении холста: иначе координаты пришлось бы
    // пересчитывать масштабом холста, то есть повторять в тесте ту самую
    // формулу, которую тест и проверяет.
    const Image img = RenderUI(ui, scene, 1920, 1080);

    // Полоса здоровья — в левом верхнем углу (отступ 24, размер 320x34);
    // значок и шкала лежат в РАЗНЫХ прямоугольниках, поэтому закрашено и там,
    // и там.
    const double icon = Covered(img, 30, 30, 56, 52);
    const double bar = Covered(img, 70, 32, 330, 50);
    std::printf("    худ: значок %.2f, шкала %.2f\n", icon, bar);
    Check(icon > 0.15, "худ: значок здоровья нарисован");
    Check(bar > 0.5, "худ: шкала здоровья нарисована рядом, а не под значком");
}

} // namespace

void RunUIChecks() {
    std::printf("\n--- Интерфейс игры ---\n");
    UIRenderer ui;
    CheckToggleDoesNotPaintTheRow(ui);
    CheckIconAndTextDoNotOverlap(ui);
    CheckSliderStaysThin(ui);
    CheckRangeColourIsItsOwn(ui);
    CheckHudDemoDraws(ui);
}

} // namespace sage::rendertest
