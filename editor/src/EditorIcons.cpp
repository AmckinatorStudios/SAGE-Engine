#include "EditorIcons.h"
#include "EditorTheme.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <unordered_map>

#include "imgui.h"
#include "imgui_internal.h"

#include "Localization.h"
#include "sage/core/Log.h"

namespace EditorIcons {

namespace {

ImU32 Col(const glm::vec3& c, float a = 1.0f) {
    return ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, a));
}

// ---------------------------------------------------------------------------
// Перо: рисует в квадрате (o.x, o.y, s, s), координаты — доли стороны.
//
// ПОЧЕМУ ЗДЕСЬ СТОЛЬКО ОКРУГЛЕНИЙ. Иконка редактора живёт на 14–24 пикселях, и
// на таком размере всё решает попадание в пиксельную сетку. Раньше и толщина
// штриха (0.09 * 16 = 1.44 px), и координаты (0.28 * 16 = 4.48 px) были
// дробными: сглаживание размазывало каждую линию на два ряда пикселей с разной
// яркостью, соседние линии одной иконки получались разной насыщенности, а
// прямоугольники — с «мохнатыми» краями. Именно это читается как «иконки
// кривые»: рисунок правильный, растр — нет.
//
// Поэтому:
//   * толщина штриха — ЦЕЛОЕ число пикселей (минимум один);
//   * координаты штрихов округляются до пикселя и, для нечётной толщины,
//     сдвигаются в его ЦЕНТР: ImGui рисует линию симметрично вокруг координаты,
//     и однопиксельный штрих на границе пикселя размазывается ровно пополам;
//   * заливки прижимаются к границам пикселей — там центр, наоборот, вреден.
// Одна и та же формула годится и для 12, и для 32 пикселей.
// ---------------------------------------------------------------------------
struct Pen {
    ImDrawList* dl;
    ImVec2 o;
    float s;
    ImU32 c;

    // Толщина штриха в пикселях: доля стороны -> целое, но не тоньше пикселя.
    float W(float frac) const { return std::max(1.0f, std::floor(frac * s + 0.5f)); }

    // Точка для ЗАЛИВКИ — на границе пикселя.
    ImVec2 P(float x, float y) const {
        return ImVec2(o.x + std::floor(x * s + 0.5f), o.y + std::floor(y * s + 0.5f));
    }
    // Точка для ШТРИХА толщиной w — с поправкой на его чётность.
    ImVec2 S(float x, float y, float w) const {
        const float half = ((int)w % 2 != 0) ? 0.5f : 0.0f;
        return ImVec2(o.x + std::floor(x * s + 0.5f) + half,
                      o.y + std::floor(y * s + 0.5f) + half);
    }

    void Line(float x0, float y0, float x1, float y1, float w = 0.085f) const {
        const float px = W(w);
        dl->AddLine(S(x0, y0, px), S(x1, y1, px), c, px);
    }
    void Rect(float x0, float y0, float x1, float y1, float w = 0.085f, float r = 0.0f) const {
        const float px = W(w);
        dl->AddRect(S(x0, y0, px), S(x1, y1, px), c, std::floor(r * s), 0, px);
    }
    void FillRect(float x0, float y0, float x1, float y1, float r = 0.0f) const {
        dl->AddRectFilled(P(x0, y0), P(x1, y1), c, std::floor(r * s));
    }
    // Радиус тоже целый: полупиксельный радиус даёт «дрожащий» контур.
    float R(float rad) const { return std::max(1.0f, std::floor(rad * s + 0.5f)); }
    // seg = 0 — ImGui сам подберёт число сегментов под радиус. Прежняя жёсткая
    // шестнадцатиугольная окружность на 24 пикселях была видна как многоугольник.
    void Circle(float cx, float cy, float rad, float w = 0.085f) const {
        const float px = W(w);
        dl->AddCircle(S(cx, cy, px), R(rad), c, 0, px);
    }
    void Disc(float cx, float cy, float rad) const {
        dl->AddCircleFilled(P(cx, cy), R(rad), c, 0);
    }
    void Tri(float x0, float y0, float x1, float y1, float x2, float y2) const {
        dl->AddTriangleFilled(P(x0, y0), P(x1, y1), P(x2, y2), c);
    }
    // Ромб (ключевой кадр, маркер): две половины одного четырёхугольника.
    void Diamond(float cx, float cy, float rad) const {
        Tri(cx, cy - rad, cx + rad, cy, cx, cy + rad);
        Tri(cx, cy - rad, cx - rad, cy, cx, cy + rad);
    }
    // Дуга: угол в оборотах (0 — вправо, 0.25 — вниз), чтобы не сыпать pi по коду.
    void Arc(float cx, float cy, float rad, float from, float to, float w = 0.085f) const {
        const float px = W(w);
        dl->PathArcTo(S(cx, cy, px), R(rad), from * 6.2831853f, to * 6.2831853f, 0);
        dl->PathStroke(c, 0, px);
    }
    // Заполненный сектор — половина шара для «затенения» (shader).
    void Pie(float cx, float cy, float rad, float from, float to) const {
        dl->PathArcTo(P(cx, cy), R(rad), from * 6.2831853f, to * 6.2831853f, 0);
        dl->PathFillConvex(c);
    }
    // Ломаная штрихом: контур документа, открытой папки, каркаса. Отдельным
    // помощником, потому что рисовать такой контур россыпью Line() — верный
    // способ разъехаться на одном звене и не заметить.
    void Poly(std::initializer_list<float> xy, bool closed, float w = 0.085f) const {
        const float px = W(w);
        const float* v = xy.begin();
        const int n = (int)xy.size() / 2;
        for (int i = 0; i + 1 < n; ++i)
            dl->AddLine(S(v[i * 2], v[i * 2 + 1], px), S(v[i * 2 + 2], v[i * 2 + 3], px), c, px);
        if (closed && n > 2)
            dl->AddLine(S(v[(n - 1) * 2], v[(n - 1) * 2 + 1], px), S(v[0], v[1], px), c, px);
    }

    // Хватает ли размера на мелкую деталь.
    //
    // На 12–14 пикселях внутренние штрихи (жилки листа, ножки жука, блик на
    // шаре) занимают меньше пикселя и превращаются в грязь: иконка становится
    // темнее и хуже узнаётся, чем без них. Профессиональные наборы поэтому
    // рисуют РАЗНЫЕ рисунки на разные размеры; здесь тот же приём в одном
    // рисунке — деталь просто не появляется, пока ей не хватает места.
    bool Fine() const { return s >= 15.0f; }

    void Curve(float x0, float y0, float cx0, float cy0, float cx1, float cy1, float x1, float y1,
               float w = 0.085f) const {
        const float px = W(w);
        dl->AddBezierCubic(S(x0, y0, px), S(cx0, cy0, px), S(cx1, cy1, px), S(x1, y1, px), c, px, 0);
    }
    // Наконечник стрелки: остриё в (tx, ty), направление задаётся знаками dx/dy.
    void ArrowUp(float cx, float ty, float half, float len) const {
        Tri(cx, ty, cx - half, ty + len, cx + half, ty + len);
    }
    void ArrowDown(float cx, float ty, float half, float len) const {
        Tri(cx, ty, cx - half, ty - len, cx + half, ty - len);
    }
    void ArrowLeft(float tx, float cy, float half, float len) const {
        Tri(tx, cy, tx + len, cy - half, tx + len, cy + half);
    }
    void ArrowRight(float tx, float cy, float half, float len) const {
        Tri(tx, cy, tx - len, cy - half, tx - len, cy + half);
    }
};

// Единственное место, где живёт форма каждой иконки. Возвращает false для
// незнакомого имени — вызывающий рисует заглушку.
//
// ГЕОМЕТРИЯ КРАТНА 1/16. Это не педантизм: при таком шаге узловые точки на
// типичных размерах (16, 24, 32 px) попадают в целые пиксели без округления, и
// иконка выглядит одинаково на всех трёх. Раньше координаты подбирались на
// глаз (0.28, 0.34, 0.62…), и на каждом размере рисунок «плыл» по-своему.
// Тонкий штрих (0.06) оставлен только для внутренних деталей, чтобы они не
// спорили с контуром.
bool Shape(const Pen& p, const char* n) {
    auto is = [&](const char* x) { return std::strcmp(n, x) == 0; };

    // --- Транспорт Play-режима ---
    if (is("play"))  { p.Tri(0.3125f, 0.1875f, 0.3125f, 0.8125f, 0.8125f, 0.5f); return true; }
    if (is("pause")) {
        p.FillRect(0.3125f, 0.1875f, 0.4375f, 0.8125f);
        p.FillRect(0.5625f, 0.1875f, 0.6875f, 0.8125f);
        return true;
    }
    if (is("stop"))  { p.FillRect(0.25f, 0.25f, 0.75f, 0.75f, 0.0625f); return true; }
    if (is("step"))  {
        p.Tri(0.25f, 0.1875f, 0.25f, 0.8125f, 0.625f, 0.5f);
        p.FillRect(0.6875f, 0.1875f, 0.8125f, 0.8125f);
        return true;
    }

    // --- Инструменты гизмо ---
    // Перенос: крест с наконечниками на ВСЕХ четырёх концах. Раньше стрелки
    // были только вверх и вправо, и значок выглядел перекошенным.
    if (is("move")) {
        p.Line(0.5f, 0.21875f, 0.5f, 0.78125f);
        p.Line(0.21875f, 0.5f, 0.78125f, 0.5f);
        p.ArrowUp(0.5f, 0.0625f, 0.09375f, 0.15625f);
        p.ArrowDown(0.5f, 0.9375f, 0.09375f, 0.15625f);
        p.ArrowLeft(0.0625f, 0.5f, 0.09375f, 0.15625f);
        p.ArrowRight(0.9375f, 0.5f, 0.09375f, 0.15625f);
        return true;
    }
    // Поворот: разомкнутое кольцо, наконечник стоит РОВНО на конце дуги.
    // Прежний висел сам по себе в стороне от неё.
    if (is("rotate")) {
        p.Arc(0.5f, 0.5f, 0.3125f, 0.0f, 0.75f);
        p.ArrowRight(0.625f, 0.1875f, 0.125f, 0.1875f);
        return true;
    }
    // Масштаб: угловой квадрат и диагональная стрелка от него.
    if (is("scale")) {
        p.Rect(0.1875f, 0.5625f, 0.4375f, 0.8125f, 0.075f);
        p.Line(0.5f, 0.5f, 0.75f, 0.25f, 0.075f);
        p.Tri(0.8125f, 0.1875f, 0.5625f, 0.25f, 0.75f, 0.4375f);
        return true;
    }
    // Универсальный: кольцо поворота и четыре стрелки переноса. Уголок
    // масштаба убран — на 16 пикселях три разных знака в одном квадрате
    // сливались в кашу, а смысл «всё сразу» несут и два.
    if (is("universal")) {
        p.Circle(0.5f, 0.5f, 0.1875f, 0.07f);
        p.ArrowUp(0.5f, 0.0625f, 0.125f, 0.1875f);
        p.ArrowDown(0.5f, 0.9375f, 0.125f, 0.1875f);
        p.ArrowLeft(0.0625f, 0.5f, 0.125f, 0.1875f);
        p.ArrowRight(0.9375f, 0.5f, 0.125f, 0.1875f);
        return true;
    }
    // Рамка UI: прямоугольник с ручками ровно по углам.
    if (is("rect")) {
        p.Rect(0.1875f, 0.25f, 0.8125f, 0.75f, 0.06f);
        const float h = 0.0625f;
        p.FillRect(0.1875f - h, 0.25f - h, 0.1875f + h, 0.25f + h);
        p.FillRect(0.8125f - h, 0.25f - h, 0.8125f + h, 0.25f + h);
        p.FillRect(0.1875f - h, 0.75f - h, 0.1875f + h, 0.75f + h);
        p.FillRect(0.8125f - h, 0.75f - h, 0.8125f + h, 0.75f + h);
        return true;
    }
    // Выравнивание: три бруска, прижатых к одной линии (равные высоты и зазоры).
    if (is("align")) {
        p.Line(0.1875f, 0.125f, 0.1875f, 0.875f, 0.075f);
        p.FillRect(0.3125f, 0.1875f, 0.8125f, 0.3125f);
        p.FillRect(0.3125f, 0.4375f, 0.625f, 0.5625f);
        p.FillRect(0.3125f, 0.6875f, 0.75f, 0.8125f);
        return true;
    }
    // Посадить на поверхность: стрелка вниз, упирающаяся в опору.
    if (is("drop")) {
        p.Line(0.5f, 0.125f, 0.5f, 0.5f);
        p.ArrowDown(0.5f, 0.6875f, 0.1875f, 0.1875f);
        p.FillRect(0.125f, 0.8125f, 0.875f, 0.875f);
        return true;
    }

    // --- Режимы вида ---
    if (is("grid")) {
        p.Rect(0.1875f, 0.1875f, 0.8125f, 0.8125f, 0.07f);
        for (int i = 1; i <= 2; ++i) {
            const float t = 0.1875f + 0.2083f * (float)i;
            p.Line(t, 0.1875f, t, 0.8125f, 0.05f);
            p.Line(0.1875f, t, 0.8125f, t, 0.05f);
        }
        return true;
    }
    // Каркас: четырёхугольник, разбитый на треугольники, — именно это и
    // показывает режим. Две сдвинутые рамки, стоявшие тут раньше, читались как
    // «копия», а не как каркас.
    if (is("wire")) {
        p.Rect(0.125f, 0.1875f, 0.875f, 0.8125f, 0.07f);
        p.Line(0.5f, 0.1875f, 0.5f, 0.8125f, 0.05f);
        p.Line(0.125f, 0.5f, 0.875f, 0.5f, 0.05f);
        // Диагонали — то, чем сетка отличается от таблицы: полигоны треугольные.
        p.Line(0.125f, 0.1875f, 0.5f, 0.5f, 0.05f);
        p.Line(0.5f, 0.5f, 0.875f, 0.8125f, 0.05f);
        return true;
    }

    // --- Типы сущностей: по ним читается иерархия ---
    if (is("cube")) {
        p.Line(0.5f, 0.125f, 0.875f, 0.3125f);
        p.Line(0.875f, 0.3125f, 0.875f, 0.6875f);
        p.Line(0.875f, 0.6875f, 0.5f, 0.875f);
        p.Line(0.5f, 0.875f, 0.125f, 0.6875f);
        p.Line(0.125f, 0.6875f, 0.125f, 0.3125f);
        p.Line(0.125f, 0.3125f, 0.5f, 0.125f);
        p.Line(0.5f, 0.5f, 0.5f, 0.875f, 0.055f);
        p.Line(0.5f, 0.5f, 0.125f, 0.3125f, 0.055f);
        p.Line(0.5f, 0.5f, 0.875f, 0.3125f, 0.055f);
        return true;
    }
    // Шар: окружность и «экватор» кривой — плоский круг сам по себе шаром не
    // читается, а прежняя полудуга поверх контура выглядела обрывком.
    if (is("sphere")) {
        p.Circle(0.5f, 0.5f, 0.3125f);
        p.Curve(0.1875f, 0.5f, 0.3125f, 0.65625f, 0.6875f, 0.65625f, 0.8125f, 0.5f, 0.055f);
        p.Curve(0.1875f, 0.5f, 0.3125f, 0.34375f, 0.6875f, 0.34375f, 0.8125f, 0.5f, 0.055f);
        return true;
    }
    // Лампа: колба с цоколем. Раньше точечный свет рисовался теми же лучами,
    // что и солнце, — в иерархии их было не различить.
    if (is("light")) {
        p.Circle(0.5f, 0.375f, 0.25f, 0.075f);
        p.Line(0.375f, 0.625f, 0.375f, 0.6875f, 0.075f);
        p.Line(0.625f, 0.625f, 0.625f, 0.6875f, 0.075f);
        p.Line(0.375f, 0.6875f, 0.625f, 0.6875f, 0.075f);
        p.Line(0.4375f, 0.8125f, 0.5625f, 0.8125f, 0.075f);
        return true;
    }
    if (is("sun")) {
        p.Disc(0.5f, 0.5f, 0.1875f);
        for (int i = 0; i < 8; ++i) {
            const float a = (float)i / 8.0f * 6.2831853f;
            p.Line(0.5f + std::cos(a) * 0.28f, 0.5f + std::sin(a) * 0.28f,
                   0.5f + std::cos(a) * 0.42f, 0.5f + std::sin(a) * 0.42f, 0.06f);
        }
        return true;
    }
    if (is("camera")) {
        p.Rect(0.125f, 0.3125f, 0.625f, 0.6875f, 0.075f, 0.0625f);
        p.Tri(0.6875f, 0.5f, 0.875f, 0.3125f, 0.875f, 0.6875f);
        return true;
    }
    if (is("script")) {
        p.Rect(0.25f, 0.125f, 0.75f, 0.875f, 0.075f, 0.0625f);
        p.Line(0.375f, 0.3125f, 0.625f, 0.3125f, 0.055f);
        p.Line(0.375f, 0.5f, 0.625f, 0.5f, 0.055f);
        p.Line(0.375f, 0.6875f, 0.5625f, 0.6875f, 0.055f);
        return true;
    }
    // Частицы: россыпь по диагонали, размер убывает от источника.
    if (is("particles")) {
        p.Disc(0.3125f, 0.6875f, 0.125f);
        p.Disc(0.5625f, 0.5f, 0.09f);
        p.Disc(0.75f, 0.3125f, 0.0625f);
        p.Disc(0.375f, 0.3125f, 0.05f);
        return true;
    }
    // Анимация: два ключевых кадра на дорожке — именно так её показывает
    // всякий редактор анимации, и это отличает её от «связи двух объектов».
    if (is("anim")) {
        p.Line(0.125f, 0.5f, 0.875f, 0.5f, 0.055f);
        p.Diamond(0.3125f, 0.5f, 0.125f);
        p.Diamond(0.6875f, 0.5f, 0.125f);
        return true;
    }
    if (is("ik")) {
        p.Line(0.25f, 0.25f, 0.4375f, 0.6875f, 0.055f);
        p.Line(0.4375f, 0.6875f, 0.8125f, 0.5f, 0.055f);
        p.Disc(0.25f, 0.25f, 0.115f);
        p.Disc(0.4375f, 0.6875f, 0.115f);
        p.Disc(0.8125f, 0.5f, 0.115f);
        return true;
    }
    // Зонд отражений: шар в габаритной коробке.
    // Сеть: два узла и связь между ними. Не «глобус» и не «шестерёнка»: узел,
    // канал, узел — единственная форма, которая читается как связь машин, а не
    // как интернет вообще.
    if (is("network")) {
        p.Line(0.25f, 0.30f, 0.75f, 0.70f, 0.055f);
        p.Disc(0.25f, 0.30f, 0.15f);
        p.Circle(0.75f, 0.70f, 0.15f, 0.07f);
        return true;
    }
    if (is("probe")) {
        p.Rect(0.125f, 0.125f, 0.875f, 0.875f, 0.05f, 0.0625f);
        p.Circle(0.5f, 0.5f, 0.25f, 0.07f);
        p.Disc(0.4375f, 0.4375f, 0.0625f);
        return true;
    }
    // Физика: шар, падающий на опору. Прежний круг с перекрестием читался как
    // запрещающий знак.
    // Физика: шар, летящий на опору, и след его пути. Две вертикальные чёрточки
    // над шаром, стоявшие здесь раньше, читались как усики, а не как движение.
    if (is("physics")) {
        p.Disc(0.625f, 0.5f, 0.1875f);
        p.FillRect(0.0625f, 0.8125f, 0.9375f, 0.875f);
        p.Line(0.0625f, 0.1875f, 0.3125f, 0.4375f, 0.06f);
        if (p.Fine()) p.Line(0.0625f, 0.4375f, 0.1875f, 0.5625f, 0.06f);
        return true;
    }

    // --- Ассеты ---
    if (is("folder")) {
        p.FillRect(0.125f, 0.25f, 0.4375f, 0.375f, 0.03f);
        p.Rect(0.125f, 0.3125f, 0.875f, 0.75f, 0.075f, 0.05f);
        return true;
    }
    // Файл: лист С СОСТРИЖЕННЫМ УГЛОМ и загибом. Прежний был просто
    // прямоугольником, через который наискось шла черта, — угол не срезан, и
    // получался не документ, а перечёркнутая рамка.
    if (is("file")) {
        p.Poly({0.25f, 0.125f, 0.5625f, 0.125f, 0.75f, 0.3125f, 0.75f, 0.875f,
                0.25f, 0.875f}, true, 0.075f);
        p.Poly({0.5625f, 0.125f, 0.5625f, 0.3125f, 0.75f, 0.3125f}, false, 0.06f);
        return true;
    }
    // Сцена: объекты на земле. Раньше это была та же картинка в рамке, что и
    // текстура, — два разных ассета выглядели одинаково.
    if (is("scene")) {
        // Земля во всю ширину, на ней куб и шар. Прежние фигурки были мельче и
        // стояли внахлёст с линией земли — на 16 пикселях это читалось как
        // случайная россыпь чёрточек.
        p.Line(0.0625f, 0.75f, 0.9375f, 0.75f, 0.075f);
        p.Rect(0.125f, 0.4375f, 0.4375f, 0.75f, 0.075f);
        p.Circle(0.6875f, 0.5625f, 0.1875f, 0.075f);
        return true;
    }
    // Материал: шар с бликом и затенённым низом — так его показывает всякий
    // просмотрщик материалов. Прежний круг с точкой внутри читался как «радио-
    // кнопка», а не как образец материала.
    if (is("material")) {
        p.Circle(0.5f, 0.5f, 0.3125f);
        p.Pie(0.5f, 0.5f, 0.25f, 0.10f, 0.55f);   // тень снизу
        if (p.Fine()) p.Disc(0.40625f, 0.375f, 0.0625f); // блик
        return true;
    }
    if (is("project")) {
        p.FillRect(0.125f, 0.1875f, 0.4375f, 0.3125f, 0.03f);
        p.Rect(0.125f, 0.25f, 0.875f, 0.8125f, 0.075f, 0.05f);
        p.Disc(0.5f, 0.5625f, 0.09f);
        return true;
    }
    // Префаб: коробка с меткой в углу. Две сдвинутые рамки, стоявшие здесь
    // раньше, ничем не отличались от значка «копировать».
    if (is("prefab")) {
        p.Line(0.5f, 0.125f, 0.8125f, 0.3125f);
        p.Line(0.8125f, 0.3125f, 0.8125f, 0.6875f);
        p.Line(0.8125f, 0.6875f, 0.5f, 0.875f);
        p.Line(0.5f, 0.875f, 0.1875f, 0.6875f);
        p.Line(0.1875f, 0.6875f, 0.1875f, 0.3125f);
        p.Line(0.1875f, 0.3125f, 0.5f, 0.125f);
        p.Disc(0.5f, 0.5f, 0.125f);
        return true;
    }
    if (is("texture")) {
        p.Rect(0.125f, 0.1875f, 0.875f, 0.8125f, 0.075f, 0.05f);
        p.Tri(0.1875f, 0.6875f, 0.4375f, 0.375f, 0.625f, 0.6875f);
        p.Disc(0.6875f, 0.3125f, 0.075f);
        p.Line(0.125f, 0.6875f, 0.875f, 0.6875f, 0.05f);
        return true;
    }
    // Шейдер: шар, у которого освещена половина, — это и есть затенение.
    if (is("shader")) {
        p.Circle(0.5f, 0.5f, 0.3125f);
        p.Pie(0.5f, 0.5f, 0.25f, 0.75f, 1.25f);
        return true;
    }
    if (is("audio")) {
        p.Tri(0.1875f, 0.5f, 0.4375f, 0.25f, 0.4375f, 0.75f);
        p.FillRect(0.1875f, 0.4375f, 0.3125f, 0.5625f);
        p.Arc(0.5f, 0.5f, 0.1875f, -0.18f, 0.18f, 0.06f);
        p.Arc(0.5f, 0.5f, 0.3125f, -0.16f, 0.16f, 0.06f);
        return true;
    }
    if (is("model")) {
        p.Line(0.5f, 0.125f, 0.875f, 0.3125f);
        p.Line(0.875f, 0.3125f, 0.5f, 0.5f);
        p.Line(0.5f, 0.5f, 0.125f, 0.3125f);
        p.Line(0.125f, 0.3125f, 0.5f, 0.125f);
        p.Line(0.125f, 0.3125f, 0.125f, 0.6875f, 0.055f);
        p.Line(0.875f, 0.3125f, 0.875f, 0.6875f, 0.055f);
        p.Line(0.5f, 0.5f, 0.5f, 0.875f, 0.055f);
        p.Line(0.125f, 0.6875f, 0.5f, 0.875f, 0.055f);
        p.Line(0.875f, 0.6875f, 0.5f, 0.875f, 0.055f);
        p.Line(0.125f, 0.3125f, 0.875f, 0.3125f, 0.055f);
        return true;
    }

    // --- Навигация и действия ---
    if (is("up")) {
        p.Line(0.5f, 0.8125f, 0.5f, 0.3125f, 0.075f);
        p.ArrowUp(0.5f, 0.1875f, 0.1875f, 0.1875f);
        return true;
    }
    // Обновить: кольцо с наконечником на конце дуги (а не рядом с ней).
    if (is("refresh")) {
        p.Arc(0.5f, 0.5f, 0.3125f, 0.05f, 0.80f, 0.075f);
        p.ArrowRight(0.75f, 0.1875f, 0.125f, 0.1875f);
        return true;
    }
    if (is("folder-plus")) {
        p.FillRect(0.125f, 0.25f, 0.4375f, 0.375f, 0.03f);
        p.Rect(0.125f, 0.3125f, 0.875f, 0.75f, 0.075f, 0.05f);
        p.Line(0.5f, 0.4375f, 0.5f, 0.625f, 0.07f);
        p.Line(0.40625f, 0.53125f, 0.59375f, 0.53125f, 0.07f);
        return true;
    }
    if (is("search")) {
        p.Circle(0.4375f, 0.4375f, 0.25f);
        p.Line(0.625f, 0.625f, 0.8125f, 0.8125f, 0.09f);
        return true;
    }
    if (is("code")) {
        p.Line(0.375f, 0.3125f, 0.1875f, 0.5f, 0.075f);
        p.Line(0.1875f, 0.5f, 0.375f, 0.6875f, 0.075f);
        p.Line(0.625f, 0.3125f, 0.8125f, 0.5f, 0.075f);
        p.Line(0.8125f, 0.5f, 0.625f, 0.6875f, 0.075f);
        p.Line(0.5625f, 0.25f, 0.4375f, 0.75f, 0.065f);
        return true;
    }
    if (is("question")) {
        p.Circle(0.5f, 0.5f, 0.375f, 0.075f);
        p.Arc(0.5f, 0.375f, 0.125f, 0.55f, 1.05f, 0.07f);
        p.Line(0.5f, 0.5f, 0.5f, 0.5625f, 0.07f);
        p.Disc(0.5f, 0.6875f, 0.05f);
        return true;
    }
    if (is("layout")) {
        p.Rect(0.125f, 0.1875f, 0.875f, 0.8125f, 0.06f, 0.05f);
        p.Line(0.5f, 0.1875f, 0.5f, 0.8125f, 0.05f);
        p.Line(0.125f, 0.5f, 0.875f, 0.5f, 0.05f);
        return true;
    }
    // Магнит — привязка к шагу. Подкова, а не слово «Snap»: подпись занимала в
    // строке инструментов вчетверо больше места, чем сама галка.
    if (is("magnet")) {
        p.Arc(0.5f, 0.5625f, 0.28f, 0.5f, 1.0f, 0.13f);
        p.Line(0.22f, 0.5625f, 0.22f, 0.8125f, 0.13f);
        p.Line(0.78f, 0.5625f, 0.78f, 0.8125f, 0.13f);
        return true;
    }

    // Шестерня — настройки. Зубья считаются, а не выписываются: восемь
    // одинаковых лучей от руки разъезжаются по углу, и на 16 пикселях это
    // видно как «мятая» иконка.
    if (is("gear")) {
        constexpr int kTeeth = 8;
        for (int i = 0; i < kTeeth; ++i) {
            const float a = (float)i * (6.2831853f / (float)kTeeth);
            const float ca = std::cos(a), sa = std::sin(a);
            p.Line(0.5f + ca * 0.24f, 0.5f + sa * 0.24f,
                   0.5f + ca * 0.42f, 0.5f + sa * 0.42f, 0.11f);
        }
        p.Circle(0.5f, 0.5f, 0.26f, 0.10f);
        p.Circle(0.5f, 0.5f, 0.10f, 0.08f);
        return true;
    }

    // --- Уровни сообщений (цвет несёт смысл наравне с формой) ---
    // Предупреждение и ошибка — ОДНА СЕМЬЯ со «справкой» и «вопросом»: контур
    // плюс знак внутри. Сплошной треугольник и сплошной круг, стоявшие тут
    // раньше, не несли вообще ничего: в консоли, где рядом три уровня
    // сообщений, они различались только цветом, а по форме были пятнами.
    if (is("warn")) {
        p.Poly({0.5f, 0.125f, 0.9375f, 0.84375f, 0.0625f, 0.84375f}, true, 0.085f);
        p.Line(0.5f, 0.40625f, 0.5f, 0.625f, 0.085f);
        p.Disc(0.5f, 0.75f, 0.0625f);
        return true;
    }
    if (is("error")) {
        p.Circle(0.5f, 0.5f, 0.375f, 0.09f);
        p.Line(0.34375f, 0.34375f, 0.65625f, 0.65625f, 0.09f);
        p.Line(0.65625f, 0.34375f, 0.34375f, 0.65625f, 0.09f);
        return true;
    }
    if (is("info"))  {
        p.Circle(0.5f, 0.5f, 0.375f, 0.09f);
        p.Line(0.5f, 0.4375f, 0.5f, 0.6875f, 0.09f);
        p.Disc(0.5f, 0.3125f, 0.06f);
        return true;
    }
    // Отладка: кольцо с четырьмя засечками. Раньше засечек было три — знак
    // выглядел незаконченным.
    // Отладка: ЖУК — общепринятый знак, и в списке уровней консоли он ни с чем
    // не путается. Кольцо с четырьмя засечками, стоявшее здесь раньше, было
    // похоже на «настройки», «цель» и «отладку» одновременно.
    if (is("debug")) {
        // Без места на лапки силуэт приходится ДЕЛАТЬ КРУПНЕЕ: жук, у которого
        // отняли лапки и не добавили тела, на 12 пикселях превращается в точку
        // и пропадает среди галочек фильтра консоли.
        const float body = p.Fine() ? 0.21875f : 0.28125f;
        const float head = p.Fine() ? 0.09375f : 0.125f;
        p.Disc(0.5f, 0.5625f, body);              // туловище
        p.Disc(0.5f, 0.28125f, head);             // голова
        p.Line(0.5f, 0.34375f, 0.5f, 0.78125f, 0.05f);
        if (p.Fine()) {
            p.Line(0.28125f, 0.4375f, 0.09375f, 0.34375f, 0.055f);
            p.Line(0.71875f, 0.4375f, 0.90625f, 0.34375f, 0.055f);
            p.Line(0.28125f, 0.5625f, 0.0625f, 0.5625f, 0.055f);
            p.Line(0.71875f, 0.5625f, 0.9375f, 0.5625f, 0.055f);
            p.Line(0.28125f, 0.6875f, 0.09375f, 0.8125f, 0.055f);
            p.Line(0.71875f, 0.6875f, 0.90625f, 0.8125f, 0.055f);
            p.Line(0.4375f, 0.21875f, 0.34375f, 0.09375f, 0.05f);
            p.Line(0.5625f, 0.21875f, 0.65625f, 0.09375f, 0.05f);
        }
        return true;
    }

    // --- Действия ---
    if (is("trash")) {
        p.Rect(0.25f, 0.3125f, 0.75f, 0.875f, 0.075f, 0.05f);
        p.Line(0.1875f, 0.25f, 0.8125f, 0.25f, 0.085f);
        p.Line(0.4375f, 0.125f, 0.5625f, 0.125f, 0.075f);
        p.Line(0.4375f, 0.4375f, 0.4375f, 0.75f, 0.05f);
        p.Line(0.5625f, 0.4375f, 0.5625f, 0.75f, 0.05f);
        return true;
    }
    if (is("copy")) {
        p.Rect(0.125f, 0.125f, 0.625f, 0.625f, 0.075f, 0.05f);
        p.Rect(0.375f, 0.375f, 0.875f, 0.875f, 0.075f, 0.05f);
        return true;
    }
    if (is("save")) {
        p.Rect(0.125f, 0.125f, 0.875f, 0.875f, 0.075f, 0.05f);
        p.FillRect(0.375f, 0.125f, 0.625f, 0.375f);
        p.Rect(0.3125f, 0.5625f, 0.6875f, 0.875f, 0.06f);
        return true;
    }
    // Открыть: папка с ОТКИНУТОЙ передней стенкой — трапецией. Прежняя
    // отличалась от закрытой одной чертой поперёк, то есть в списке команд
    // «Открыть» и «Папка» были неразличимы.
    if (is("open")) {
        p.FillRect(0.0625f, 0.1875f, 0.375f, 0.3125f, 0.03f);
        p.Poly({0.0625f, 0.25f, 0.625f, 0.25f, 0.625f, 0.4375f}, false, 0.075f);
        p.Poly({0.0625f, 0.25f, 0.0625f, 0.8125f, 0.8125f, 0.8125f, 0.9375f, 0.4375f,
                0.1875f, 0.4375f}, true, 0.075f);
        return true;
    }
    if (is("plus")) {
        p.Line(0.5f, 0.1875f, 0.5f, 0.8125f, 0.115f);
        p.Line(0.1875f, 0.5f, 0.8125f, 0.5f, 0.115f);
        return true;
    }
    if (is("eye")) {
        p.Curve(0.125f, 0.5f, 0.3125f, 0.1875f, 0.6875f, 0.1875f, 0.875f, 0.5f, 0.075f);
        p.Curve(0.125f, 0.5f, 0.3125f, 0.8125f, 0.6875f, 0.8125f, 0.875f, 0.5f, 0.075f);
        p.Disc(0.5f, 0.5f, 0.125f);
        return true;
    }
    if (is("lock")) {
        p.Rect(0.25f, 0.4375f, 0.75f, 0.8125f, 0.085f, 0.0625f);
        p.Arc(0.5f, 0.4375f, 0.1875f, 0.5f, 1.0f, 0.075f);
        return true;
    }

    return false;
}

// Заглушка для незнакомого имени: пустой квадрат с диагональю. Заметно, но не
// ломает вёрстку — ровно как у иконок игрового интерфейса.
void Unknown(const Pen& p) {
    p.Rect(0.1875f, 0.1875f, 0.8125f, 0.8125f, 0.075f);
    p.Line(0.1875f, 0.1875f, 0.8125f, 0.8125f, 0.06f);
}

void DrawAt(ImDrawList* dl, ImVec2 pos, float size, const char* icon, ImU32 color) {
    // Целый размер и целая позиция: половина пикселя здесь сдвигает ВСЮ иконку
    // и сводит на нет округления внутри Pen.
    Pen pen{dl, ImVec2(std::floor(pos.x), std::floor(pos.y)), std::floor(size), color};
    if (!Shape(pen, icon)) Unknown(pen);
}

// Все имена — для страницы проверки и для тестов. Порядок сгруппирован так же,
// как формы выше.
const char* const kNames[] = {
    "play", "pause", "stop", "step",
    "move", "rotate", "scale", "universal", "rect", "align", "drop",
    "grid", "wire",
    "cube", "sphere", "light", "sun", "camera", "script", "particles", "anim", "ik", "probe",
    "network",
    "physics",
    "folder", "file", "scene", "material", "project", "prefab", "texture", "shader", "audio",
    "model",
    "up", "refresh", "folder-plus", "search", "code", "question", "layout", "gear",
    "magnet",
    "warn", "error", "info", "debug",
    "trash", "copy", "save", "open", "plus", "eye", "lock",
};

} // namespace

bool Has(const char* icon) {
    // Формы рисуются, а не перечисляются, поэтому «есть ли иконка» проверяется
    // прогоном по невидимому списку команд: ImDrawList с нулевым размером
    // ничего не рисует, а Shape честно возвращает, знает ли он имя.
    ImDrawList dl(ImGui::GetDrawListSharedData());
    dl.PushClipRectFullScreen();
    dl.PushTextureID(ImGui::GetIO().Fonts->TexID);
    Pen pen{&dl, ImVec2(0, 0), 0.0f, 0};
    return Shape(pen, icon);
}

int Count() { return (int)(sizeof(kNames) / sizeof(kNames[0])); }
const char* NameAt(int index) {
    return (index >= 0 && index < Count()) ? kNames[index] : nullptr;
}

// Совпадающие ID — В ЛОГ, а не только под курсор.
//
// ImGui умеет ловить два видимых элемента с одинаковым ID, но замечает это
// ТОЛЬКО когда мышь стоит на одном из них: проверка привязана к наведённому
// элементу. Поэтому дефект живёт в редакторе месяцами и всплывает у человека
// красным окном «Programmer error: 2 visible items with conflicting ID» ровно в
// тот момент, когда он навёл мышь на кнопку — и выглядит это как поломка
// редактора, а не как наша опечатка. А последствие настоящее: у двух кнопок с
// одним ID нажатие достаётся одной, и вторая просто не работает.
//
// Здесь проверка идёт ПО ФАКТУ ПОДАЧИ элемента, без всякого наведения: значит,
// её видит headless-прогон, и конфликт ловится в CI, а не глазами пользователя.
// Кнопки редактора почти все проходят через EditorIcons, а имя подписи сразу
// говорит, какие именно две кнопки столкнулись.
void CheckDuplicateId(const char* what) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window) return;
    const ImGuiID id = window->GetID("##btn_probe");
    static std::unordered_map<ImGuiID, int> seen;
    auto it = seen.find(id);
    if (it != seen.end() && it->second == g.FrameCount) {
        LOG_ERROR("Editor") << "одинаковый ID у двух элементов интерфейса: '" << what
                            << "' в окне '" << window->Name
                            << "' — нажатие достанется только одному из них "
                               "(нужен PushID или ##суффикс)";
    }
    seen[id] = g.FrameCount;
}

bool Button(const char* icon, const char* label, const char* tooltip, bool active) {
    const float h = ImGui::GetFrameHeight();
    const float icon_s = std::floor(h * 0.68f);
    ImGui::PushID(label);
    CheckDuplicateId(label);
    // Нажатое состояние — акцент ПОДЛОЖКОЙ, а не заливкой.
    //
    // Сплошной жёлтый на каждой включённой кнопке превращает тулбар в жёлтую
    // ленту: акцентом перестаёт быть что бы то ни было, потому что акцентом
    // становится всё. Подложка в 20 % и жёлтый значок читаются как «включено»
    // не хуже, а рядом стоящее главное действие (Играть) снова выделяется.
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(EditorTheme::Role::AccentMuted));
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Accent));
    }

    // Место под иконку резервируется отступом слева — так подпись не наезжает
    // на рисунок при любом шрифте.
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    const ImVec2 size(textSize.x + icon_s + pad.x * 3.0f, h);
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::Button("##btn", size);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    DrawAt(dl, ImVec2(cursor.x + pad.x, cursor.y + std::floor((h - icon_s) * 0.5f)), icon_s, icon,
           col);
    dl->AddText(ImVec2(cursor.x + pad.x * 2.0f + icon_s, cursor.y + (h - textSize.y) * 0.5f), col,
                label);

    if (active) ImGui::PopStyleColor(2);
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return pressed;
}

bool IconOnlyButton(const char* icon, const char* tooltip, bool active, const glm::vec3& tint) {
    const float h = ImGui::GetFrameHeight();
    ImGui::PushID(icon);
    CheckDuplicateId(icon);
    // Нажатое состояние — акцент ПОДЛОЖКОЙ, а не заливкой.
    //
    // Сплошной жёлтый на каждой включённой кнопке превращает тулбар в жёлтую
    // ленту: акцентом перестаёт быть что бы то ни было, потому что акцентом
    // становится всё. Подложка в 20 % и жёлтый значок читаются как «включено»
    // не хуже, а рядом стоящее главное действие (Играть) снова выделяется.
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(EditorTheme::Role::AccentMuted));
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Accent));
    }
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::Button("##ibtn", ImVec2(h, h));
    const float icon_s = std::floor(h * 0.64f);
    const float off = std::floor((h - icon_s) * 0.5f);
    DrawAt(ImGui::GetWindowDrawList(), ImVec2(cursor.x + off, cursor.y + off), icon_s, icon,
           Col(tint));
    if (active) ImGui::PopStyleColor(2);
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return pressed;
}

void Inline(const char* icon, const glm::vec3& color) {
    const float s = std::floor(ImGui::GetTextLineHeight());
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    DrawAt(ImGui::GetWindowDrawList(), ImVec2(cursor.x, cursor.y), s, icon, Col(color));
    // Место под иконку резервируется Dummy: без него следующий SameLine лёг бы
    // поверх рисунка, потому что ImGui о нарисованном напрямую не знает.
    ImGui::Dummy(ImVec2(s, s));
}

void Overlay(float x, float y, float size, const char* icon, const glm::vec3& color) {
    // Ни курсора, ни Dummy: рисунок ложится в список отрисовки окна, а «последний
    // элемент» ImGui остаётся тем, что подали до вызова.
    DrawAt(ImGui::GetWindowDrawList(), ImVec2(x, y), size, icon, Col(color));
}

void DrawSheet(bool* open) {
    ImGui::SetNextWindowSize(ImVec2(1000, 780), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(T("Icon sheet" "###IconSheet"), open)) {
        ImGui::End();
        return;
    }
    ImGui::TextUnformatted(T("Each icon at the sizes it is actually used at."));
    ImGui::Separator();

    // Все иконки на одном экране: 32 px показывает ЗАМЫСЕЛ (форму), 16 и 12 —
    // РАСТР, то есть то, каким его увидят в тулбаре и в списках. Обе стороны
    // нужны сразу: форма может быть хороша, а на 12 пикселях превращаться в
    // пятно — и наоборот.
    static const float kSizes[] = {32.0f, 16.0f, 12.0f};
    const int columns = 4;
    if (ImGui::BeginTable("icons", columns,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
        for (int i = 0; i < Count(); ++i) {
            if (i % columns == 0) ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const ImVec2 c = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
            float x = c.x + 6.0f;
            for (float size : kSizes) {
                DrawAt(dl, ImVec2(x, c.y + std::floor((36.0f - size) * 0.5f)), size, kNames[i], col);
                x += size + 12.0f;
            }
            ImGui::Dummy(ImVec2(104.0f, 36.0f));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(kNames[i]);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace EditorIcons
