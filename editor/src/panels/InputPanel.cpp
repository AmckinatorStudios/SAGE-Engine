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

// --- ВЫДЕЛЕНИЕ -----------------------------------------------------------

bool InputPanel::IsSelected(const Ref& ref) const {
    return std::find(m_selection.begin(), m_selection.end(), ref) != m_selection.end();
}

void InputPanel::SelectOnly(const Ref& ref) {
    m_selection.clear();
    m_selection.push_back(ref);
    m_anchor = ref;
    m_selectedContext = ref.Context;
    m_selectedAction = ref.Name;
}

void InputPanel::ToggleSelected(const Ref& ref) {
    auto it = std::find(m_selection.begin(), m_selection.end(), ref);
    if (it != m_selection.end()) {
        m_selection.erase(it);
        // Свойства показывают ПЕРВОЕ выделенное: сняли то, что показано —
        // показываем следующее, а не пустоту при непустом наборе.
        if (m_selectedContext == ref.Context && m_selectedAction == ref.Name) {
            if (m_selection.empty()) { m_selectedContext.clear(); m_selectedAction.clear(); }
            else { m_selectedContext = m_selection.front().Context;
                   m_selectedAction = m_selection.front().Name; }
        }
        return;
    }
    m_selection.push_back(ref);
    m_anchor = ref;
    m_selectedContext = ref.Context;
    m_selectedAction = ref.Name;
}

void InputPanel::SelectRange(const std::vector<Ref>& visible, const Ref& to) {
    auto findIndex = [&visible](const Ref& r) {
        for (size_t i = 0; i < visible.size(); ++i)
            if (visible[i] == r) return (int)i;
        return -1;
    };
    const int from = findIndex(m_anchor);
    const int target = findIndex(to);
    if (target < 0) return;
    // Якоря нет (его строку спрятали поиском или свернули группу) — значит и
    // диапазона нет: тянуть его от невидимого некуда.
    if (from < 0) { SelectOnly(to); return; }

    m_selection.clear();
    const int a = std::min(from, target);
    const int b = std::max(from, target);
    for (int i = a; i <= b; ++i) m_selection.push_back(visible[(size_t)i]);
    m_selectedContext = to.Context;
    m_selectedAction = to.Name;
}

void InputPanel::DeleteSelection(EditorHost& host, InputSystem& input) {
    if (m_selection.empty()) return;
    for (const Ref& ref : m_selection) {
        if (Context* ctx = input.FindContext(ref.Context)) ctx->Remove(ref.Name);
    }
    m_selection.clear();
    m_selectedContext.clear();
    m_selectedAction.clear();
    host.SetProjectInputDirty(true);
}

int InputPanel::MoveSelectionTo(EditorHost& host, InputSystem& input,
                                const std::string& targetContext) {
    if (!input.FindContext(targetContext)) return 0;

    int moved = 0;
    std::vector<Ref> stayed;
    for (const Ref& ref : m_selection) {
        if (ref.Context == targetContext) { stayed.push_back(ref); continue; }
        // Сам перенос — в движке (InputSystem::MoveAction): о правилах, при
        // которых он невозможен, должен знать один.
        if (!input.MoveAction(ref.Context, targetContext, ref.Name)) {
            stayed.push_back(ref);
            continue;
        }
        stayed.push_back({targetContext, ref.Name});
        ++moved;
    }
    m_selection = stayed;
    if (!m_selection.empty()) {
        m_selectedContext = m_selection.front().Context;
        m_selectedAction = m_selection.front().Name;
    }
    if (moved > 0) host.SetProjectInputDirty(true);
    return moved;
}

// --- СВОЙСТВА НАБОРА -----------------------------------------------------
//
// Когда выделено больше одного, свойства одного действия показывать нельзя:
// человек выделял набор, чтобы сделать что-то со ВСЕМ набором. Показываем, что
// именно с ним можно сделать, и перечисляем, что в нём лежит.
void InputPanel::DrawMultiSelection(EditorHost& host, InputSystem& input) {
    char title[96];
    std::snprintf(title, sizeof(title), T("Selected: %d"), (int)m_selection.size());
    ImGui::TextUnformatted(title);
    Sage::UI::TextSecondary("%s", T("Drag them onto a context header to move, or use the buttons."));
    Sage::UI::Separator();

    std::vector<Context*> contexts = input.ContextsByPriority();
    if (contexts.size() > 1) {
        Sage::UI::BeginProperties("moveto");
        Sage::UI::PropertyLabel(T("Move to context"));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##moveto", T("Choose..."))) {
            for (Context* c : contexts) {
                if (ImGui::Selectable(c->Name().c_str())) {
                    const int moved = MoveSelectionTo(host, input, c->Name());
                    host.SetStatusMessage(moved > 0 ? T("Actions moved") 
                                                    : T("Nothing moved: the names are taken there"));
                }
            }
            ImGui::EndCombo();
        }
        Sage::UI::EndProperties();
        ImGui::Spacing();
    }

    if (Sage::UI::Button(T("Delete selected"), Sage::UI::ButtonStyle::Danger, ImVec2(-1, 0)))
        DeleteSelection(host, input);
    Sage::UI::Tooltip(T("Delete removes them too"));

    Sage::UI::Separator();
    ImGui::BeginChild("##sellist", ImVec2(0, 0));
    for (const Ref& ref : m_selection) {
        Sage::UI::TextSecondary("%s", ref.Context.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(ref.Name.c_str());
    }
    ImGui::EndChild();
}


// --- ШАПКА ---------------------------------------------------------------
//
// Поиск и «Действие» — то, чем работают каждый раз, и стоят они там, где рука
// ищет их первой. Сохранение, перечитывание и типовая раскладка — рядом, но
// значками: их нажимают раз за сеанс, и подписи они отняли бы у поиска.
void InputPanel::DrawHeader(EditorHost& host, InputSystem& input) {
    const Sage::UI::Style& ui = Sage::UI::Get();

    const float addW = 150.0f;
    const float iconW = ImGui::GetFrameHeight();
    const float tail = addW + (iconW + ui.SpacingXS) * 3.0f + ui.SpacingSM;
    const float searchW = std::max(120.0f, ImGui::GetContentRegionAvail().x - tail);

    // Ширина — параметром, а не SetNextItemWidth: поле ставит её себе само
    // (см. Sage::UI::SearchField), и наша просьба до него не доходила — поиск
    // занимал всю строку и выталкивал кнопку «Действие» за край окна.
    Sage::UI::SearchField("##search", m_search, sizeof(m_search), T("Search actions..."), searchW);

    ImGui::SameLine(0.0f, ui.SpacingSM);
    if (Sage::UI::Button(T("Action"), Sage::UI::ButtonStyle::Primary, ImVec2(addW, 0))) {
        m_newActionOpen = true;
        if (m_newActionContext.empty()) {
            m_newActionContext = m_selectedContext.empty() ? std::string(sage::input::kDefaultContext)
                                                           : m_selectedContext;
        }
    }
    Sage::UI::Tooltip(T("Create an action and assign a key to it"));

    ImGui::SameLine(0.0f, ui.SpacingXS);
    if (EditorIcons::IconOnlyButton("save", T("Save to the project (input.sageinput)"),
                                    host.ProjectInputDirty())) {
        if (host.SaveProjectInput()) host.SetStatusMessage(T("Controls saved: input.sageinput"));
        else host.SetStatusMessage(T("Controls not saved — no project open?"));
    }
    ImGui::SameLine(0.0f, ui.SpacingXS);
    if (EditorIcons::IconOnlyButton("refresh", T("Reread from disk, losing unsaved edits"))) {
        if (host.ReloadProjectInput()) host.SetStatusMessage(T("Controls reloaded from disk"));
    }
    ImGui::SameLine(0.0f, ui.SpacingXS);
    if (EditorIcons::IconOnlyButton("dots", T("More"))) ImGui::OpenPopup("##inputMore");

    if (Sage::UI::MenuScope moreMenu; ImGui::BeginPopup("##inputMore")) {
        if (EditorIcons::MenuItem("plus", T("New context..."))) m_newContextOpen = true;
        ImGui::Separator();
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

// --- НАСТРОЙКИ КОНТЕКСТА -------------------------------------------------
//
// Приоритет, отбор устройств и удаление — в меню у заголовка группы, а не на
// экране всегда. Их трогают при заведении контекста и больше не вспоминают, а
// места на экране они занимали ровно столько же, сколько список действий.
void InputPanel::DrawContextMenu(EditorHost& host, Context& ctx) {
    if (Sage::UI::MenuScope ctxMenu; ImGui::BeginPopup("##ctxSettings")) {
        Sage::UI::MenuSection(ctx.Name().c_str(), true);
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
                host.ProjectInput().RemoveContext(ctx.Name());
                if (m_selectedContext == ctx.Name()) {
                    m_selectedContext.clear();
                    m_selectedAction.clear();
                }
                host.SetProjectInputDirty(true);
            }
        }
        ImGui::EndPopup();
    }
}

// --- СТРОКА ДЕЙСТВИЯ -----------------------------------------------------
//
// Две строки: имя сверху, назначенные клавиши снизу. Отвечает ровно на вопрос,
// с которым в список и смотрят, — «что это и на чём висит». Всё остальное
// живёт в свойствах справа, у ОДНОГО выбранного действия.
bool InputPanel::DrawActionRow(EditorHost& host, const Action& action, bool selected) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    const float lineH = ImGui::GetTextLineHeight();
    const float rowH = lineH * 2.0f + ui.SpacingXS * 2.0f;

    const ImVec2 rowPos = ImGui::GetCursorScreenPos();
    const float rowW = ImGui::GetContentRegionAvail().x;
    const bool clicked =
        ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None, ImVec2(rowW, rowH));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = ui.SpacingSM;
    const float textY = rowPos.y + ui.SpacingXS;

    // Вид и конфликт — у правого края: их читают взглядом вдоль края, а не
    // вместе с именем.
    const char* kindText = ActionTypeShort(action.Type());
    const ImVec2 kindSize = ImGui::CalcTextSize(kindText);
    const bool conflict = HasAnyConflict(host.ProjectInput(), action);
    const char* warn = T("conflict");
    const ImVec2 warnSize = conflict ? ImGui::CalcTextSize(warn) : ImVec2(0.0f, 0.0f);
    const float rightLimit = rowPos.x + rowW - pad - std::max(kindSize.x, warnSize.x) - ui.SpacingSM;

    dl->AddText(ImVec2(rowPos.x + pad, textY), ImGui::GetColorU32(ImGuiCol_Text),
                action.Name().c_str());
    dl->AddText(ImVec2(rowPos.x + rowW - pad - kindSize.x, textY),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), kindText);

    // КЛАВИШИ — ГЛАВНОЕ В СТРОКЕ, и у действия без них строка обязана говорить
    // об этом словами: пустое место читается как «просто не поместилось».
    const std::string keys = BindingSummary(action);
    const ImU32 keyColor = keys.empty()
                               ? ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Warn))
                               : ImGui::GetColorU32(ImGuiCol_Text);
    const std::string shown =
        Sage::UI::Truncate(keys.empty() ? T("no key assigned") : keys.c_str(),
                           std::max(40.0f, rightLimit - rowPos.x - pad));
    dl->AddText(ImVec2(rowPos.x + pad, textY + lineH), keyColor, shown.c_str());

    if (conflict) {
        dl->AddText(ImVec2(rowPos.x + rowW - pad - warnSize.x, textY + lineH),
                    ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Warn)), warn);
    }
    return clicked;
}

// --- СПИСОК --------------------------------------------------------------
//
// ОДИН список на все контексты, а не колонка контекстов плюс колонка действий.
// Контекстов в игре два-три, и отдельная колонка под них стоила целой трети
// окна ради выбора из трёх строк. Заголовок группы отвечает на тот же вопрос и
// стоит там, где он возникает, — над действиями, которые к нему относятся.
void InputPanel::DrawActionList(EditorHost& host, InputSystem& input) {
    std::vector<Context*> contexts = input.ContextsByPriority();

    // ПОКАЗАННЫЙ порядок строк — для Shift-диапазона. Он зависит от поиска и
    // свёрнутых групп, и считать диапазон по хранимому порядку значило бы
    // выделять то, чего на экране нет.
    std::vector<Ref> visible;

    int shownTotal = 0;
    for (Context* ctx : contexts) {
        const std::vector<std::string> names = ctx->ActionNames();
        std::vector<std::string> matched;
        for (const std::string& n : names)
            if (ContainsCaseInsensitive(n, m_search)) matched.push_back(n);
        shownTotal += (int)matched.size();

        // ПРИ ПОИСКЕ ПУСТЫЕ ГРУППЫ НЕ ПОКАЗЫВАЮТСЯ: три заголовка без строк —
        // это три ложных следа, а не сведения о том, где искать дальше.
        if (m_search[0] != '\0' && matched.empty()) continue;

        ImGui::PushID(ctx->Name().c_str());
        const bool collapsed = IsCollapsed(ctx->Name());

        // Заголовок группы: стрелка, имя, число действий, меню настроек.
        // Стрелка — родная ImGui: у набора значков редактора нет «вниз», а
        // рисовать её седьмым способом ради одного заголовка незачем.
        if (ImGui::ArrowButton("##fold", collapsed ? ImGuiDir_Right : ImGuiDir_Down))
            ToggleCollapsed(ctx->Name());
        Sage::UI::Tooltip(collapsed ? T("Expand") : T("Collapse"));
        ImGui::SameLine(0.0f, Sage::UI::Get().SpacingXS);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ctx->Name().c_str());

        // ЗАГОЛОВОК — ЦЕЛЬ ПЕРЕТАСКИВАНИЯ. Перенести действие в другой контекст
        // иначе можно было только пересоздав его там руками: имя, вид и все
        // клавиши заново. Бросок на имя группы отвечает ровно на тот вопрос,
        // который человек и задаёт: «пусть это живёт вон там».
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_INPUT_ACTIONS")) {
                (void)p;
                const int moved = MoveSelectionTo(host, input, ctx->Name());
                host.SetStatusMessage(moved > 0
                                          ? T("Actions moved")
                                          : T("Nothing moved: the names are taken there"));
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::SameLine(0.0f, Sage::UI::Get().SpacingSM);
        Sage::UI::Badge(std::to_string((int)names.size()).c_str(), EditorTheme::Role::TextDim);
        const float gearW = ImGui::GetFrameHeight();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x >= gearW + 8.0f
                            ? ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - gearW
                            : ImGui::GetCursorPosX());
        if (EditorIcons::IconOnlyButton("gear", T("Context settings")))
            ImGui::OpenPopup("##ctxSettings");
        DrawContextMenu(host, *ctx);

        if (!collapsed) {
            for (const std::string& name : matched) {
                Action* action = ctx->Find(name);
                if (!action) continue;
                const Ref ref{ctx->Name(), name};
                visible.push_back(ref);

                ImGui::PushID(name.c_str());
                if (DrawActionRow(host, *action, IsSelected(ref))) {
                    const ImGuiIO& io = ImGui::GetIO();
                    if (io.KeyShift) SelectRange(visible, ref);
                    else if (io.KeyCtrl) ToggleSelected(ref);
                    else SelectOnly(ref);
                }

                // ТАЩИМ ВЕСЬ НАБОР, а не строку под курсором: человек выделил
                // девять боевых команд именно затем, чтобы перенести девять.
                // Строка вне набора тащит сама себя — и заодно становится
                // выделением, иначе перенос сработал бы не с тем, что видно.
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
                    if (!IsSelected(ref)) SelectOnly(ref);
                    ImGui::SetDragDropPayload("SAGE_INPUT_ACTIONS", "", 0);
                    char label[128];
                    std::snprintf(label, sizeof(label), T("Move: %d"), (int)m_selection.size());
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(label);
                    ImGui::EndTooltip();
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
            }
            if (matched.empty()) {
                Sage::UI::TextSecondary("   %s", T("No actions in this context yet"));
            }
        }
        ImGui::Spacing();
        ImGui::PopID();
    }

    if (shownTotal == 0) {
        Sage::UI::EmptyState(m_search[0] ? T("Nothing matches") : T("No actions yet"),
                             m_search[0] ? T("Try another search.")
                                         : T("Press «Action» above: name it, and the editor will "
                                             "ask for the key right away."));
    }

    // DELETE РАБОТАЕТ ПО СПИСКУ, а не по всему окну: пока печатают в поиске или
    // в поле имени, эта клавиша принадлежит полю.
    if (!m_selection.empty() && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
        !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        DeleteSelection(host, input);
    }
}


void InputPanel::DrawBody(EditorHost& host) {
    InputSystem& input = host.ProjectInput();

    // Контекст по умолчанию для новых действий — тот, в котором человек сейчас
    // смотрит: чаще всего заводят соседа тому, на что смотрят.
    if (m_newActionContext.empty()) m_newActionContext = sage::input::kDefaultContext;

    DrawHeader(host, input);
    Sage::UI::Separator();

    // ПЕРВОЕ ДЕЙСТВИЕ ВЫБИРАЕТСЯ САМО. Иначе, открыв окно, человек видит
    // список слева и пустоту справа — и обязан ДОГАДАТЬСЯ, что по списку надо
    // щёлкнуть, чтобы окно показало хоть что-то.
    if (m_selection.empty()) {
        for (Context* c : input.ContextsByPriority()) {
            const std::vector<std::string>& names = c->ActionNames();
            if (names.empty()) continue;
            SelectOnly({c->Name(), names.front()});
            break;
        }
    }

    // --- ДВЕ КОЛОНКИ: список слева, свойства выбранного справа --------------
    //
    // Третьей колонки (контекстов) больше нет: контекстов в игре два-три, и
    // отдельный столбец под выбор из трёх строк стоил трети окна. Теперь они —
    // заголовки групп в том же списке.
    const float sideW = std::floor(340.0f * Sage::UI::Scale());
    const float listW = std::max(240.0f, ImGui::GetContentRegionAvail().x - sideW -
                                             Sage::UI::Get().SpacingSM);

    ImGui::BeginChild("##list", ImVec2(listW, 0), true);
    DrawActionList(host, input);
    ImGui::EndChild();

    ImGui::SameLine(0.0f, Sage::UI::Get().SpacingSM);

    ImGui::BeginChild("##props", ImVec2(0, 0), true);
    if (m_selection.size() > 1) {
        DrawMultiSelection(host, input);
    } else {
        Context* ctx = m_selectedContext.empty() ? nullptr : input.FindContext(m_selectedContext);
        Action* action = ctx && !m_selectedAction.empty() ? ctx->Find(m_selectedAction) : nullptr;
        if (!action) {
            Sage::UI::EmptyState(T("No action selected"),
                                 T("Pick one on the left — its keys and settings appear here."));
        } else if (DrawActionProperties(host, *action, ctx->Name())) {
            ctx->Remove(m_selectedAction);
            m_selection.clear();
            m_selectedAction.clear();
            host.SetProjectInputDirty(true);
        }
    }
    ImGui::EndChild();

    // --- Окна поверх ---------------------------------------------------------
    DrawNewActionModal(host, input);
    DrawNewContextModal(host, input);

    Binding caught;
    if (DrawCaptureModal(host, caught)) {
        Context* target = input.FindContext(m_captureContext);
        Action* targetAction = target ? target->Find(m_captureAction) : nullptr;
        if (targetAction) {
            if (m_captureBinding >= 0 && m_captureBinding < (int)targetAction->Bindings().size()) {
                // Переназначение сохраняет вклад и половину вектора: человек
                // менял КЛАВИШУ, а не роль привязки в оси.
                const Binding& old = targetAction->Bindings()[(size_t)m_captureBinding];
                caught.Scale = old.Scale;
                caught.Axis = old.Axis;
                targetAction->MutableBindings()[(size_t)m_captureBinding] = caught;
            } else {
                targetAction->Bind(caught);
            }
            host.SetProjectInputDirty(true);
        }
    }
}
