#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include <glm/glm.hpp>

#include "sage/ui/UIAnchor.h" // UIRect — общий тип прямоугольника интерфейса

// ---------------------------------------------------------------------------
// БАЗОВЫЕ ТИПЫ НОВОЙ СИСТЕМЫ ИНТЕРФЕЙСА.
//
// Здесь нет ни одного «элемента интерфейса» — только величины, которыми
// интерфейс описывается: прямоугольник, поля со всех четырёх сторон, четыре
// независимых радиуса, цвет, режим наложения, единица измерения. Всё остальное
// (узел, раскладка, оформление, маска, эффект, рисование, ввод) построено
// поверх этого файла и НИЧЕГО о соседях не знает.
//
// ПОЧЕМУ ОТДЕЛЬНЫМ ФАЙЛОМ. Прежняя система описывала поля отступов четырьмя
// разными способами (vec4 «л,в,п,н» у маски, vec4 «л,в,п,н» у раскладки, две
// отдельные числовые пары у текста), и каждый читающий должен был помнить, в
// каком порядке лежат стороны у ЭТОГО поля. Одна структура с именованными
// сторонами дешевле любой договорённости: перепутать L и T невозможно.
// ---------------------------------------------------------------------------
namespace sage::ui {

// --- Прямоугольник ----------------------------------------------------------
//
// Сам тип (UIRect) объявлен в UIAnchor.h и общий со старой системой: два
// несовместимых «прямоугольника» в одном движке — гарантированная путаница на
// границе старого и нового кода.
inline float UIRight(const UIRect& r) { return r.x + r.w; }
inline float UIBottom(const UIRect& r) { return r.y + r.h; }
inline glm::vec2 UIPos(const UIRect& r) { return {r.x, r.y}; }
inline glm::vec2 UISize(const UIRect& r) { return {r.w, r.h}; }
inline glm::vec2 UICenter(const UIRect& r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
inline bool UIRectValid(const UIRect& r) { return r.w > 0.0f && r.h > 0.0f; }

inline bool UIContains(const UIRect& r, glm::vec2 p) {
    return p.x >= r.x && p.y >= r.y && p.x <= r.x + r.w && p.y <= r.y + r.h;
}

// Пересечение. Пустой результат (w или h <= 0) — законный ответ «не пересеклись»,
// а не ошибка: вложенные маски, ушедшие друг за друга, обязаны давать пустоту.
inline UIRect UIIntersectRect(const UIRect& a, const UIRect& b) {
    const float x0 = std::max(a.x, b.x);
    const float y0 = std::max(a.y, b.y);
    const float x1 = std::min(UIRight(a), UIRight(b));
    const float y1 = std::min(UIBottom(a), UIBottom(b));
    return UIRect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

// Объединение (охватывающий прямоугольник). Нужен подгонке по содержимому и
// вычислению границ поддерева.
inline UIRect UIUnionRect(const UIRect& a, const UIRect& b) {
    if (!UIRectValid(a)) return b;
    if (!UIRectValid(b)) return a;
    const float x0 = std::min(a.x, b.x);
    const float y0 = std::min(a.y, b.y);
    const float x1 = std::max(UIRight(a), UIRight(b));
    const float y1 = std::max(UIBottom(a), UIBottom(b));
    return UIRect{x0, y0, x1 - x0, y1 - y0};
}

// --- Поля со всех сторон ----------------------------------------------------
//
// Одно и то же понятие для Padding, Margin и толщины рамки: стороны названы, а
// не пронумерованы. Сокращённые формы (§13 ТЗ) — конструкторы, а не три разных
// поля: «одинаково со всех сторон» и «по горизонтали/вертикали» это не другой
// вид отступа, а другой способ его записать.
struct UIEdges {
    float L = 0.0f, T = 0.0f, R = 0.0f, B = 0.0f;

    UIEdges() = default;
    UIEdges(float l, float t, float r, float b) : L(l), T(t), R(r), B(b) {}

    static UIEdges Uniform(float v) { return UIEdges(v, v, v, v); }
    static UIEdges Axis(float horizontal, float vertical) {
        return UIEdges(horizontal, vertical, horizontal, vertical);
    }

    float Horizontal() const { return L + R; }
    float Vertical() const { return T + B; }
    bool Empty() const { return L == 0.0f && T == 0.0f && R == 0.0f && B == 0.0f; }

    bool operator==(const UIEdges& o) const {
        return L == o.L && T == o.T && R == o.R && B == o.B;
    }
    bool operator!=(const UIEdges& o) const { return !(*this == o); }
};

// Сжать прямоугольник полями (никогда не в отрицательный размер: контейнер с
// отступами больше себя — это не ошибка вёрстки, а пустое место).
inline UIRect UIDeflate(const UIRect& r, const UIEdges& e) {
    const float w = std::max(0.0f, r.w - e.Horizontal());
    const float h = std::max(0.0f, r.h - e.Vertical());
    return UIRect{r.x + e.L, r.y + e.T, w, h};
}
inline UIRect UIInflate(const UIRect& r, const UIEdges& e) {
    return UIRect{r.x - e.L, r.y - e.T, r.w + e.Horizontal(), r.h + e.Vertical()};
}

// --- Скругления -------------------------------------------------------------
//
// ЧЕТЫРЕ НЕЗАВИСИМЫХ РАДИУСА, а не один на весь прямоугольник (§21). Карточка
// со скруглённым верхом и прямым низом, вкладка, всплывающая подсказка с
// «хвостиком» — всё это одним числом не описывается вовсе.
struct UICorners {
    float TL = 0.0f, TR = 0.0f, BR = 0.0f, BL = 0.0f;

    UICorners() = default;
    explicit UICorners(float v) : TL(v), TR(v), BR(v), BL(v) {}
    UICorners(float tl, float tr, float br, float bl) : TL(tl), TR(tr), BR(br), BL(bl) {}

    bool Empty() const { return TL <= 0.0f && TR <= 0.0f && BR <= 0.0f && BL <= 0.0f; }
    float Max() const { return std::max(std::max(TL, TR), std::max(BR, BL)); }

    // Радиусы не могут быть больше половины стороны: иначе соседние скругления
    // «съедают» друг друга и фигура выворачивается наизнанку. Ограничение
    // делает ОДНО место — здесь, а не каждый рисующий по-своему.
    UICorners Clamped(float w, float h) const {
        const float m = 0.5f * std::min(w, h);
        UICorners c;
        c.TL = std::max(0.0f, std::min(TL, m));
        c.TR = std::max(0.0f, std::min(TR, m));
        c.BR = std::max(0.0f, std::min(BR, m));
        c.BL = std::max(0.0f, std::min(BL, m));
        return c;
    }

    bool operator==(const UICorners& o) const {
        return TL == o.TL && TR == o.TR && BR == o.BR && BL == o.BL;
    }
    bool operator!=(const UICorners& o) const { return !(*this == o); }
};

// --- Цвет -------------------------------------------------------------------
//
// Обычный vec4 (RGBA, 0..1). Своего типа цвета нет намеренно: он тут же
// потребовал бы арифметики, преобразований и перегрузок, а выигрыш нулевой —
// GLM всё это уже умеет.
using UIColor = glm::vec4;

inline UIColor UIRgba(float r, float g, float b, float a = 1.0f) { return {r, g, b, a}; }
inline UIColor UIWithAlpha(const UIColor& c, float a) { return {c.r, c.g, c.b, a}; }
// Цвет из #RRGGBB / #RRGGBBAA — единственный формат, в котором цвета пишут люди
// (темы, документация, дизайн-макеты).
UIColor UIColorFromHex(const std::string& hex, const UIColor& fallback = UIColor(1.0f));
std::string UIColorToHex(const UIColor& c);

// --- Режимы наложения (§29) -------------------------------------------------
//
// Перечисление здесь — не «switch на всё», а СЛОВАРЬ: система рисования знает,
// какие состояния GPU соответствуют каждому режиму, и добавление нового режима
// не трогает ни дерево, ни раскладку, ни ввод.
enum class UIBlendMode {
    Normal,
    Add,
    Multiply,
    Screen,
    Overlay,
    Darken,
    Lighten,
};

const char* const* UIBlendModeNames();
int UIBlendModeCount();

// --- Единица измерения ------------------------------------------------------
//
// Размер задаётся числом И тем, что это число значит. Без этого «ширина 50»
// приходится читать как пиксели всегда, и «половина родителя» выразить нечем
// (§11).
enum class UIUnit {
    Pixels,   // ровно столько логических пикселей холста
    Percent,  // доля соответствующей стороны родителя
    Content,  // столько, сколько занимает содержимое
    Stretch,  // от края до края области якорей минус поля
};

const char* const* UIUnitNames();
int UIUnitCount();

// --- Мелкие помощники -------------------------------------------------------
inline float UILerp(float a, float b, float t) { return a + (b - a) * t; }
inline float UIClamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline bool UINearlyEqual(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

} // namespace sage::ui
