#pragma once
#include "sage/ui/UIAnchor.h"
#include "sage/ui/UIInteraction.h"
#include <entt/entt.hpp>

class Scene;
class UIRenderer;
struct UIElementComponent;

// ---------------------------------------------------------------------
// Система интерфейса сцены: рисует сущности с UIElementComponent (см.
// scene/Components.h) поверх кадра и отвечает на «что под точкой?».
//
//   • Вёрстка: якорь + отступ внутри РОДИТЕЛЯ (HierarchyComponent) — вложенные
//     панели ведут себя как контейнеры; корневые элементы якорятся к экрану.
//   • Порядок: корни и группы детей сортируются по Layer (стабильно, при
//     равенстве — по Id), дети рисуются поверх родителя.
//   • Маски: ClipChildren обрезает всё поддерево прямоугольником элемента
//     (аппаратные ножницы, вложенные маски пересекаются).
//
// Использование (рантайм/редактор):
//   ui.Begin(w, h);
//   sage::ui::DrawSceneUI(scene, ui, w, h);
//   ui.End();
// ---------------------------------------------------------------------
namespace sage::ui {

// Итоговый экранный прямоугольник элемента: якорь+отступ внутри parent.
// Чистая математика (юнит-тестируется без GL). Размер берётся из LayoutSize,
// если он посчитан (AutoWidth), иначе из Size.
UIRect ResolveElementRect(const UIElementComponent& e, const UIRect& parent);

// То же, но с явно заданным размером — вариант для отрисовки, где ширина
// содержимого уже измерена шрифтом.
UIRect ResolveElementRect(const UIElementComponent& e, const UIRect& parent, glm::vec2 size);

// Рисует весь UI сцены. Вызывать между ui.Begin() и ui.End().
void DrawSceneUI(Scene& scene, UIRenderer& ui, int screenW, int screenH);

// Один шаг интерактива: подсветка под курсором, нажатие, фокус, набор текста,
// перетаскивание ползунков. Вызывать РАНЬШЕ игровой логики кадра — по
// результату (UIInputResult) игра решает, доставать ли ей тот же щелчок.
//
// Отдельно от DrawSceneUI намеренно: рисовать интерфейс может и тот, кто не
// даёт ему ввод (превью в редакторе, скриншот), а обрабатывать ввод надо ровно
// один раз за кадр, даже если панелей с картинкой две.
UIInputResult UpdateSceneUI(Scene& scene, const UIInputState& input, int screenW, int screenH);

// Верхний видимый элемент под экранной точкой (x, y): id сущности или -1.
// Учитывает Layer/порядок отрисовки (возвращается тот, кто нарисован поверх)
// и маски (точка вне маски не попадает в её поддерево). Для кнопок: игра
// сама решает, что делать по клику (см. Lua-биндинг GetUIElementAt).
int HitTest(Scene& scene, float x, float y, int screenW, int screenH);

} // namespace sage::ui
