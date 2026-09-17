#include "InterfaceWidgets.h"

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

} // namespace sage::editor::interfacewidgets
