#include "InputPanel.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <vector>

#include <imgui.h>

#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../ui/UI.h"

using sage::input::Action;
using sage::input::ActionType;
using sage::input::Binding;
using sage::input::Component;
using sage::input::Context;
using sage::input::DeadZoneMode;
using sage::input::DeviceKeyboard;
using sage::input::DeviceMouse;
using sage::input::DeviceNone;
using sage::input::InputEvent;
using sage::input::InputEventType;
using sage::input::InputSystem;
using sage::input::SourceKind;
using sage::input::TriggerMode;

#include "InputPanelDetail.h"

bool InputPanel::IsCollapsed(const std::string& context) const {
    return std::find(m_collapsed.begin(), m_collapsed.end(), context) != m_collapsed.end();
}

void InputPanel::ToggleCollapsed(const std::string& context) {
    auto it = std::find(m_collapsed.begin(), m_collapsed.end(), context);
    if (it == m_collapsed.end()) m_collapsed.push_back(context);
    else m_collapsed.erase(it);
}

std::string InputPanel::ConflictWith(InputSystem& input, const Binding& binding,
                                     const Action* self) const {
    for (Context* ctx : input.ContextsByPriority()) {
        for (const std::string& name : ctx->ActionNames()) {
            Action* other = ctx->Find(name);
            if (!other || other == self) continue;
            if (other->UsesSource(binding)) return ctx->Name() + " / " + name;
        }
    }
    return {};
}

bool InputPanel::HasAnyConflict(InputSystem& input, const Action& action) const {
    for (const Binding& b : action.Bindings()) {
        if (!ConflictWith(input, b, &action).empty()) return true;
    }
    return false;
}

// --- ШАПКА ---------------------------------------------------------------
//
// Поиск, «+ Действие» и «+ Контекст» — то, чем работают каждый раз. Сохранение и
// откат живут в общих кнопках окна настроек (Cancel / Apply): второй системы
// сохранения здесь нет.
void InputPanel::DrawHeader(EditorHost& host, InputSystem& input) {
    const Sage::UI::Style& ui = Sage::UI::Get();

    const float h = ImGui::GetFrameHeight();
    const float addW = EditorIcons::LabeledWidth(std::floor(h * 0.68f), T("Add Action")) +
                       ImGui::GetStyle().FramePadding.x * 2.0f;
    const float ctxW = EditorIcons::LabeledWidth(std::floor(h * 0.68f), T("Context")) +
                       ImGui::GetStyle().FramePadding.x * 2.0f;
    const float tail = addW + ctxW + h + ui.SpacingSM * 3.0f;
    const float searchW = std::max(120.0f, ImGui::GetContentRegionAvail().x - tail);

    // Ширина — параметром, а не SetNextItemWidth: поле ставит её себе само.
    Sage::UI::SearchField("##search", m_search, sizeof(m_search), T("Search actions..."), searchW);

    ImGui::SameLine(0.0f, ui.SpacingSM);
    if (EditorIcons::Button("plus", T("Add Action"), T("Create an action and assign a key to it")))
        OpenNewAction(std::string());
    ImGui::SameLine(0.0f, ui.SpacingSM);
    if (EditorIcons::Button("plus", T("Context"),
                            T("One key means different things in the game, in the inventory "
                              "and in a dialogue. Each set of them is a context.")))
        m_newContextOpen = true;
    ImGui::SameLine(0.0f, ui.SpacingSM);
    if (EditorIcons::IconOnlyButton("dots", T("More"))) ImGui::OpenPopup("##inputMore");

    if (Sage::UI::MenuScope moreMenu; ImGui::BeginPopup("##inputMore")) {
        Sage::UI::MenuSection(T("Standard layout"), true);
        Sage::UI::TextSecondary("%s", T("Adds the usual set of actions. Existing ones are left alone."));
        if (EditorIcons::MenuItem("pilot", T("First-person / third-person game"))) {
            Action& move = input.Register("Move", ActionType::Vector);
            if (move.Bindings().empty()) {
                move.BindVector("W", "S", "A", "D");
                move.Bind(Binding::OfPadAxis(sage::input::GamepadAxis::LeftX).On(Component::X));
                move.Bind(Binding::OfPadAxis(sage::input::GamepadAxis::LeftY).On(Component::Y));
            }
            Action& look = input.Register("Look", ActionType::Vector);
            if (look.Bindings().empty()) {
                look.Bind(Binding::MouseAxisX().On(Component::X));
                look.Bind(Binding::MouseAxisY().On(Component::Y));
                look.Bind(Binding::OfPadAxis(sage::input::GamepadAxis::RightX).On(Component::X));
                look.Bind(Binding::OfPadAxis(sage::input::GamepadAxis::RightY).On(Component::Y));
                // Обзору сглаживание вредит, а мёртвая зона мыши не нужна.
                look.Settings().Smoothing = 0.0f;
                look.Settings().Normalize = false;
            }
            struct Simple { const char* Name; const char* Key; const char* Pad; };
            static const Simple kSimple[] = {
                {"Jump", "SPACE", "PAD_A"},          {"Sprint", "LEFT_SHIFT", "PAD_LEFT_THUMB"},
                {"Crouch", "LEFT_CONTROL", "PAD_B"}, {"Interact", "E", "PAD_X"},
                {"Attack", "MOUSE_LEFT", "PAD_RB"},  {"Aim", "MOUSE_RIGHT", "PAD_LB"},
                {"Reload", "R", "PAD_Y"},            {"Pause", "ESCAPE", "PAD_START"},
                {"Inventory", "TAB", "PAD_BACK"},
            };
            for (const Simple& item : kSimple) {
                Action& a = input.Register(item.Name, ActionType::Digital);
                if (!a.Bindings().empty()) continue;
                a.Bind(item.Key);
                a.Bind(item.Pad);
            }
            host.SetProjectInputDirty(true);
            host.SetStatusMessage(T("Standard layout added"));
        }
        ImGui::EndPopup();
    }
}

void InputPanel::DrawContextMenu(EditorHost& host, InputSystem& input, Context& ctx) {
    if (Sage::UI::MenuScope ctxMenu; ImGui::BeginPopup("##ctxSettings")) {
        Sage::UI::MenuSection(ctx.Name().c_str(), true);
        if (EditorIcons::MenuItem("plus", T("Add action here"))) OpenNewAction(ctx.Name());
        ImGui::Separator();
        Sage::UI::BeginProperties("ctxprops");
        Sage::UI::PropertyLabel(T("Priority"),
                                T("Higher priority takes a device away from contexts below it."));
        ImGui::SetNextItemWidth(140.0f);
        int priority = ctx.Priority();
        if (ImGui::DragInt("##priority", &priority, 1.0f, -1000, 1000)) {
            ctx.SetPriority(priority);
            host.SetProjectInputDirty(true);
        }
        Sage::UI::PropertyLabel(T("Takes"),
                                T("Taken from the contexts below only when this context actually "
                                  "used the device that frame."));
        bool blocksMouse = (ctx.Blocks() & DeviceMouse) != 0;
        bool blocksKeys = (ctx.Blocks() & DeviceKeyboard) != 0;
        if (ImGui::Checkbox(T("Mouse"), &blocksMouse)) {
            ctx.SetBlocks((uint8_t)((ctx.Blocks() & ~DeviceMouse) | (blocksMouse ? DeviceMouse : 0)));
            host.SetProjectInputDirty(true);
        }
        ImGui::SameLine();
        if (ImGui::Checkbox(T("Keyboard"), &blocksKeys)) {
            ctx.SetBlocks(
                (uint8_t)((ctx.Blocks() & ~DeviceKeyboard) | (blocksKeys ? DeviceKeyboard : 0)));
            host.SetProjectInputDirty(true);
        }
        Sage::UI::EndProperties();

        // Контекст по умолчанию не удаляется: действия из него никуда бы не
        // делись, а дома у них не осталось бы.
        if (ctx.Name() != sage::input::kDefaultContext) {
            ImGui::Separator();
            if (EditorIcons::MenuItem("trash", T("Delete the context"))) {
                input.RemoveContext(ctx.Name());
                host.SetProjectInputDirty(true);
            }
        }
        ImGui::EndPopup();
    }
}

// --- СТРОКА ДЕЙСТВИЯ -----------------------------------------------------
//
// Одна строка: имя, кейкапы назначенного, вид значения, меню. Щелчок по строке
// открывает окно действия — то же, что и «Изменить» в меню. Правой кнопки,
// выделения и перетаскивания нет: всё доступное написано в меню строки.
void InputPanel::DrawActionRow(EditorHost& host, InputSystem& input, Context& ctx,
                               const Action& action) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    const float h = ImGui::GetFrameHeight();
    const float rowH = h + ui.SpacingXS;
    const ImVec2 rowPos = ImGui::GetCursorScreenPos();
    const float rowW = ImGui::GetContentRegionAvail().x;

    if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(rowW, rowH)))
        OpenEditAction(input, {ctx.Name(), action.Name()});
    Sage::UI::Tooltip(T("Edit"));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = ui.SpacingSM;
    const float midY = rowPos.y + rowH * 0.5f;
    const float lineH = ImGui::GetTextLineHeight();
    const float textY = midY - lineH * 0.5f;

    // Колонки: имя — доля ширины (не меньше 140), справа вид и меню.
    const float nameW = std::max(140.0f * Sage::UI::Scale(), rowW * 0.22f);
    const float menuW = h;
    const char* kind = ActionTypeLabel(action.Type());
    const float kindW = ImGui::CalcTextSize(T("Vector2")).x + ui.SpacingSM;
    const float rightEdge = rowPos.x + rowW - pad;
    const float kindX = rightEdge - menuW - ui.SpacingXS - kindW;

    dl->AddText(ImVec2(rowPos.x + pad, textY), ImGui::GetColorU32(ImGuiCol_Text),
                Sage::UI::Truncate(action.Name().c_str(), nameW - pad).c_str());
    dl->AddText(ImVec2(kindX, textY), ImGui::GetColorU32(ImGuiCol_TextDisabled), kind);

    // КЕЙКАПЫ — ГЛАВНОЕ В СТРОКЕ; у действия без них строка говорит об этом
    // словами: пустое место читается как «просто не поместилось».
    const float capH = lineH + ui.SpacingXS * 1.5f;
    float x = rowPos.x + pad + nameW;
    const float limit = kindX - ui.SpacingSM;
    const auto& bindings = action.Bindings();
    if (bindings.empty()) {
        dl->AddText(ImVec2(x, textY), ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Warn)),
                    T("no key assigned"));
    }
    for (size_t i = 0; i < bindings.size(); ++i) {
        const std::string label = bindings[i].ToString();
        const bool conflict = !ConflictWith(input, bindings[i], &action).empty();
        const float w = KeycapWidth(label.c_str());
        const std::string more = "+" + std::to_string(bindings.size() - i);
        const bool last = i + 1 == bindings.size();
        // Не влезает — остаток сворачивается в «+N», а не обрезается посреди.
        if (x + w > limit - (last ? 0.0f : KeycapWidth(more.c_str()) + ui.SpacingXS)) {
            if (x + KeycapWidth(more.c_str()) <= limit)
                DrawKeycap(dl, ImVec2(x, midY - capH * 0.5f), capH, more.c_str(), false);
            break;
        }
        DrawKeycap(dl, ImVec2(x, midY - capH * 0.5f), capH, label.c_str(), conflict);
        x += w + ui.SpacingXS;
    }

    // Меню строки — кнопкой поверх Selectable.
    ImGui::SetCursorScreenPos(ImVec2(rightEdge - menuW, rowPos.y + (rowH - h) * 0.5f));
    ImGui::PushID("##rowmenu");
    if (EditorIcons::IconOnlyButton("dots", T("More"))) ImGui::OpenPopup("##rowMenu");
    if (Sage::UI::MenuScope rowMenu; ImGui::BeginPopup("##rowMenu")) {
        if (EditorIcons::MenuItem("pencil", T("Edit"))) OpenEditAction(input, {ctx.Name(), action.Name()});
        const std::vector<Context*> contexts = input.ContextsByPriority();
        if (contexts.size() > 1 && ImGui::BeginMenu(T("Move to context"))) {
            for (Context* c : contexts) {
                if (c == &ctx) continue;
                if (ImGui::MenuItem(c->Name().c_str())) {
                    // Правила переноса — в движке (MoveAction): совпавшее имя
                    // в целевом контексте не затирается молча.
                    if (input.MoveAction(ctx.Name(), c->Name(), action.Name())) {
                        host.SetProjectInputDirty(true);
                        host.SetStatusMessage(T("Actions moved"));
                    } else {
                        host.SetStatusMessage(T("Nothing moved: the names are taken there"));
                    }
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (EditorIcons::MenuItem("trash", T("Delete this action"))) {
            ctx.Remove(action.Name());
            host.SetProjectInputDirty(true);
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(rowPos.x, rowPos.y + rowH));
}

// --- СПИСОК --------------------------------------------------------------
//
// ОДИН список на все контексты, на всю ширину окна. Заголовок группы: шеврон,
// имя, число действий, меню контекста.
void InputPanel::DrawActionList(EditorHost& host, InputSystem& input) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    int shownTotal = 0;
    for (Context* ctx : input.ContextsByPriority()) {
        const std::vector<std::string> names = ctx->ActionNames();
        std::vector<std::string> matched;
        for (const std::string& n : names)
            if (ContainsCaseInsensitive(n, m_search)) matched.push_back(n);
        shownTotal += (int)matched.size();

        // ПРИ ПОИСКЕ ПУСТЫЕ ГРУППЫ НЕ ПОКАЗЫВАЮТСЯ: заголовки без строк — ложные следы.
        if (m_search[0] != '\0' && matched.empty()) continue;

        ImGui::PushID(ctx->Name().c_str());
        const bool collapsed = IsCollapsed(ctx->Name()) && m_search[0] == '\0';

        ImGui::PushID("##fold");
        if (EditorIcons::IconOnlyButton(collapsed ? "chevron-right" : "chevron-down",
                                        collapsed ? T("Expand") : T("Collapse")))
            ToggleCollapsed(ctx->Name());
        ImGui::PopID();
        ImGui::SameLine(0.0f, ui.SpacingXS);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ctx->Name().c_str());
        ImGui::SameLine(0.0f, ui.SpacingSM);
        Sage::UI::Badge(std::to_string((int)names.size()).c_str(), EditorTheme::Role::TextDim);

        const float menuW = ImGui::GetFrameHeight();
        ImGui::SameLine(ImGui::GetCursorPosX() +
                        std::max(0.0f, ImGui::GetContentRegionAvail().x - menuW - ui.SpacingSM));
        if (EditorIcons::IconOnlyButton("dots", T("Context settings")))
            ImGui::OpenPopup("##ctxSettings");
        DrawContextMenu(host, input, *ctx);

        if (!collapsed) {
            for (const std::string& name : matched) {
                Action* action = ctx->Find(name);
                if (!action) continue;
                ImGui::PushID(name.c_str());
                DrawActionRow(host, input, *ctx, *action);
                ImGui::PopID();
            }
            if (matched.empty())
                Sage::UI::TextSecondary("   %s", T("No actions in this context yet"));
        }
        ImGui::Spacing();
        ImGui::PopID();
    }

    if (shownTotal == 0) {
        Sage::UI::EmptyState(m_search[0] ? T("Nothing matches") : T("No actions yet"),
                             m_search[0] ? T("Try another search.")
                                         : T("Press «Add Action» above: name it, and the editor will "
                                             "ask for the key right away."));
    }
}

void InputPanel::DrawBody(EditorHost& host) {
    InputSystem& input = host.ProjectInput();

    DrawHeader(host, input);
    Sage::UI::Separator();

    // Один список на всю ширину; прокрутка — только его.
    ImGui::BeginChild("##list", ImVec2(0, 0), false);
    DrawActionList(host, input);
    ImGui::EndChild();

    // --- Окна поверх ---------------------------------------------------------
    DrawActionModal(host, input);
    DrawNewContextModal(host, input);
}
