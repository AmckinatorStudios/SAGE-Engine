#include "InterfaceWidgets.h"

#include "sage/ui/UI.h"

#include "imgui.h"

#include "../EditorIcons.h"

namespace sage::editor::interfacewidgets {

namespace {

// Сторона -> имя иконки набора. Имена про СМЫСЛ («выровнять по левому краю»),
// какой глиф Tabler за ними стоит — дело таблицы в scripts/gen_icon_font.py.
const char* IconFor(sage::ui::AlignEdge edge) {
    switch (edge) {
        case sage::ui::AlignEdge::Left:    return "align-left";
        case sage::ui::AlignEdge::CenterX: return "align-center-x";
        case sage::ui::AlignEdge::Right:   return "align-right";
        case sage::ui::AlignEdge::Top:     return "align-top";
        case sage::ui::AlignEdge::CenterY: return "align-center-y";
        case sage::ui::AlignEdge::Bottom:  return "align-bottom";
    }
    return "align";
}

} // namespace

bool AlignButton(const char* id, sage::ui::AlignEdge edge, const char* tooltip, bool enabled) {
    ImGui::PushID(id);
    if (!enabled) ImGui::BeginDisabled();
    const bool pressed = EditorIcons::IconOnlyButton(IconFor(edge), tooltip);
    if (!enabled) ImGui::EndDisabled();
    ImGui::PopID();
    return pressed && enabled;
}

const char* PresetIcon(const std::string& preset) {
    // Значок объявлен у самого типа (sage::ui::Preset::Icon): второй список
    // здесь расходился с ним при каждом переименовании.
    if (const sage::ui::Preset* p = sage::ui::FindPreset(preset)) return p->Icon;
    return "ui-empty";
}

} // namespace sage::editor::interfacewidgets
