#include "ObjectSlot.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorTheme.h"
#include "Localization.h"
#include "panels/HierarchyPanel.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "ui/UI.h"

namespace objectslot {

namespace {

// Пипетка одна на редактор: включена она у одного поля — у того, чью кнопку
// нажали. Поле узнаёт себя по ImGui-идентификатору.
struct PickState {
    bool Active = false;
    ImGuiID Field = 0;
    bool Delivered = false;
    int Id = 0;
};
PickState& Pick() {
    static PickState s;
    return s;
}

// Строка поиска — одна на открытый список: открыт он всегда один.
std::string& Search() {
    static std::string s;
    return s;
}

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Объекты сцены в порядке дерева, с глубиной — так список читается так же,
// как иерархия, и одинаковые имена различимы по родителю.
void Collect(const entt::registry& reg, entt::entity e, int depth,
             std::vector<std::pair<entt::entity, int>>& out) {
    out.push_back({e, depth});
    if (const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e))
        for (entt::entity c : h->Children)
            if (reg.valid(c) && reg.all_of<IdComponent>(c)) Collect(reg, c, depth + 1, out);
}

} // namespace

bool Picking() { return Pick().Active; }

void Deliver(int objectId) {
    PickState& p = Pick();
    if (!p.Active) return;
    p.Active = false;
    p.Delivered = true;
    p.Id = objectId;
}

void Cancel() { Pick() = PickState{}; }

void StartPick(unsigned int field) {
    PickState& p = Pick();
    p = PickState{};
    p.Active = true;
    p.Field = field;
}

bool TakeDelivered(unsigned int field, int& objectId) {
    PickState& p = Pick();
    if (!p.Delivered || p.Field != field) return false;
    p.Delivered = false;
    objectId = p.Id;
    return true;
}

Result Draw(EditorHost& host, const char* id, int current, const Options& options) {
    Result result;
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();
    ImGui::PushID(id);
    const ImGuiID fieldId = ImGui::GetID("##objslot");

    // Пипетка принесла объект этому полю.
    PickState& pick = Pick();
    int delivered = 0;
    if (TakeDelivered(fieldId, delivered)) {
        GameObject o = scene.Get(delivered);
        if (o.Valid() && delivered != options.SelfId &&
            (!options.Accept || options.Accept(reg, o.Entity()))) {
            result.Changed = true;
            result.Id = delivered;
        } else if (o.Valid()) {
            host.SetStatusMessage(options.AcceptHint ? T(options.AcceptHint)
                                                     : T("This object does not fit here"));
        }
    }
    // Esc снимает пипетку: иначе следующий щелчок по сцене молча уйдёт в поле.
    if (pick.Active && pick.Field == fieldId && ImGui::IsKeyPressed(ImGuiKey_Escape)) Cancel();

    const bool world = options.AllowWorld && current == options.WorldId;
    GameObject pointed = (!world && current > 0) ? scene.Get(current) : GameObject{};
    const bool missing = !world && current > 0 && !pointed.Valid();
    std::string label;
    const char* icon = "cube";
    if (world) {
        label = T("World");
        icon = "world";
    } else if (pointed.Valid()) {
        label = pointed.Name();
        icon = HierarchyPanel::IconFor(reg, pointed.Entity());
    } else if (missing) {
        label = std::string(T("Object is gone (id ")) + std::to_string(current) + ")";
        icon = "warn";
    } else {
        label = options.EmptyLabel ? T(options.EmptyLabel) : T("None");
        icon = "dots";
    }

    // --- Кнопка-слот: имя объекта, щелчок открывает список ---------------------
    const float buttons = ImGui::GetFrameHeight() * 3.0f + ImGui::GetStyle().ItemSpacing.x * 3.0f;
    const float width = std::max(80.0f, ImGui::GetContentRegionAvail().x - buttons);
    const bool picking = pick.Active && pick.Field == fieldId;
    if (missing) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.16f, 0.16f, 1.0f));
    if (picking) ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(EditorTheme::Role::Accent));
    const std::string shown = picking ? std::string(T("Click an object in the scene or the hierarchy…"))
                                      : label;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (ImGui::Button(("##slotbtn"), ImVec2(width, 0.0f))) {
        Search().clear();
        ImGui::OpenPopup("##objpick");
    }
    // Значок и имя поверх кнопки, обрезанные её шириной: длинное имя не
    // выталкивает кнопки справа за край панели.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = ImGui::GetFrameHeight();
        const float pad = ImGui::GetStyle().FramePadding.x;
        const float line = ImGui::GetTextLineHeight();
        const ImU32 textCol =
            ImGui::GetColorU32(pointed.Valid() || world || picking ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        dl->PushClipRect(ImVec2(at.x + pad, at.y), ImVec2(at.x + width - pad, at.y + h), true);
        EditorIcons::DrawLabeled(dl, ImVec2(at.x + pad, at.y + (h - line) * 0.5f), line,
                                 picking ? "select" : icon, textCol, shown.c_str(), textCol);
        dl->PopClipRect();
    }
    if (picking) ImGui::PopStyleColor();
    if (missing) ImGui::PopStyleColor();

    // Бросок строки из иерархии — с ответом до отпускания.
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiDragDropFlags flags = ImGuiDragDropFlags_AcceptBeforeDelivery;
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ENTITY", flags)) {
            if (p->DataSize == (int)sizeof(int)) {
                const int dropped = *(const int*)p->Data;
                GameObject o = scene.Get(dropped);
                const bool ok = o.Valid() && dropped != options.SelfId &&
                                (!options.Accept || options.Accept(reg, o.Entity()));
                if (!ok) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(dropped == options.SelfId
                                               ? T("An object cannot point at itself")
                                               : (options.AcceptHint ? T(options.AcceptHint)
                                                                     : T("This object does not fit here")));
                    ImGui::EndTooltip();
                } else if (p->IsDelivery()) {
                    result.Changed = true;
                    result.Id = dropped;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        ImGui::SetTooltip("%s", T("Click to choose from the list, or drag an object here from Hierarchy.\n"
                                  "The link keeps the object's id: renaming it breaks nothing."));

    // --- Список объектов с поиском --------------------------------------------
    if (Sage::UI::MenuScope pickMenu; ImGui::BeginPopup("##objpick")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", Search().c_str());
        ImGui::SetNextItemWidth(320.0f);
        if (ImGui::InputTextWithHint("##search", T("Search objects..."), buf, sizeof(buf))) Search() = buf;
        const std::string needle = Lower(Search());

        if (ImGui::Selectable(options.EmptyLabel ? T(options.EmptyLabel) : T("None"), current == 0)) {
            result.Changed = true;
            result.Id = 0;
        }
        if (options.AllowWorld && ImGui::Selectable(T("World"), world)) {
            result.Changed = true;
            result.Id = options.WorldId;
        }
        ImGui::Separator();

        // Корни — по возрастанию номера, как в иерархии.
        std::vector<std::pair<int, entt::entity>> roots;
        for (entt::entity e : reg.view<IdComponent>()) {
            const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
            if (h && h->Parent != entt::null && reg.valid(h->Parent)) continue;
            roots.push_back({reg.get<IdComponent>(e).Id, e});
        }
        std::sort(roots.begin(), roots.end());
        std::vector<std::pair<entt::entity, int>> rows;
        for (const auto& [rid, root] : roots) Collect(reg, root, 0, rows);

        if (ImGui::BeginChild("##objlist", ImVec2(320.0f, 280.0f))) {
            int shownRows = 0;
            for (const auto& [e, depth] : rows) {
                const int oid = reg.get<IdComponent>(e).Id;
                const NameComponent* nc = reg.try_get<NameComponent>(e);
                const std::string name = nc ? nc->Name : std::string("Object");
                if (!needle.empty() && Lower(name).find(needle) == std::string::npos) continue;
                const bool fits = oid != options.SelfId && (!options.Accept || options.Accept(reg, e));
                ++shownRows;
                ImGui::PushID(oid);
                // Отступ по глубине — только без поиска: с поиском дерево
                // рвётся, и отступ сбивал бы с толку.
                if (needle.empty() && depth > 0) ImGui::Indent((float)depth * 12.0f);
                ImGui::BeginDisabled(!fits);
                EditorIcons::Inline(HierarchyPanel::IconFor(reg, e));
                ImGui::SameLine(0.0f, EditorIcons::TextGap());
                if (ImGui::Selectable(name.c_str(), oid == current)) {
                    result.Changed = true;
                    result.Id = oid;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                if (!fits && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", oid == options.SelfId ? T("An object cannot point at itself")
                                                                  : (options.AcceptHint ? T(options.AcceptHint)
                                                                                        : T("This object does not fit here")));
                if (needle.empty() && depth > 0) ImGui::Unindent((float)depth * 12.0f);
                ImGui::PopID();
            }
            if (shownRows == 0) ImGui::TextDisabled("%s", T("Nothing found"));
        }
        ImGui::EndChild();
        if (result.Changed) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // --- Пипетка, показать, очистить ---------------------------------------------
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("select", T("Pick in the scene: click the object in the viewport "
                                                "or in Hierarchy (Esc — cancel)"),
                                    picking)) {
        if (picking) {
            Cancel();
        } else {
            StartPick(fieldId);
            host.SetStatusMessage(T("Click the object in the viewport or in Hierarchy (Esc — cancel)"));
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!pointed.Valid());
    if (EditorIcons::IconOnlyButton("focus", T("Select the linked object")) && pointed.Valid())
        host.Selection().SetPrimary(current);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(current == 0);
    if (EditorIcons::IconOnlyButton("trash", T("Clear the link"))) {
        result.Changed = true;
        result.Id = 0;
    }
    ImGui::EndDisabled();

    ImGui::PopID();
    return result;
}

} // namespace objectslot
