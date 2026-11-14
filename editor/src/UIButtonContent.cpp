// ---------------------------------------------------------------------------
// CONTENT КНОПКИ в инспекторе: подпись, значок, выравнивание, отступ.
//
// Подпись и значок — дети кнопки (sage/ui/UIButton.h), но правятся здесь,
// у самой кнопки: «написать на кнопке "Играть" и поставить значок» — одно
// действие, а не поиск двух объектов в иерархии.
// ---------------------------------------------------------------------------
#include "UIElementProperties.h"

#include <cstdio>

#include "imgui.h"

#include "EditorHost.h"
#include "Localization.h"

#include "sage/scene/Scene.h"
#include "sage/ui/UIButton.h"
#include "sage/ui/UIIcons.h"

namespace sage::editor {

void DrawButtonContent(EditorHost& host, GameObject obj) {
    namespace ui = sage::ui;
    Scene& scene = host.CurrentScene();
    ui::ButtonContent content;
    if (!ui::ReadButtonContent(scene, obj.Entity(), content)) return;
    if (!ImGui::CollapsingHeader(T("Content"), ImGuiTreeNodeFlags_DefaultOpen)) return;
    bool changed = false;

    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", content.Label.c_str());
    if (ImGui::InputText(T("Label"), buf, sizeof(buf))) {
        content.Label = buf;
        changed = true;
    }
    host.TrackLastImGuiItem();

    if (ImGui::BeginCombo(T("Icon"), content.Icon.empty() ? T("(none)") : content.Icon.c_str())) {
        if (ImGui::Selectable(T("(none)"), content.Icon.empty())) {
            host.PushUndoSnapshot();
            content.Icon.clear();
            changed = true;
        }
        for (const std::string& name : ui::IconNames()) {
            if (ImGui::Selectable(name.c_str(), name == content.Icon)) {
                host.PushUndoSnapshot();
                content.Icon = name;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    const char* kAlign[] = {T("Start"), T("Center"), T("End")};
    int align = content.Alignment == ui::Arrangement::Spread::Start ? 0
                : content.Alignment == ui::Arrangement::Spread::End ? 2
                                                                     : 1;
    if (ImGui::Combo(T("Alignment"), &align, kAlign, 3)) {
        host.PushUndoSnapshot();
        content.Alignment = align == 0   ? ui::Arrangement::Spread::Start
                            : align == 2 ? ui::Arrangement::Spread::End
                                         : ui::Arrangement::Spread::Center;
        changed = true;
    }
    if (!content.Icon.empty()) {
        if (ImGui::DragFloat(T("Spacing"), &content.Spacing, 0.5f, 0.0f, 128.0f, "%.0f")) changed = true;
        host.TrackLastImGuiItem();
    }
    if (changed) ui::ApplyButtonContent(scene, obj.Entity(), content);
}

} // namespace sage::editor
