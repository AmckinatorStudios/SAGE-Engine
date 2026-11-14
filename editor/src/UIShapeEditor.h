#pragma once
#include <functional>

#include "imgui.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "sage/ui/UIAnchor.h"

class EditorHost;
class Scene;

// ---------------------------------------------------------------------------
// ПРАВКА СВОЕГО КОНТУРА НА ХОЛСТЕ (Shape::Geometry::Custom).
//
// Форму правят там, где её видно: точки и ручки кривых — прямо на холсте,
// поверх элемента. Путь (Shape::Path) — источник правды; редактор его
// разбирает, меняет и записывает обратно, одним шагом отмены на жест.
//
//   • тащить точку — двигать её; тащить ручку — менять кривую (Alt — только
//     эту ручку, без зеркала);
//   • двойной щелчок по линии — новая точка;
//   • правая кнопка по точке — угол/кривая, удалить точку, замкнуть или
//     разомкнуть контур, удалить контур;
//   • Delete — удалить выбранную точку.
//
// Холст отдаёт мышь сюда РАНЬШЕ рамок и ручек размера: точка контура часто
// лежит на краю элемента, и щелчок по ней обязан выигрывать.
// ---------------------------------------------------------------------------
class UIShapeEditor {
public:
    // true — мышь занята правкой контура (холст остальное не разбирает).
    bool Draw(EditorHost& host, Scene& scene, ImDrawList* dl, entt::entity element,
              const sage::ui::UIRect& rect, const std::function<ImVec2(float, float)>& toScreen,
              const std::function<glm::vec2(ImVec2)>& toUI, ImVec2 mouse, bool hovered);

private:
    enum class Grab { None, Point, In, Out };
    entt::entity m_entity = entt::null;
    int m_contour = -1, m_point = -1;     // выбранная точка
    Grab m_grab = Grab::None;
    bool m_pushedUndo = false;
    int m_menuContour = -1, m_menuPoint = -1;   // точка под меню правой кнопки
};
