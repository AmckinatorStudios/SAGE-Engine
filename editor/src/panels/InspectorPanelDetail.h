#pragma once
#include <string>

#include "sage/physics/PhysicsTypes.h"

class EditorHost;

// Общие мелочи секций инспектора объекта (InspectorPanel_*.cpp). Только для них.
namespace sage::editor::inspectordetail {

// Две строки пояснения одной подсказкой. Разбивать их на две всплывашки нечем —
// подсказка у элемента одна, а разносить смысл по разным полям значит заставить
// искать вторую половину.
inline std::string Hint2(const char* a, const char* b) { return std::string(a) + "\n" + b; }

void LayerSlider(EditorHost& host, sage::physics::LayerMask& mask);
void LayerMaskCombo(EditorHost& host, const char* label, sage::physics::LayerMask& mask);

} // namespace sage::editor::inspectordetail
