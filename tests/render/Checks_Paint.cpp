// ---------------------------------------------------------------------------
// Эталонные кадры — рисование в 3D (sage/paint).
//
// Рисование проверяется ЧИСЛАМИ, а не взглядом на картинку, и вопросы здесь
// ровно те, ради которых система написана:
//
//   • КРАСКА ЛОЖИТСЯ ТУДА, КУДА БЬЁТ КИСТЬ. Удар в мировой точке обязан
//     покрасить холст в этой точке и НЕ покрасить далеко от неё. Без этой
//     проверки ошибка в привязке холста к миру (перепутанные оси, смещение на
//     полтекселя) не видна вовсе: картинка «какая-то есть».
//   • У КИСТИ МЯГКИЙ КРАЙ. Между серединой следа и его границей плотность
//     обязана падать постепенно. Ради этого края система и заводилась: с
//     резким краем нарисованное неотличимо от залитого многоугольника.
//   • ЗАКРАШЕННАЯ ОБЛАСТЬ НЕ РАВНА ЗАЛИТОЙ. Край области, обведённой кистью,
//     обязан быть шире и мягче, чем край того же многоугольника, залитого
//     сплошь, — иначе «нарисовано» это просто другое слово для «залито».
//   • ЛАСТИК СНИМАЕТ КРАСКУ. Не закрашивает фоном (на прозрачном холсте фона
//     нет), а именно снимает: альфа обязана падать.
// ---------------------------------------------------------------------------
#include "Fixture.h"

#include <cmath>
#include <cstdio>
#include <vector>

#include "sage/paint/PaintCanvas.h"

namespace sage::rendertest {
namespace {

constexpr int kCanvas = 256;
// Холст на 20x20 метров вокруг начала координат: числа круглые, чтобы перевод
// «метры -> тексели» в проверке можно было посчитать в уме.
const sage::paint::PlanarMapping kMap{{-10.0f, -10.0f}, {10.0f, 10.0f}};

sage::paint::Brush BasicBrush() {
    sage::paint::Brush b;
    b.Color = {0.9f, 0.3f, 0.2f, 1.0f};
    b.Radius = 2.0f;
    b.Hardness = 0.2f;
    b.Flow = 1.0f;
    return b;
}

void CheckStampLandsWhereAimed() {
    sage::paint::Canvas canvas(kCanvas, kMap);
    canvas.Clear();
    canvas.Stamp({3.0f, -4.0f}, BasicBrush());

    const float hit = canvas.SampleWorld({3.0f, -4.0f}).a;
    const float miss = canvas.SampleWorld({-6.0f, 6.0f}).a;
    // Проверяем и попадание, и ПРОМАХ: покрасить весь холст целиком — тоже
    // способ «попасть» в нужную точку, и без второго числа он бы прошёл.
    const bool ok = hit > 0.9f && miss < 0.02f;
    std::printf("    краска: в точке удара %.2f, вдали %.2f\n", hit, miss);
    Check(ok, "рисование: удар кисти красит там, куда бьёт, и только там");
}

void CheckBrushEdgeIsSoft() {
    sage::paint::Canvas canvas(kCanvas, kMap);
    canvas.Clear();
    sage::paint::Brush b = BasicBrush();
    b.Radius = 3.0f;
    b.Hardness = 0.1f;   // нарочно мягкая
    canvas.Stamp({0.0f, 0.0f}, b);

    // Три точки: центр, середина радиуса, почти край.
    const float c = canvas.SampleWorld({0.0f, 0.0f}).a;
    const float mid = canvas.SampleWorld({1.5f, 0.0f}).a;
    const float rim = canvas.SampleWorld({2.8f, 0.0f}).a;
    const bool ok = c > 0.9f && mid < c - 0.05f && rim < mid - 0.05f && rim >= 0.0f;
    std::printf("    плотность по радиусу: центр %.2f, середина %.2f, край %.2f\n", c, mid, rim);
    Check(ok, "рисование: у кисти мягкий край, плотность падает к границе");
}

// Доля текселей на границе области, где краска ЧАСТИЧНАЯ (не 0 и не 1).
// У залитого многоугольника таких почти нет: край — ступенька в один тексель.
// У закрашенного их много: кисть размазывает границу.
float PartialShareAlongEdge(const sage::paint::Canvas& canvas, float x0, float x1, float z) {
    int partial = 0, total = 0;
    for (int i = 0; i <= 200; ++i) {
        const float x = x0 + (x1 - x0) * (float)i / 200.0f;
        const float a = canvas.SampleWorld({x, z}).a;
        ++total;
        if (a > 0.05f && a < 0.95f) ++partial;
    }
    return total > 0 ? (float)partial / (float)total : 0.0f;
}

void CheckPaintedAreaIsNotFilledArea() {
    // Один и тот же квадрат: слева заливаем сплошь, справа закрашиваем кистью.
    const std::vector<Vec2> square{{-4.0f, -4.0f}, {4.0f, -4.0f}, {4.0f, 4.0f}, {-4.0f, 4.0f}};

    sage::paint::Canvas filled(kCanvas, kMap);
    filled.Clear();
    sage::paint::Brush hard = BasicBrush();
    hard.Radius = 0.05f;      // край почти без кисти — это и есть «залито»
    hard.Hardness = 1.0f;
    filled.FillPolygon(square, hard, 0.0f);

    sage::paint::Canvas painted(kCanvas, kMap);
    painted.Clear();
    sage::paint::Brush soft = BasicBrush();
    soft.Radius = 1.2f;
    soft.Hardness = 0.05f;
    painted.FillPolygon(square, soft, 0.4f);

    // Идём поперёк верхней границы квадрата (z = 4) и считаем полупрозрачные
    // тексели: у залитого их единицы, у закрашенного — заметная доля.
    const float filledShare = PartialShareAlongEdge(filled, 4.0f - 2.5f, 4.0f + 2.5f, 0.0f);
    const float paintedShare = PartialShareAlongEdge(painted, 4.0f - 2.5f, 4.0f + 2.5f, 0.0f);
    const bool ok = paintedShare > filledShare + 0.10f;
    std::printf("    мягкость края: залито %.2f, закрашено %.2f\n", filledShare, paintedShare);
    Check(ok, "рисование: закрашенная область мягче по краю, чем залитая");
}

void CheckEraseRemovesPaint() {
    sage::paint::Canvas canvas(kCanvas, kMap);
    canvas.Clear();
    sage::paint::Brush b = BasicBrush();
    b.Radius = 3.0f;
    b.Hardness = 1.0f;
    canvas.Stamp({0.0f, 0.0f}, b);
    const float before = canvas.SampleWorld({0.0f, 0.0f}).a;

    sage::paint::Brush eraser = b;
    eraser.Mode = sage::paint::BlendMode::Erase;
    eraser.Radius = 1.5f;
    canvas.Stamp({0.0f, 0.0f}, eraser);
    const float after = canvas.SampleWorld({0.0f, 0.0f}).a;
    // И проверяем, что стёрли ИМЕННО ПОД ЛАСТИКОМ: краска в стороне цела.
    const float aside = canvas.SampleWorld({2.5f, 0.0f}).a;
    const bool ok = before > 0.9f && after < 0.1f && aside > 0.5f;
    std::printf("    ластик: было %.2f, стало %.2f, в стороне %.2f\n", before, after, aside);
    Check(ok, "рисование: ластик снимает краску под собой и не трогает соседнюю");
}

void CheckStrokeIsContinuous() {
    sage::paint::Canvas canvas(kCanvas, kMap);
    canvas.Clear();
    sage::paint::Brush b = BasicBrush();
    b.Radius = 1.0f;
    b.Hardness = 0.6f;
    canvas.Stroke({-6.0f, 0.0f}, {6.0f, 0.0f}, b);

    // Мазок обязан быть СПЛОШНЫМ: провал между отпечатками — самая частая
    // ошибка (слишком редкий шаг), и заметна она только на просвет.
    int gaps = 0;
    for (int i = 0; i <= 120; ++i) {
        const float x = -6.0f + 12.0f * (float)i / 120.0f;
        if (canvas.SampleWorld({x, 0.0f}).a < 0.5f) ++gaps;
    }
    std::printf("    мазок: провалов по длине %d из 121\n", gaps);
    Check(gaps == 0, "рисование: мазок сплошной, без провалов между ударами");
}

} // namespace

void RunPaintChecks() {
    CheckStampLandsWhereAimed();
    CheckBrushEdgeIsSoft();
    CheckPaintedAreaIsNotFilledArea();
    CheckEraseRemovesPaint();
    CheckStrokeIsContinuous();
}

} // namespace sage::rendertest
