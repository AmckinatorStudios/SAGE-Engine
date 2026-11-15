#pragma once
#include <cfloat>

#include <glm/glm.hpp>

class EditorHost;

// ---------------------------------------------------------------------------
// ЕДИНЫЕ КОНТРОЛЫ ИНСПЕКТОРА: одинаковые значения правятся одинаково везде.
//
// Числовое поле — [−][ 120 ][+]: ручной ввод (двойной щелчок), перетаскивание
// по полю, колесо мыши над полем, удержание кнопок (с ускорением), пределы и
// шаг поля, сброс к исходному (правый щелчок по полю). Вектор — строками с
// подписанными осями (X/Y, Width/Height, L/T/R/B), а не безымянным полем на
// четыре числа, в котором порядок приходится помнить.
//
// Правки идут в историю: перетаскивание — одним шагом (TrackLastImGuiItem),
// кнопки и колесо — снимком перед изменением.
// ---------------------------------------------------------------------------
namespace sage::editor::ictl {

struct NumOpts {
    float Step = 1.0f;              // шаг кнопок и колеса; перетаскивание — его четверть на пиксель
    float Min = -FLT_MAX;
    float Max = FLT_MAX;
    const char* Format = "%.0f";
    bool HasReset = false;          // правый щелчок по полю возвращает Reset
    float Reset = 0.0f;
};

// Шаг и формат по диапазону поля: координаты — по 1, доли — по 0.01.
NumOpts OptsForRange(float min, float max);

// Ядро: [−][значение][+] шириной width (<=0 — всё доступное). Без подписи.
bool NumberCore(EditorHost& host, const char* id, float* v, const NumOpts& o, float width = 0.0f);

// Число с подписью справа — как у остальных полей инспектора.
bool Number(EditorHost& host, const char* label, float* v, const NumOpts& o);
bool Integer(EditorHost& host, const char* label, int* v, int min, int max, bool hasReset = false, int reset = 0);

// Ползунок с точным полем рядом — для небольших ограниченных диапазонов
// (прозрачность 0..1, толщина 0..1).
bool SliderNumber(EditorHost& host, const char* label, float* v, float min, float max, const char* format = "%.2f",
                  bool hasReset = false, float reset = 0.0f);

// Подзаголовок группы значений: «POSITION», «SIZE».
void GroupTitle(const char* title);

// Оси построчно: «X [−][120][+]». axes — подписи осей (n штук).
bool Axes(EditorHost& host, const char* id, float* v, int n, const char* const* axes, const NumOpts& o);

// Position — X и Y.
bool Position(EditorHost& host, const char* title, glm::vec2* v, const NumOpts& o);

// Связь пропорций: правили одну сторону (now против before) — вторая
// следует за ней в прежнем соотношении. Без прежнего соотношения — как есть.
glm::vec2 LinkedSize(glm::vec2 before, glm::vec2 now, const NumOpts& o);

// Size — отдельно Width/Height или вместе «240 × 80», и связь пропорций.
// Режим показа общий для инспектора (запоминается), связь — у поля.
bool Size(EditorHost& host, const char* title, glm::vec2* v, const NumOpts& o);

// Стороны (Margin, Padding): L/T/R/B, со связью — правка одной меняет все.
bool Sides(EditorHost& host, const char* title, glm::vec4* v, const NumOpts& o);

// РАЗДЕЛ ИНСПЕКТОРА: спокойный заголовок без цветной плашки, раскрытие
// запоминается между запусками (по id). Advanced — закрыт по умолчанию.
bool Section(const char* id, const char* title, bool defaultOpen = true);

} // namespace sage::editor::ictl
