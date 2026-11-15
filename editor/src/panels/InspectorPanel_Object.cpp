#include "InspectorPanel.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorTheme.h"
#include "InspectorControls.h"
#include "Localization.h"
#include "ui/UI.h"
#include "sage/audio/AudioComponents.h"
#include "sage/render/ReflectionComponents.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneComponents.h"
#include "sage/scene/SceneLayer.h"
#include "sage/ui/components/InterfaceComponent.h"

namespace {

namespace ictl = sage::editor::ictl;

// Что это за объект — по тому, что в нём есть: значок и короткий тип в шапке.
struct ObjectKind {
    const char* Icon;
    const char* Name;
};
ObjectKind KindOf(entt::registry& reg, entt::entity e) {
    if (reg.all_of<FolderComponent>(e)) return {"folder", T("Folder")};
    if (reg.all_of<CameraComponent>(e)) return {"camera", T("Camera")};
    if (reg.all_of<LightComponent>(e)) return {"light", T("Light")};
    if (reg.all_of<sage::ui::InterfaceComponent>(e)) return {"layout", T("Interface")};
    if (reg.all_of<AudioSourceComponent>(e)) return {"audio", T("Audio")};
    if (reg.all_of<ReflectionProbeComponent>(e)) return {"probe", T("Reflection probe")};
    if (reg.all_of<MeshRendererComponent>(e)) return {"cube", T("Mesh object")};
    return {"cube", T("Empty object")};
}

} // namespace

// ШАПКА ОБЪЕКТА: значок и тип, имя, замок инспектора — одной строкой; ниже —
// тег и слой сцены рядом (или друг под другом в узкой панели) и «переходит в
// следующую сцену». Раньше каждое из них было своей длинной строкой с
// подписью справа.
void InspectorPanel::DrawObjectIdentity(EditorHost& host, GameObject obj) {
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();
    const ObjectKind kind = KindOf(reg, obj.Entity());
    const float side = ImGui::GetFrameHeight();

    // --- Значок, имя, замок -------------------------------------------------
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float icon = std::floor(side * 0.8f);
    ImGui::Dummy(ImVec2(side, side));
    EditorIcons::Overlay(p.x + std::floor((side - icon) * 0.5f), p.y + std::floor((side - icon) * 0.5f), icon,
                         kind.Icon, glm::vec3(0.62f, 0.72f, 0.85f));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kind.Name);
    ImGui::SameLine(0.0f, 6.0f);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", obj.Name().c_str());
    ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - side - 8.0f));
    if (ImGui::InputTextWithHint("##object_name", T("Name"), buf, sizeof(buf))) obj.SetName(buf);
    host.TrackLastImGuiItem();
    DrawLockButton(host);
    // Тип — приглушённо под именем: «что это» видно сразу, места не занимает.
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + side + 6.0f);
    ImGui::TextDisabled("%s", kind.Name);

    // У папки нет ни тега, ни слоя: она — порядок в списке, а не объект игры.
    if (reg.all_of<FolderComponent>(obj.Entity())) {
        ImGui::Separator();
        return;
    }

    // --- Тег и слой сцены ---------------------------------------------------
    //
    // Тег — один на объект (TagComponent): имя группы для скриптов
    // (scene:FindByTag). Вводится как есть, стрелка рядом подсказывает теги,
    // что уже есть в сцене. Слой сцены — классификация объектов (не слой
    // рендера меша): выбрать, завести, переименовать, удалить.
    const float avail = ImGui::GetContentRegionAvail().x;
    const bool twoColumns = avail >= 300.0f;
    const float colW = twoColumns ? std::floor((avail - 10.0f) * 0.5f) : avail;

    auto fieldTitle = [](const char* text) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::TextDim));
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    };

    ImGui::BeginGroup();
    fieldTitle(T("Tag"));
    {
        char tag[64];
        const TagComponent* tc = reg.try_get<TagComponent>(obj.Entity());
        std::snprintf(tag, sizeof(tag), "%s", tc ? tc->Tag.c_str() : "");
        ImGui::SetNextItemWidth(colW - side - 2.0f);
        if (ImGui::InputTextWithHint("##tag", T("none"), tag, sizeof(tag))) {
            if (tag[0]) reg.emplace_or_replace<TagComponent>(obj.Entity(), TagComponent{tag});
            else reg.remove<TagComponent>(obj.Entity());
        }
        host.TrackLastImGuiItem();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Group name for scripts: scene:FindByTag(\"Enemy\")"));
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::ArrowButton("##tag_list", ImGuiDir_Down)) ImGui::OpenPopup("SceneTags###SceneTags");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Tags already used in this scene"));
        if (Sage::UI::MenuScope menu; ImGui::BeginPopup("SceneTags###SceneTags")) {
            const std::vector<std::string> tags = sage::scene::SceneTags(scene);
            if (tags.empty()) ImGui::TextDisabled("%s", T("No tags in this scene yet: type one"));
            for (const std::string& t : tags) {
                if (ImGui::Selectable(t.c_str(), tc && tc->Tag == t)) {
                    host.PushUndoSnapshot();
                    reg.emplace_or_replace<TagComponent>(obj.Entity(), TagComponent{t});
                }
            }
            if (tc) {
                ImGui::Separator();
                if (ImGui::Selectable(T("Clear the tag"))) {
                    host.PushUndoSnapshot();
                    reg.remove<TagComponent>(obj.Entity());
                }
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndGroup();
    if (twoColumns) ImGui::SameLine(0.0f, 10.0f);

    ImGui::BeginGroup();
    fieldTitle(T("Scene Layer"));
    {
        const std::string current = sage::scene::LayerOf(scene, obj.Id());
        ImGui::SetNextItemWidth(colW - side - 2.0f);
        if (ImGui::BeginCombo("##scene_layer", current.empty() ? T("none") : current.c_str())) {
            if (ImGui::Selectable(T("none"), current.empty()) && !current.empty()) {
                host.PushUndoSnapshot();
                sage::scene::SetLayerOf(scene, obj.Id(), "");
            }
            for (const std::string& l : sage::scene::SceneLayerNames(scene)) {
                if (ImGui::Selectable(l.c_str(), l == current) && l != current) {
                    host.PushUndoSnapshot();
                    sage::scene::SetLayerOf(scene, obj.Id(), l);
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Scene layer: a group of objects for sorting and filtering. "
                                      "It is not the render layer of a mesh."));
        ImGui::SameLine(0.0f, 2.0f);
        if (EditorIcons::IconOnlyButton("pencil", T("Scene layers: add, rename, remove")))
            ImGui::OpenPopup("SceneLayers###SceneLayers");
        DrawSceneLayersPopup(host);
    }
    ImGui::EndGroup();

    // --- Переходит в следующую сцену ----------------------------------------
    bool keep = reg.all_of<KeepOnLoadComponent>(obj.Entity());
    ImGui::Spacing();
    if (ictl::Switch(host, T("Keep on scene change"), &keep)) {
        if (keep) reg.emplace_or_replace<KeepOnLoadComponent>(obj.Entity());
        else reg.remove<KeepOnLoadComponent>(obj.Entity());
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("When a script loads another scene (scene:Load), this object moves there "
                                  "with its children: the player, music, a game manager."));
    ImGui::Separator();
}

// СЛОИ СЦЕНЫ: список с переименованием на месте и удалением, внизу —
// новый слой. Удалённый слой снимается с объектов (они остаются без слоя).
void InspectorPanel::DrawSceneLayersPopup(EditorHost& host) {
    if (Sage::UI::MenuScope menu; ImGui::BeginPopup("SceneLayers###SceneLayers")) {
        Scene& scene = host.CurrentScene();
        ImGui::TextDisabled("%s", T("Scene layers"));
        ImGui::Separator();
        const std::vector<std::string> layers = sage::scene::SceneLayerNames(scene);
        if (layers.empty()) ImGui::TextDisabled("%s", T("No layers yet"));
        std::string removeName;
        for (size_t i = 0; i < layers.size(); ++i) {
            ImGui::PushID((int)i);
            char name[64];
            std::snprintf(name, sizeof(name), "%s", layers[i].c_str());
            ImGui::SetNextItemWidth(180.0f);
            ImGui::InputText("##layer_name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsItemDeactivatedAfterEdit() && layers[i] != name) {
                host.PushUndoSnapshot();
                if (!sage::scene::RenameSceneLayer(scene, layers[i], name))
                    host.SetStatusMessage(T("This layer name is empty or taken"));
            }
            ImGui::SameLine(0.0f, 2.0f);
            if (EditorIcons::IconOnlyButton("trash", T("Remove the layer: its objects stay without a layer")))
                removeName = layers[i];
            ImGui::PopID();
        }
        if (!removeName.empty()) {
            host.PushUndoSnapshot();
            sage::scene::RemoveSceneLayer(scene, removeName);
        }
        ImGui::Separator();
        static char fresh[64] = "";
        ImGui::SetNextItemWidth(180.0f);
        const bool enter = ImGui::InputTextWithHint("##new_layer", T("New layer"), fresh, sizeof(fresh),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine(0.0f, 2.0f);
        if ((EditorIcons::IconOnlyButton("plus", T("Add the layer")) || enter) && fresh[0]) {
            host.PushUndoSnapshot();
            if (sage::scene::AddSceneLayer(scene, fresh)) fresh[0] = 0;
            else host.SetStatusMessage(T("This layer name is taken"));
        }
        ImGui::EndPopup();
    }
}
