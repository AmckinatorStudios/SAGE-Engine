#pragma once
#include <string>

#include <glm/glm.hpp>

#include "sage/physics/PhysicsTypes.h"

class EditorHost;
class GameObject;

// Общие мелочи секций инспектора объекта (InspectorPanel_*.cpp). Только для них.
namespace sage::editor::inspectordetail {

// Две строки пояснения одной подсказкой. Разбивать их на две всплывашки нечем —
// подсказка у элемента одна, а разносить смысл по разным полям значит заставить
// искать вторую половину.
inline std::string Hint2(const char* a, const char* b) { return std::string(a) + "\n" + b; }

void LayerSlider(EditorHost& host, sage::physics::LayerMask& mask);
void LayerMaskCombo(EditorHost& host, const char* label, sage::physics::LayerMask& mask);

// Раздел Pivot внутри Transform (InspectorPanel_Pivot.cpp).
void DrawPivotSection(EditorHost& host, GameObject obj);
// Готовое положение опоры по габаритам: at — доли коробки, −1…+1 по осям.
glm::vec3 PivotPreset(const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& at);

} // namespace sage::editor::inspectordetail
