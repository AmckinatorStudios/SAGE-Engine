#pragma once
#include <string>
#include <vector>

#include "sage/ui/UIAnchor.h"
#include "sage/ui/components/Interact.h"
#include "sage/ui/components/Layout.h"
#include "sage/ui/components/Visual.h"

// ---------------------------------------------------------------------------
// ИНТЕРФЕЙС ИГРЫ — отдельная подсистема движка. Один вход: этот заголовок.
//
// Устройство:
//   components/Layout.h   — где элемент стоит: Transform, Mask, Layout, Canvas, Group
//   components/Visual.h   — из чего он сделан: Fill, Label, Image, Bar, Icon
//   components/Interact.h — что он делает: Interactable, TextInput, Range, State
//   UI.h / UI.cpp         — математика раскладки и заготовки (этот файл)
//   UIRenderer.h          — рисование примитивов интерфейса
//   UISceneSystem.h       — обход сцены: отрисовка, ввод, попадание курсором
//   UIIcons.h             — векторные значки
//
// ЧЕМ ЭТО ОТЛИЧАЕТСЯ ОТ ПРЕЖНЕГО. Раньше весь интерфейс был ОДНИМ компонентом на
// сорок с лишним полей и перечислением видов (панель, надпись, картинка, полоса,
// значок, поле, галка, ползунок). Такой компонент
// нельзя расширить, не тронув его самого: каждый новый вид элемента — новое
// значение в перечислении, новые поля у ВСЕХ элементов и новая ветка в
// отрисовке. Нельзя и спросить у сцены «все элементы с картинкой» — только
// «все элементы» с проверкой поля у каждого.
//
// Теперь элемент — это НАБОР компонентов поверх обязательного Transform:
//   кнопка   = Transform + Fill + Label + Interactable
//   картинка = Transform + Image
//   шкала    = Transform + Fill + Bar (+ Icon + Label, если нужно)
//   список   = Transform + Fill + Mask + Layout (дети раскладываются сами)
// Свой вид элемента — свой компонент и своя система, без единой правки здесь.
// ---------------------------------------------------------------------------
namespace sage::ui {

// --- Раскладка -------------------------------------------------------------

// Итоговый прямоугольник элемента внутри родителя. Чистая математика: без ECS,
// без GL, юнит-тестируется целиком.
UIRect Resolve(const Transform& t, const UIRect& parent);

// То же с явным размером — для случая, когда ширину содержимого уже измерил
// шрифт (AutoWidth) или посчитала раскладка родителя.
UIRect Resolve(const Transform& t, const UIRect& parent, glm::vec2 size);

// Размер элемента с учётом растяжения (Stretch) и полей.
glm::vec2 ResolveSize(const Transform& t, const UIRect& parent);

// Множитель масштаба интерфейса для холста при данном размере экрана.
// 1.0 — «пиксель в пиксель».
float CanvasScale(const Canvas& canvas, glm::vec2 screen);

// Окно обрезки маски: прямоугольник элемента, сжатый её полями.
UIRect MaskWindow(const Mask& mask, const UIRect& rect);

// Пересечение двух окон обрезки (вложенные маски режут друг друга).
UIRect Intersect(const UIRect& a, const UIRect& b);

// Один элемент раскладки: что положили и куда легло.
struct LayoutSlot {
    glm::vec2 Size{0.0f, 0.0f}; // вход: желаемый размер (выход: с учётом StretchCross)
    glm::vec2 Pos{0.0f, 0.0f};  // выход: левый верхний угол в экранных координатах
};

// Раскладывает детей внутри контейнера. Порядок — тот, в каком их подали
// (вызывающий сортирует по Layer). Возвращает размер, занятый содержимым, —
// по нему контейнер с FitContent подгоняет себя.
glm::vec2 ApplyLayout(const Layout& layout, const UIRect& container,
                      std::vector<LayoutSlot>& slots);

// --- Заготовки --------------------------------------------------------------

// Готовый набор компонентов: что именно значит «кнопка» или «полоса».
//
// Заготовка — это ДАННЫЕ, а не код редактора: собрать кнопку одинаково должны и
// меню редактора, и скрипт, собирающий диалог на лету, и игра, запущенная без
// редактора вовсе.
struct Preset {
    std::string Name;
    Transform Xf;
    bool HasFill = false;
    Fill FillStyle;
    bool HasLabel = false;
    Label LabelStyle;
    bool HasImage = false;
    Image ImageStyle;
    bool HasBar = false;
    Bar BarStyle;
    bool HasInteractable = false;
    Interactable Interact;
    bool HasInput = false;
    TextInput Input;
    bool HasRange = false;
    Range RangeValue;
    bool HasLayout = false;
    Layout LayoutRule;
    bool HasMask = false;
    Mask MaskRule;

    // ДЕТИ — ОТДЕЛЬНЫЕ ОБЪЕКТЫ, а не «встроенная надпись».
    //
    // Раньше кнопка была подложкой С ТЕКСТОМ ВНУТРИ: Fill и Label на одной
    // сущности, и текст в ней двигался по правилам, зашитым в отрисовку
    // (значок сдвигает, галка сдвигает). Теперь кнопка — подложка, реагирующая
    // на мышь, а надпись на ней — ОБЪЕКТ внутри неё. Его видно в дереве, его
    // можно подвинуть, покрасить, заменить картинкой или убрать совсем; можно
    // добавить второй. Ровно этого от заготовок и ждут: они дают удобное
    // начало, а не неразбираемую деталь.
    //
    // Имя ребёнка — то, что человек увидит в дереве интерфейса.
    std::vector<Preset> Children;
};

// Имена заготовок — имена в коде и в сценах, они не переводятся.
const std::vector<std::string>& PresetNames();

// Заготовка по имени. nullptr — имени нет: молча отдать пустой элемент значило
// бы «кнопка создалась и не работает».
const Preset* FindPreset(const std::string& name);

} // namespace sage::ui
