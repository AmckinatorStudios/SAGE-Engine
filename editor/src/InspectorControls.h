#pragma once
#include <cfloat>
#include <functional>

#include <glm/glm.hpp>

class EditorHost;

// ---------------------------------------------------------------------------
// ЕДИНЫЕ КОНТРОЛЫ ИНСПЕКТОРА: одинаковые значения правятся одинаково везде.
//
// ТРИ УРОВНЯ: раздел (BeginSection — карточка со своей шапкой), группа
// (GroupTitle — подпись над связанными значениями: Position, Size, Range) и
// контрол. Подписи полей — СЛЕВА, в одной колонке (Label), значения — справа;
// связанные значения — в одной строке (X и Y, W и H, Min и Max).
//
// Числовое поле — [−][ 120 ][+]: ручной ввод (двойной щелчок), перетаскивание,
// колесо мыши над полем, удержание кнопок (с ускорением), пределы и шаг поля,
// сброс к исходному (правый щелчок). Ширина — по типу значения, а не во всю
// панель.
//
// Правки идут в историю: перетаскивание — одним шагом (TrackLastImGuiItem),
// кнопки и колесо — снимком перед изменением.
// ---------------------------------------------------------------------------
namespace sage::editor::ictl {

// СВОЯ ИСТОРИЯ ПРАВОК. Контролы по умолчанию пишут в историю сцены (host);
// инструмент со своими данными (редактор спрайтов) подменяет её на время
// отрисовки своих полей: снимок — его функцией.
struct UndoScope {
    explicit UndoScope(std::function<void()> push);
    ~UndoScope();
    UndoScope(const UndoScope&) = delete;
    UndoScope& operator=(const UndoScope&) = delete;

private:
    std::function<void()> m_prev;   // вложенная область возвращает внешнюю
};

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

// --- Строка: подпись слева, контрол справа --------------------------------
float LabelWidth();
// Подпись в колонке подписей; следующий контрол встаёт справа от неё и
// получает оставшуюся ширину (SetNextItemWidth).
void Label(const char* text);
// Ширина контрола числа по типу значения (не растягивать без причины).
float NumberWidth();

// Ядро: [−][значение][+] шириной width (<=0 — NumberWidth()). Без подписи.
bool NumberCore(EditorHost& host, const char* id, float* v, const NumOpts& o, float width = 0.0f);

// Число в строке с подписью слева.
bool Number(EditorHost& host, const char* label, float* v, const NumOpts& o);
bool Integer(EditorHost& host, const char* label, int* v, int min, int max, bool hasReset = false, int reset = 0);

// Ползунок с точным полем — для ограниченных диапазонов (прозрачность 0..1,
// значение шкалы в её Min..Max).
bool SliderNumber(EditorHost& host, const char* label, float* v, float min, float max, const char* format = "%.2f",
                  bool hasReset = false, float reset = 0.0f);

// Переключатель вкл/выкл: подпись слева, «таблетка» справа.
bool Switch(EditorHost& host, const char* label, bool* v);

// Цвет одной строкой: [■ образец][#HEX][85%]. Образец открывает палитру.
bool Color(EditorHost& host, const char* label, glm::vec4* rgba);

// Подпись группы связанных значений («Position», «Size»).
void GroupTitle(const char* title);

// Пара значений в одной строке: «X [−620+]   Y [−40+]».
bool Pair(EditorHost& host, const char* id, float* a, float* b, const char* tagA, const char* tagB,
          const NumOpts& o);

// Оси построчно по две: подписи осей — n штук (2 или 4).
bool Axes(EditorHost& host, const char* id, float* v, int n, const char* const* axes, const NumOpts& o);

// Position — X и Y в одной строке.
bool Position(EditorHost& host, const char* title, glm::vec2* v, const NumOpts& o);

// Связь пропорций: правили одну сторону (now против before) — вторая
// следует за ней в прежнем соотношении. Без прежнего соотношения — как есть.
glm::vec2 LinkedSize(glm::vec2 before, glm::vec2 now, const NumOpts& o);

// Size — W и H в одной строке, замок связи пропорций в подписи группы.
bool Size(EditorHost& host, const char* title, glm::vec2* v, const NumOpts& o);

// Range — Min и Max в одной строке.
bool Range(EditorHost& host, const char* title, float* min, float* max, const NumOpts& o);

// Стороны (Margin, Padding): L/T/R/B двумя строками, со связью — одно число.
bool Sides(EditorHost& host, const char* title, glm::vec4* v, const NumOpts& o);

// РАЗДЕЛ: карточка со своей шапкой (акцентная метка, заглавные буквы,
// стрелка справа). Раскрытие запоминается между запусками. true — раскрыт:
// тогда обязателен EndSection().
bool BeginSection(const char* id, const char* title, bool defaultOpen = true);
void EndSection();

// Вложенная раскрывашка внутри раздела («▸ Advanced»): лёгкая, без карточки.
bool SubSection(const char* id, const char* title, bool defaultOpen = false);
void EndSubSection();

} // namespace sage::editor::ictl
