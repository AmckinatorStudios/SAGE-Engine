#include "SpritePicker.h"

#include <algorithm>
#include <string>
#include <vector>

#include "imgui.h"
#include "../EditorHost.h"
#include "../InspectorControls.h"
#include "../Localization.h"
#include "sage/render/ResourceManager.h"

namespace sage::editor {

bool SpritePicker(EditorHost& host, const char* label, assets::SpriteRef& value) {
    auto& resources = ResourceManager::Instance();
    std::string preview = T("(none)");
    if (value.Valid()) {
        const auto current = resources.ResolveSprite(value);
        if (const auto* sequence = current.Sequence()) preview = sequence->Name;
        else if (const auto* region = current.Region()) preview = region->Name;
        else preview = T("Missing sprite");
    }
    ictl::Label(label);
    assets::SpriteRef next = value;
    if (ictl::BeginDropdown("##sprite", preview.c_str())) {
        if (ImGui::Selectable(T("(none)"), !value.Valid())) next = {};
        std::vector<AssetRecord> sheets;
        for (const auto& record : AssetDatabase::Instance().All())
            if (record.Type == "spriteSheet") sheets.push_back(record);
        std::sort(sheets.begin(), sheets.end(), [](const auto& a, const auto& b) { return a.Path < b.Path; });
        for (const auto& sheet : sheets) {
            ImGui::PushID(sheet.Path.c_str());
            if (ImGui::TreeNode("##sheet", "%s", sheet.Path.c_str())) {
                const auto document = resources.ResolveSpriteSheet(sheet.Guid);
                if (!document)
                    ImGui::TextWrapped("%s", document.Diagnostic.c_str());
                else {
                    const auto pick = [&](const auto& item, assets::SpriteRef::Kind kind) {
                        const auto id = item.Id().ToString();
                        ImGui::PushID(id.c_str());
                        const std::string title = kind == assets::SpriteRef::Kind::Sequence
                            ? std::string(T("Sequence")) + " / " + item.Name : item.Name;
                        if (ImGui::Selectable(title.c_str(), value.Asset == sheet.Guid &&
                                              value.Item == item.Id() && value.Type == kind))
                            next = {sheet.Guid, item.Id(), kind};
                        ImGui::PopID();
                    };
                    for (const auto& region : document.Resource->Sheet.Regions()) pick(region, assets::SpriteRef::Kind::Region);
                    for (const auto& sequence : document.Resource->Sheet.Sequences()) pick(sequence, assets::SpriteRef::Kind::Sequence);
                    if (document.Resource->Sheet.Regions().empty() && document.Resource->Sheet.Sequences().empty())
                        ImGui::TextDisabled("%s", T("(none)"));
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ictl::EndDropdown();
    }
    if (value.Asset == next.Asset && value.Item == next.Item && value.Type == next.Type) return false;
    host.PushUndoSnapshot();
    value = next;
    return true;
}

} // namespace sage::editor
