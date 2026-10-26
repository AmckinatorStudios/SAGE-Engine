#pragma once
#include <glm/glm.hpp>

#include "gizmo/Handles.h"
#include "sage/render/DebugDraw.h"
#include "sage/scene/Components.h"

// ---------------------------------------------------------------------------
// ГИЗМО СВЕТА — СВОЁ У КАЖДОГО ВИДА, а не одна сфера на всех.
//
// По виду значка должно быть понятно, ЧТО это за свет и КУДА он светит, ещё до
// того, как человек откроет инспектор:
//   • направленный (солнце) — диск и пучок параллельных лучей вдоль света;
//   • точечный — маленькая «звёздочка» из трёх колец; у выделенного — сфера
//     дальности тремя окружностями;
//   • прожектор — конус: внешний край ярко, внутренний (полная яркость)
//     бледнее, дальность — основанием конуса;
//   • площадной — сама панель с нормалью-стрелкой: светит одной стороной.
// У выделенного — ручки: дальность (точечный, прожектор), угол конуса
// (прожектор), ширина и высота (площадной). Направление и положение — обычным
// гизмо поворота и переноса, второй ручки для них нет.
// ---------------------------------------------------------------------------
namespace sage::editor {

namespace lightedit {
// Новые значения по ходу ручки (мировые единицы вдоль её оси).
float Range(float start, float worldDelta);
// Прожектор: угол по радиусу основания на дальности range. Внутренний угол не
// больше внешнего — иначе «полная яркость» шире самого конуса.
void SpotAngle(const LightComponent& start, float rimRadius, LightComponent& out);
float AreaSide(float start, float worldDelta);   // ручка на краю: размер растёт на 2×ход
} // namespace lightedit

// Значок света в сцене (линиями DebugDraw).
void DrawLightGizmo(DebugDraw& dd, const LightComponent& light, const glm::mat4& world, bool selected);

class LightHandles {
public:
    struct Result {
        bool WantMouse = false;
        bool Began = false;
        bool Changed = false;
        bool Ended = false;
    };
    Result Update(LightComponent& light, const glm::mat4& world, const handles::View& view, bool allowInput);
    bool Dragging() const { return m_active >= 0; }

private:
    enum Id { kRange = 0, kAngle = 1, kWidthPos = 2, kWidthNeg = 3, kHeightPos = 4, kHeightNeg = 5 };
    int m_active = -1;
    LightComponent m_start;
    glm::vec3 m_startWorld{0.0f};
    glm::vec3 m_axis{0.0f};
    float m_startParam = 0.0f;
};

} // namespace sage::editor
