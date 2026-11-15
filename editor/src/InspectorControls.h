#pragma once
#include <cfloat>
#include <functional>
#include <string>

#include <glm/glm.hpp>

class EditorHost;

// ---------------------------------------------------------------------------
// EDITOR UI KIT — ЕДИНЫЕ КОНТРОЛЫ РЕДАКТОРА: одинаковые значения правятся
// одинаково везде (инспектор сцены, инспектор интерфейса, окна инструментов).
//
// ГЛАВНОЕ ПРАВИЛО: одна функция — один цельный контрол. Кнопки −/+, стрелка
// списка и кнопки одной операции живут ВНУТРИ общей рамки, разделённые
// тонкими линиями, а не стоят рядом отдельными квадратами. Вектор — поле на
// ось: каждая ось правится сама по себе, и у каждой своя рамка.
//
// Размеры, отступы и цвета состояний — EditorTheme::Kit/State: поменял там —
// поменялись все контролы. Вариант вида (Standard/Compact/…) выбирают при
// вызове; ввод, история (Undo/Redo) и сброс у вариантов общие.
//
// Модули: InspectorControls.cpp — числа, переключатель, цвет;
// _Vector.cpp — векторы, пары, размер, стороны; _Choice.cpp — список, группа
// кнопок, кнопка; _Layout.cpp — подпись, группа, разделитель, разделы.
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

// --- Варианты вида -----------------------------------------------------------
enum class NumLook { Standard, Compact };              // Compact — без −/+
enum class VecLook { Segmented, Compact, Stacked };    // узко — сам переходит дальше
enum class DropLook { Standard, Compact };             // Compact — по ширине текста
enum class SwitchLook { Standard, Inline };            // Inline — без колонки подписей
enum class ButtonLook { Standard, Compact, IconOnly };
enum class ColorLook { Standard, Compact };            // Compact — только образец

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
// Подпись «Имя:» в колонке подписей; следующий контрол встаёт справа от неё и
// получает оставшуюся ширину (SetNextItemWidth). Подпись, не влезающая в
// колонку, или поле уже minField — подпись НАД полем, целиком.
void Label(const char* text, float minField = 0.0f);
// Ширина контрола числа по типу значения (не растягивать без причины).
float NumberWidth();

// Ядро: [−|значение|+] шириной width (<=0 — NumberWidth()). Без подписи.
// Compact (или узкое поле) — без кнопок.
bool NumberCore(EditorHost& host, const char* id, float* v, const NumOpts& o, float width = 0.0f,
                NumLook look = NumLook::Standard);

// Число в строке с подписью слева.
bool Number(EditorHost& host, const char* label, float* v, const NumOpts& o, NumLook look = NumLook::Standard);
bool Integer(EditorHost& host, const char* label, int* v, int min, int max, bool hasReset = false, int reset = 0);

// Ползунок с точным полем — для ограниченных диапазонов (прозрачность 0..1,
// значение шкалы в её Min..Max).
bool SliderNumber(EditorHost& host, const char* label, float* v, float min, float max, const char* format = "%.2f",
                  bool hasReset = false, float reset = 0.0f);

// Переключатель вкл/выкл: подпись слева, «таблетка» справа (Standard) или
// «таблетка» и подпись рядом, без колонки подписей (Inline).
bool Switch(EditorHost& host, const char* label, bool* v, SwitchLook look = SwitchLook::Standard);

// Замена ImGui::Checkbox: переключатель с подписью справа (вид Inline), без
// истории — для мест, где снимок делает вызывающий или правка не в сцене.
bool Check(const char* label, bool* v);

// Цвет одной строкой: [■ образец][#HEX][85%] (Standard) или только образец
// (Compact). Образец открывает палитру.
bool Color(EditorHost& host, const char* label, glm::vec4* rgba, ColorLook look = ColorLook::Standard);
bool Color3(EditorHost& host, const char* label, glm::vec3* rgb, ColorLook look = ColorLook::Compact);

// Подпись группы связанных значений («Position», «Size»).
void GroupTitle(const char* title);

// Пара значений в одной строке: «X [−620+]   Y [−40+]».
bool Pair(EditorHost& host, const char* id, float* a, float* b, const char* tagA, const char* tagB,
          const NumOpts& o);

// Пара в строке с подписью: «Позиция: X [..] Y [..]».
bool PairRow(EditorHost& host, const char* label, float* a, float* b, const char* tagA, const char* tagB,
             const NumOpts& o);

// Оси построчно по две: подписи осей — n штук (2 или 4).
bool Axes(EditorHost& host, const char* id, float* v, int n, const char* const* axes, const NumOpts& o);

// VECTOR2/3/4 — поле на ось: X, Y, Z (W) — самостоятельные числовые поля,
// каждое своей рамкой, одной ширины, через небольшой отступ (Kit::AxisGap), в
// одну строку; в поле — цветная метка оси, число и компактные −/+.
// Узко: Segmented (в строку, с −/+) → Compact (в строку, без −/+) → Stacked
// (ось на строку).
struct VecOpts {
    VecLook Look = VecLook::Segmented;
    const char* const* Tags = nullptr;  // подписи полей осей; nullptr — X/Y/Z/W
    bool AxisColors = true;             // метки цветом осей (X красная…); иначе нейтральные
    float Trailing = 0.0f;              // место справа под свои кнопки (замок, сброс)
};
bool VectorN(EditorHost& host, const char* label, float* v, int n, const NumOpts& o, const VecOpts& vo = {});
bool Vector2(EditorHost& host, const char* label, glm::vec2* v, const NumOpts& o, const VecOpts& vo = {});
bool Vector4(EditorHost& host, const char* label, glm::vec4* v, const NumOpts& o, const VecOpts& vo = {});
// Тот же вектор без подписи — во всю оставшуюся ширину строки.
bool VectorCore(EditorHost& host, const char* id, float* v, int n, const NumOpts& o, const VecOpts& vo = {});
// Замена ImGui::DragFloat2/3/4 (те же аргументы + host первым): вектор kit с
// подписью слева. История правок — внутри (через host или UndoScope; host
// может быть nullptr), свой TrackLastImGuiItem после не нужен.
bool DragVec(EditorHost* host, const char* label, float* v, int n, float speed, float min = 0.0f, float max = 0.0f,
             const char* format = "%.3f");
// Раскладка строки полей (для проверок): отступ между полями и ширина поля
// оси при ширине строки width.
float VecAxisGap();
float VecFieldWidth(float width, int n);
// Какой вид встанет в ширину width (чистая функция: проверяется тестом).
VecLook FitVecLook(VecLook wanted, float width, int n, float tagW, float h);

// VECTOR3 — «Позиция: [X ..] [Y ..] [Z ..]»: метка оси цветом (X красная,
// Y зелёная, Z синяя) — часть своего поля; три поля одной ширины в строке.
// trailing — место справа под свои кнопки (замок, сброс), одинаковое у
// соседних строк, чтобы столбцы X/Y/Z совпадали.
// Узко — подпись над полями.
bool Vector3(EditorHost& host, const char* label, glm::vec3* v, const NumOpts& o, float trailing = 0.0f,
             VecLook look = VecLook::Segmented);

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

// --- Выбор и действия ---------------------------------------------------------
//
// ВЫПАДАЮЩИЙ СПИСОК — одно цельное поле: значение внутри, шеврон Tabler в
// правой части того же поля, нажимается всё поле. Ширина — SetNextItemWidth
// или вся строка (Compact — по тексту). true — список раскрыт: пункты, затем
// EndDropdown().
bool BeginDropdown(const char* id, const char* preview, DropLook look = DropLook::Standard);
void EndDropdown();
// Замена ImGui::Combo: те же аргументы, вид kit. Видимая подпись (без «##»)
// встаёт слева, в колонку подписей.
bool Combo(const char* id, int* current, const char* const* items, int count, DropLook look = DropLook::Standard);
// Список из готовых пунктов в строке с подписью; true — выбран другой.
bool Dropdown(const char* label, int* current, const char* const* items, int count,
              DropLook look = DropLook::Standard);

// ГРУППА КНОПОК одной операции — общая рамка, равные сегменты, тонкие
// разделители; у каждого сегмента своё наведение и нажатое состояние.
struct GroupItem {
    const char* Icon = nullptr;     // значок Tabler (EditorIcons)
    const char* Text = nullptr;     // подпись (IconOnly — не показывается)
    const char* Tooltip = nullptr;
    bool Active = false;            // включён/выбран
    bool Enabled = true;
};
// columns > 0 — сеткой по columns в ряд (якоря 3×3). width <= 0 — по
// содержимому (IconOnly — квадраты высотой с поле). -1 — ничего не нажато.
int ButtonGroup(const char* id, const GroupItem* items, int count, ButtonLook look = ButtonLook::IconOnly,
                int columns = 0, float width = 0.0f);
// Одиночная кнопка того же вида (группа из одной кнопки).
bool Button(const char* icon, const char* text, const char* tooltip = nullptr, ButtonLook look = ButtonLook::Standard,
            float width = 0.0f, bool active = false);

// --- Компоновка -----------------------------------------------------------------
// Разделитель: горизонтальный — линия через всю ширину с отступами kit;
// вертикальный — в строке, высотой с поле (между группами одной строки).
void Separator(bool vertical = false);
// Подпись части и тонкая линия до края («Exposure ———»).
void SeparatorText(const char* text);
// Группа связанных контролов: подпись группы (если есть) и общий отступ после.
void BeginGroup(const char* title = nullptr);
void EndGroup();

// Подпись раздела ЗАГЛАВНЫМИ (и кириллицей), без «###id».
std::string UpperTitle(const char* title);

// РАЗДЕЛ: шапка (нейтральная подложка, значок, заглавные буквы, шеврон
// справа). Раскрытие запоминается между запусками. true — раскрыт:
// тогда обязателен EndSection().
bool BeginSection(const char* id, const char* title, bool defaultOpen = true, const char* icon = nullptr);
void EndSection();

// Шапка раздела — ОДНА на весь редактор (и BeginSection, и
// EditorTheme::SectionHeader): значок, заголовок, шеврон, справа действия
// «сброс» и «удалить», когда они есть (не nullptr). *open переключается
// щелчком по шапке; true — щёлкнули по шапке.
struct HeaderActions {
    bool* Reset = nullptr;
    bool* Remove = nullptr;
    const char* Hint = nullptr;
};
bool SectionHeader(const char* id, const char* icon, const char* title, bool* open, const HeaderActions& a = {});

// Вложенная группа внутри раздела («▸ Коробка», «▸ Дополнительно»): строка-
// заголовок и содержимое с единым отступом и линией слева. Раскрытие
// запоминается между запусками.
bool SubSection(const char* id, const char* title, bool defaultOpen = false);
void EndSubSection();

} // namespace sage::editor::ictl
