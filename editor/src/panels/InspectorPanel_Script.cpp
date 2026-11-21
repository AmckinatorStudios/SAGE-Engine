#include "InspectorPanel.h"

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "imgui.h"

#include "../AssetSlot.h"
#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../FileBrowser.h"
#include "../InspectorControls.h"
#include "../Localization.h"
#include "../Project.h"
#include "../VarsEditor.h"
#include "sage/scene/ScriptComponent.h"
#include "ui/UI.h"

namespace ictl = sage::editor::ictl;

// ---------------------------------------------------------------------------
// Секция «Script»: скрипты объекта, их файлы и публичные переменные.
//
// Вверху — «Добавить скрипт» и «Добавить группу». Ниже — скрипты без группы
// прямо в списке, затем группы человека. Каждый скрипт — сворачиваемый блок:
// слот файла и переменные ЭТОГО скрипта. Всё остальное (порядок, группа,
// удаление) — в меню правой кнопки по заголовку блока и перетаскиванием
// заголовка на группу: блок не обрастает рядом кнопок, которые нужны раз в
// неделю.
//
// ГРУППЫ — ТОЛЬКО РАСКЛАДКА. Порядок исполнения — порядок списка компонента
// («Выше»/«Ниже» в меню), и перенос в группу или из неё его не меняет: иначе
// разбор инспектора по папкам молча менял бы поведение уровня.
//
// Правки списка копятся за кадр и применяются ПОСЛЕ отрисовки: удалить
// элемент вектора, по которому сейчас идёт цикл, значит читать удалённое.
// ---------------------------------------------------------------------------
namespace {

constexpr const char* kSlotPayload = "SAGE_SCRIPT_SLOT";

struct ScriptOp {
    enum Kind { None, Remove, MoveUp, MoveDown, SetGroup, RemoveGroup, RenameGroup } What = None;
    int Slot = -1;
    std::string Group;  // SetGroup: куда (пусто — общий список); группы: какая
    std::string Name;   // RenameGroup: новое имя
};

// Подпись блока — имя файла: путь целиком не помещается в ширину инспектора,
// а различает скрипты одного объекта именно имя.
std::string ScriptTitle(const ScriptEntry& entry) {
    if (entry.Path.empty()) return T("No script attached");
    const size_t slash = entry.Path.find_last_of("/\\");
    return slash == std::string::npos ? entry.Path : entry.Path.substr(slash + 1);
}

bool FileMissing(EditorHost& host, const std::string& path) {
    if (path.empty()) return false;
    std::filesystem::path full = path;
    if (full.is_relative() && host.CurrentProject().Loaded()) full = host.CurrentProject().Dir() / full;
    std::error_code ec;
    return !std::filesystem::exists(full, ec);
}

std::string UniqueGroupName(const ScriptComponent& sc) {
    const std::string base = T("Group");
    for (int n = 1;; ++n) {
        std::string name = base + " " + std::to_string(n);
        if (!sc.HasGroup(name)) return name;
    }
}

// Приём перетаскиваемого скрипта: в группу group (пусто — в общий список).
void AcceptScriptDrop(const std::string& group, ScriptOp& op) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kSlotPayload)) {
        op.What = ScriptOp::SetGroup;
        op.Slot = *(const int*)p->Data;
        op.Group = group;
    }
    ImGui::EndDragDropTarget();
}

void Apply(EditorHost& host, ScriptComponent& sc, const ScriptOp& op) {
    if (op.What == ScriptOp::None) return;
    const int n = (int)sc.Scripts.size();
    const bool slotOk = op.Slot >= 0 && op.Slot < n;
    switch (op.What) {
        case ScriptOp::Remove:
            if (!slotOk) return;
            host.PushUndoSnapshot();
            sc.Scripts.erase(sc.Scripts.begin() + op.Slot);
            return;
        case ScriptOp::MoveUp:
        case ScriptOp::MoveDown: {
            const int other = op.Slot + (op.What == ScriptOp::MoveUp ? -1 : 1);
            if (!slotOk || other < 0 || other >= n) return;
            host.PushUndoSnapshot();
            std::swap(sc.Scripts[(size_t)op.Slot], sc.Scripts[(size_t)other]);
            return;
        }
        case ScriptOp::SetGroup:
            if (!slotOk || sc.Scripts[(size_t)op.Slot].Group == op.Group) return;
            if (!op.Group.empty() && !sc.HasGroup(op.Group)) return;
            host.PushUndoSnapshot();
            sc.Scripts[(size_t)op.Slot].Group = op.Group;
            return;
        case ScriptOp::RemoveGroup:
            // Скрипты группы НЕ удаляются, а возвращаются в общий список:
            // группа — раскладка, и её удаление не должно снимать поведение.
            host.PushUndoSnapshot();
            sc.Groups.erase(std::remove(sc.Groups.begin(), sc.Groups.end(), op.Group), sc.Groups.end());
            for (ScriptEntry& e : sc.Scripts)
                if (e.Group == op.Group) e.Group.clear();
            return;
        case ScriptOp::RenameGroup: {
            if (op.Name.empty() || op.Name == op.Group || sc.HasGroup(op.Name)) return;
            host.PushUndoSnapshot();
            for (std::string& g : sc.Groups)
                if (g == op.Group) g = op.Name;
            for (ScriptEntry& e : sc.Scripts)
                if (e.Group == op.Group) e.Group = op.Name;
            return;
        }
        case ScriptOp::None: return;
    }
}

} // namespace

void InspectorPanel::DrawScriptSection(EditorHost& host, GameObject obj, entt::registry& reg, EntityRemovals& rm) {
    if (!reg.all_of<ScriptComponent>(obj.Entity())) return;
    if (!EditorTheme::SectionHeader("script", T("Script" "###Script"), ImGuiTreeNodeFlags_DefaultOpen, &rm.Script,
                                    T("Runs in Play mode: Start, Update, FixedUpdate, collisions")))
        return;

    // Объявления подмешиваются ПЕРЕД показом, а не однажды при назначении
    // файла: .lua правят снаружи редактора, и переменная, добавленная в
    // скрипт минуту назад, обязана появиться здесь сама.
    host.MergeScriptVars(obj);
    ScriptComponent& sc = reg.get<ScriptComponent>(obj.Entity());
    ScriptOp op;

    if (ictl::Button("plus", T("Add Script"), T("Add one more script to this object"))) {
        host.PushUndoSnapshot();
        sc.Add(std::string());
    }
    ImGui::SameLine();
    if (ictl::Button("folder-plus", T("Add Group"),
                     T("A group only arranges scripts here: it does not change their order or behaviour"))) {
        host.PushUndoSnapshot();
        sc.Groups.push_back(UniqueGroupName(sc));
    }

    // Один блок скрипта: заголовок (перетаскивание, приём, меню) и, если
    // раскрыт, слот файла и переменные.
    auto drawScript = [&](int i) {
        ScriptEntry& entry = sc.Scripts[(size_t)i];
        const bool missing = FileMissing(host, entry.Path);
        const std::string title = (missing ? std::string(T("(missing) ")) : std::string()) + ScriptTitle(entry);
        // Раскрытие запоминается по месту в списке, а не по объекту: иначе
        // настройки редактора копили бы строку на каждый объект проекта.
        const std::string id = "script.slot." + std::to_string(i);
        ImGui::PushID(i);
        const bool open = ictl::SubSection(id.c_str(), title.c_str(), true, [&] {
            if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                ImGui::SetTooltip("%s\n%s", entry.Path.empty() ? T("No script attached") : entry.Path.c_str(),
                                  T("Drag onto a group to move it there; right-click for more"));
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(kSlotPayload, &i, sizeof(int));
                ImGui::TextUnformatted(ScriptTitle(entry).c_str());
                ImGui::EndDragDropSource();
            }
            // Брошенный на скрипт попадает в ЕГО группу — так скрипт и
            // возвращают в общий список, перетащив на скрипт без группы.
            AcceptScriptDrop(entry.Group, op);
            if (Sage::UI::MenuScope menu; ImGui::BeginPopupContextItem("##script_ctx")) {
                if (EditorIcons::MenuItem("chevron-up", T("Run Earlier"), nullptr, i > 0)) {
                    op.What = ScriptOp::MoveUp;
                    op.Slot = i;
                }
                if (EditorIcons::MenuItem("chevron-down", T("Run Later"), nullptr, i + 1 < (int)sc.Scripts.size())) {
                    op.What = ScriptOp::MoveDown;
                    op.Slot = i;
                }
                if (!sc.Groups.empty() && ImGui::BeginMenu(T("Move to Group"))) {
                    if (EditorIcons::MenuItemSelected("list", T("No Group"), entry.Group.empty())) {
                        op.What = ScriptOp::SetGroup;
                        op.Slot = i;
                        op.Group.clear();
                    }
                    for (const std::string& g : sc.Groups)
                        if (EditorIcons::MenuItemSelected("folder", g.c_str(), entry.Group == g)) {
                            op.What = ScriptOp::SetGroup;
                            op.Slot = i;
                            op.Group = g;
                        }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (EditorIcons::MenuItem("trash", T("Remove Script"))) {
                    op.What = ScriptOp::Remove;
                    op.Slot = i;
                }
                ImGui::EndPopup();
            }
        });
        if (open) {
            // Тот же слот, что у меша, материала и текстур (см. AssetSlot.h):
            // обложка, приём перетаскивания с проверкой типа, «показать в
            // Assets». «+» создаёт скрипт с именем объекта, «править»
            // открывает его в редакторе кода.
            assetslot::Actions actions;
            for (char c : obj.Name())
                actions.SuggestedName += (std::isalnum((unsigned char)c) || c == '_') ? c : '_';
            const assetslot::Result r = assetslot::Draw(host, "script", assetslot::Kind::Script, entry.Path,
                                                        &m_preview, T("No script attached"), &actions);
            if (r.Changed) {
                host.PushUndoSnapshot();
                // Значения переменных остаются: одноимённые переменные нового
                // файла их подхватят (Table::MergeDeclaration), остальные
                // погаснут, но не пропадут — вернуть прежний файл без потерь.
                sc.Scripts[(size_t)i].Path = r.Path;
            }
            if (r.BrowseRequested) {
                FileBrowser::Config c;
                c.Title = T("Choose a script");
                c.Filters = assetslot::Extensions(assetslot::Kind::Script);
                c.FilterLabel = T("Scripts (*.lua, *.slua)");
                c.StartDir = c.Root = assetslot::ProjectRoot(host);
                m_browser.Open(c);
                // Цель — СУЩНОСТЬ и номер скрипта, а не указатель: диалог
                // отвечает через кадр, а сцену за это время могут перезагрузить.
                m_browseTarget = nullptr;
                m_browseScriptEntity = obj.Id();
                m_browseScriptSlot = i;
                m_browseIsShader = false;
                m_browseIsMesh = false;
                m_browseIsMaterial = false;
            }
            ScriptEntry& cur = sc.Scripts[(size_t)i];
            if (!cur.Fields.Empty()) {
                ictl::SeparatorText(T("Variables"));
                // Имя слева, значение справа — та же раскладка, что у
                // переменных объекта (VarsEditor.cpp). Раньше здесь стояли
                // одни поля без имён, и какое число — скорость, а какое —
                // прыжок, приходилось угадывать.
                if (ImGui::BeginTable("##script_vars", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
                    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.42f);
                    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
                    for (sage::vars::Var& var : cur.Fields.All()) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        // Переменная, которой в скрипте больше нет, остаётся
                        // видна приглушённой: молча стереть настройку из-за
                        // опечатки в имени хуже, чем показать лишнюю строку.
                        if (!var.Declared) ImGui::BeginDisabled();
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(var.Title().c_str());
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            std::string tip = var.Name;
                            if (!var.Tooltip.empty()) tip += "\n" + var.Tooltip;
                            if (!var.Declared) tip += std::string("\n") + T("The script no longer declares this variable");
                            ImGui::SetTooltip("%s", tip.c_str());
                        }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::SetNextItemWidth(-1.0f);
                        // Снимок для отмены берёт сам рисовальщик значения (у
                        // ползунка он нужен ДО перетаскивания, а не после).
                        varsui::DrawValue(host, var.Name.c_str(), var.Data, &var, &m_preview);
                        if (!var.Declared) ImGui::EndDisabled();
                    }
                    ImGui::EndTable();
                }
            }
            ictl::EndSubSection();
        }
        ImGui::PopID();
    };

    // Сначала скрипты без группы — прямо в списке, потом группы. Внутри
    // любой части — порядок исполнения.
    for (int i = 0; i < (int)sc.Scripts.size(); ++i)
        if (sc.Scripts[(size_t)i].Group.empty() || !sc.HasGroup(sc.Scripts[(size_t)i].Group)) drawScript(i);

    for (size_t g = 0; g < sc.Groups.size(); ++g) {
        const std::string group = sc.Groups[g];
        const std::string id = "script.group." + group;
        ImGui::PushID((int)(1000 + g));
        const bool open = ictl::SubSection(id.c_str(), group.c_str(), true, [&] {
            AcceptScriptDrop(group, op);
            if (Sage::UI::MenuScope menu; ImGui::BeginPopupContextItem("##group_ctx")) {
                if (m_scriptGroupRenaming != group) {
                    m_scriptGroupRenaming = group;
                    std::snprintf(m_scriptGroupName, sizeof(m_scriptGroupName), "%s", group.c_str());
                }
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##group_name", m_scriptGroupName, sizeof(m_scriptGroupName),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    op.What = ScriptOp::RenameGroup;
                    op.Group = group;
                    op.Name = m_scriptGroupName;
                    ImGui::CloseCurrentPopup();
                }
                EditorTheme::Hint(T("Enter renames the group"));
                ImGui::Separator();
                if (EditorIcons::MenuItem("trash", T("Remove Group"))) {
                    op.What = ScriptOp::RemoveGroup;
                    op.Group = group;
                }
                ImGui::EndPopup();
            } else if (m_scriptGroupRenaming == group) {
                m_scriptGroupRenaming.clear();
            }
        });
        if (open) {
            bool any = false;
            for (int i = 0; i < (int)sc.Scripts.size(); ++i)
                if (sc.Scripts[(size_t)i].Group == group) {
                    drawScript(i);
                    any = true;
                }
            if (!any) ImGui::TextDisabled("%s", T("Drag scripts here"));
            ictl::EndSubSection();
        }
        ImGui::PopID();
    }

    Apply(host, sc, op);
}
