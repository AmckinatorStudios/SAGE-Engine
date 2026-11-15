#include "InspectorFields.h"

#include <cmath>

#include "imgui.h"

#include "EditorHost.h"
#include "InspectorControls.h"
#include "ui/ColorPicker.h"

namespace ictl = sage::editor::ictl;

namespace sage::editor::ifield {

// Каждое поле — ГРУППОЙ: «последний элемент» для ImGui — вся строка, и
// правый щелчок «сбросить» (ResetField) и подсказка (Hint) после поля
// относятся к строке целиком, а не к кнопке «+».
struct Row {
    Row() { ImGui::BeginGroup(); }
    ~Row() { ImGui::EndGroup(); }
};

bool Float(EditorHost& host, const char* label, float* v, float speed, float min, float max, const char* format) {
    Row row;
    ictl::NumOpts o;
    // Шаг кнопок и колеса — вчетверо больше шага перетаскивания (NumberCore
    // тянет четвертью шага на пиксель): скорость мыши та же, что была.
    o.Step = speed * 4.0f;
    if (max > min) {
        o.Min = min;
        o.Max = max;
    }
    o.Format = format;
    return ictl::Number(host, label, v, o);
}

bool Int(EditorHost& host, const char* label, int* v, int min, int max) {
    Row row;
    return ictl::Integer(host, label, v, min, max);
}

bool Slider(EditorHost& host, const char* label, float* v, float min, float max, const char* format) {
    Row row;
    return ictl::SliderNumber(host, label, v, min, max, format);
}

bool Check(EditorHost& host, const char* label, bool* v) {
    Row row;
    return ictl::Switch(host, label, v);
}

bool Combo(EditorHost& host, const char* label, int* current, const char* const items[], int count) {
    Row row;
    ImGui::PushID(label);
    ictl::Label(label);
    const int before = *current;
    const bool changed = ictl::Combo("##combo", current, items, count);
    if (changed && *current != before) {
        // Снимок — ДО правки: вернуть старое, снять, поставить новое.
        const int now = *current;
        *current = before;
        host.PushUndoSnapshot();
        *current = now;
    }
    ImGui::PopID();
    return changed && *current != before;
}

bool Color3(EditorHost& host, const char* label, glm::vec3* rgb) {
    Row row;
    ImGui::PushID(label);
    ictl::Label(label);
    const bool changed = Sage::UI::ColorField3("##color", &rgb->x, Sage::UI::ColorField_Compact);
    host.TrackLastImGuiItem();
    ImGui::PopID();
    return changed;
}

bool Color4(EditorHost& host, const char* label, glm::vec4* rgba) {
    Row row;
    return ictl::Color(host, label, rgba);
}

} // namespace sage::editor::ifield
