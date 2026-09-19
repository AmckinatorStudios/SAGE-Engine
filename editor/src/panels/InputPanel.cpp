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

namespace {

// Подписи видов действий. Порядок совпадает с ActionType — на этом держится
// перевод индекса комбо-бокса обратно в вид, и менять его нельзя.
const char* ActionTypeLabel(ActionType type) {
    switch (type) {
        case ActionType::Digital: return T("Button (pressed / not pressed)");
        case ActionType::Axis:    return T("Axis (-1 .. 1)");
        case ActionType::Vector:  return T("Vector (two axes)");
    }
    return "";
}

// Короткая подпись — для бейджа в строке списка, где полная не влезает.
const char* ActionTypeShort(ActionType type) {
    switch (type) {
        case ActionType::Digital: return T("Button");
        case ActionType::Axis:    return T("Axis");
        case ActionType::Vector:  return T("Vector");
    }
    return "";
}

const char* TriggerLabel(TriggerMode mode) {
    switch (mode) {
        case TriggerMode::Press:   return T("On press");
        case TriggerMode::Release: return T("On release");
        case TriggerMode::Hold:    return T("On hold");
        case TriggerMode::Tap:     return T("On short tap");
    }
    return "";
}

// Значок устройства привязки. Рисунком, а не словом: в строке списка клавиш
// пять штук, и «Клавиатура, Клавиатура, Мышь» занимает больше места, чем сами
// клавиши, ради сведения, которое видно по значку без чтения.
const char* DeviceIcon(const Binding& b) {
    switch (b.Kind) {
        case SourceKind::Key: return "keyboard";
        case SourceKind::MouseButton:
        case SourceKind::MouseWheel:
        case SourceKind::MouseAxis: return "select";
        case SourceKind::GamepadButton:
        case SourceKind::GamepadAxis: return "pilot";
        case SourceKind::None: return "question";
    }
    return "question";
}

// Из события ввода — привязка, которую человек только что нажал. Пусто, если
// событие не про назначение (движение мыши, отпускание, набранный символ).
//
// Движение мыши сюда НЕ попадает намеренно: назначить «ось мыши» ловлей
// нельзя — курсор шевелится всегда, и первая же дрожь руки заняла бы поле.
// Оси мыши и стиков назначаются списком из восьми пунктов, где выбор осмыслен.
bool BindingFromEvent(const InputEvent& e, Binding& out) {
    switch (e.Type) {
        case InputEventType::KeyPressed: {
            // Сам модификатор действием не назначают: «действие на Shift»
            // почти всегда означает, что человек ещё не дожал сочетание.
            switch (e.Keyboard) {
                case sage::input::Key::LeftShift:
                case sage::input::Key::RightShift:
                case sage::input::Key::LeftControl:
                case sage::input::Key::RightControl:
                case sage::input::Key::LeftAlt:
                case sage::input::Key::RightAlt:
                case sage::input::Key::LeftSuper:
                case sage::input::Key::RightSuper:
                    return false;
                default: break;
            }
            out = Binding::OfKey(e.Keyboard, e.Modifiers);
            return true;
        }
        case InputEventType::MouseButtonPressed:
            out = Binding::OfMouse(e.Button, e.Modifiers);
            return true;
        case InputEventType::MouseWheel:
            out = (e.Wheel > 0.0f) ? Binding::WheelUp() : Binding::WheelDown();
            return true;
        case InputEventType::GamepadButtonPressed:
            out = Binding::OfPadButton(e.PadButton);
            return true;
        default:
            return false;
    }
}

// Подстрока без учёта регистра — тот же приём, что и в поиске других панелей.
bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

// Все клавиши действия одной строкой — для строки списка.
std::string BindingSummary(const Action& action) {
    std::string out;
    for (const Binding& b : action.Bindings()) {
        if (!out.empty()) out += "   ";
        out += b.ToString();
    }
    return out;
}

// Оси, которые нельзя поймать нажатием (см. BindingFromEvent). Список общий у
// свойств действия и у модалки создания: один и тот же набор, написанный
// дважды, однажды разойдётся.
struct AxisItem { const char* Label; Binding (*Make)(); };
const AxisItem kAxes[] = {
    {"MOUSE_X", [] { return Binding::MouseAxisX(); }},
    {"MOUSE_Y", [] { return Binding::MouseAxisY(); }},
    {"PAD_LEFT_X", [] { return Binding::OfPadAxis(sage::input::GamepadAxis::LeftX); }},
    {"PAD_LEFT_Y", [] { return Binding::OfPadAxis(sage::input::GamepadAxis::LeftY); }},
    {"PAD_RIGHT_X", [] { return Binding::OfPadAxis(sage::input::GamepadAxis::RightX); }},
    {"PAD_RIGHT_Y", [] { return Binding::OfPadAxis(sage::input::GamepadAxis::RightY); }},
    {"PAD_LEFT_TRIGGER", [] { return Binding::OfPadAxis(sage::input::GamepadAxis::LeftTrigger); }},
    {"PAD_RIGHT_TRIGGER", [] { return Binding::OfPadAxis(sage::input::GamepadAxis::RightTrigger); }},
};

} // namespace

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

// --- ЛОВЛЯ --------------------------------------------------------------
//
// Окно ловли намеренно пустое: в нём нечего читать, кроме одной строки, и
// нечего нажимать, кроме отмены. Всё, что человек должен сделать, — нажать ту
// клавишу, которую он и хотел назначить.
bool InputPanel::DrawCaptureModal(EditorHost& host, Binding& caught) {
    if (!m_captureOpen) return false;

    ImGui::OpenPopup(T("Press a control###AssignControl"));
    ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_Always);
    bool captured = false;
    if (ImGui::BeginPopupModal(T("Press a control###AssignControl"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings)) {
        // Кому именно назначаем — написано прямо здесь: модалка перекрывает
        // список, и «а к чему это окно» иначе приходится вспоминать.
        Sage::UI::TextSecondary("%s", m_captureAction.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted(T("Press a key, a mouse button, the wheel or a gamepad button."));
        Sage::UI::TextSecondary("%s", T("Hold Ctrl / Shift / Alt for a combination. Escape cancels."));
        ImGui::Spacing();

        for (const InputEvent& e : host.FrameInputEvents()) {
            // Escape отменяет, а не назначается: иначе первым же действием
            // человек назначил бы Escape на то, что пытался отменить.
            if (e.Type == InputEventType::KeyPressed && e.Keyboard == sage::input::Key::Escape) {
                m_captureOpen = false;
                ImGui::CloseCurrentPopup();
                break;
            }
            if (BindingFromEvent(e, caught)) {
                captured = true;
                m_captureOpen = false;
                ImGui::CloseCurrentPopup();
                break;
            }
        }

        if (Sage::UI::Button(T("Cancel"), Sage::UI::ButtonStyle::Secondary, ImVec2(120, 0))) {
            m_captureOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return captured;
}

// --- НОВОЕ ДЕЙСТВИЕ -----------------------------------------------------
//
// Одно окно вместо трёх полей внизу колонки. Спрашивается ровно то, без чего
// действия не бывает: как его зовут. Вид и контекст стоят готовыми ответами —
// кнопка и текущий контекст, — и трогают их редко.
//
// Сразу после создания открывается ловля: действие без клавиши не срабатывает
// никогда, и оставить человека перед пустой строкой значит предложить ему
// догадаться о втором шаге.
void InputPanel::DrawNewActionModal(EditorHost& host, InputSystem& input) {
    if (!m_newActionOpen) return;

    ImGui::OpenPopup(T("New action###NewAction"));
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal(T("New action###NewAction"), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    // Фокус в поле имени сразу: окно открыли, чтобы набрать имя, и лишний
    // щелчок по единственному полю — работа, которой не должно быть.
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    const bool entered = ImGui::InputTextWithHint("##name", T("Action name, e.g. Jump"),
                                                  m_newActionName, sizeof(m_newActionName),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);

    Sage::UI::BeginProperties("newaction");
    Sage::UI::PropertyLabel(T("Kind"),
                            T("A button answers yes/no, an axis gives -1..1, a vector gives two "
                              "axes at once — movement and look."));
    ImGui::SetNextItemWidth(-1);
    const char* types[] = {ActionTypeLabel(ActionType::Digital), ActionTypeLabel(ActionType::Axis),
                           ActionTypeLabel(ActionType::Vector)};
    ImGui::Combo("##kind", &m_newActionType, types, 3);

    // Контекст спрашивается только когда их БОЛЬШЕ ОДНОГО: у игры с единственным
    // контекстом этот выбор — поле без выбора.
    std::vector<Context*> contexts = input.ContextsByPriority();
    if (contexts.size() > 1) {
        Sage::UI::PropertyLabel(T("Context"),
                                T("A group of actions that work at the same time: the game, the "
                                  "inventory, a dialogue."));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##ctx", m_newActionContext.c_str())) {
            for (Context* c : contexts) {
                const bool selected = c->Name() == m_newActionContext;
                if (ImGui::Selectable(c->Name().c_str(), selected)) m_newActionContext = c->Name();
            }
            ImGui::EndCombo();
        }
    }
    Sage::UI::EndProperties();

    ImGui::Spacing();
    const bool named = m_newActionName[0] != '\0';
    ImGui::BeginDisabled(!named);
    const bool create = Sage::UI::Button(T("Create and assign a key"),
                                         Sage::UI::ButtonStyle::Primary, ImVec2(-1, 0));
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (Sage::UI::Button(T("Cancel"), Sage::UI::ButtonStyle::Secondary, ImVec2(-1, 0))) {
        m_newActionOpen = false;
        ImGui::CloseCurrentPopup();
    }

    if ((create || entered) && named) {
        Context* ctx = input.FindContext(m_newActionContext);
        if (!ctx && !contexts.empty()) ctx = contexts.front();
        if (ctx) {
            ctx->Add(m_newActionName, (ActionType)m_newActionType);
            m_selectedContext = ctx->Name();
            m_selectedAction = m_newActionName;
            host.SetProjectInputDirty(true);
            // Ловля — сразу, без ещё одного нажатия.
            m_captureContext = ctx->Name();
            m_captureAction = m_newActionName;
            m_captureBinding = -1;
            m_captureOpen = true;
        }
        m_newActionName[0] = '\0';
        m_newActionOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void InputPanel::DrawNewContextModal(EditorHost& host, InputSystem& input) {
    if (!m_newContextOpen) return;

    ImGui::OpenPopup(T("New context###NewContext"));
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal(T("New context###NewContext"), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    Sage::UI::TextSecondary("%s", T("One key means different things in the game, in the inventory "
                                    "and in a dialogue. Each set of them is a context."));
    ImGui::Spacing();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    const bool entered = ImGui::InputTextWithHint("##name", T("Context name, e.g. Inventory"),
                                                  m_newContextName, sizeof(m_newContextName),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    Sage::UI::BeginProperties("newcontext");
    Sage::UI::PropertyLabel(T("Priority"),
                            T("Higher priority takes a device away from contexts below it."));
    ImGui::SetNextItemWidth(-1);
    ImGui::DragInt("##prio", &m_newContextPriority, 1.0f, -1000, 1000);
    Sage::UI::EndProperties();

    ImGui::Spacing();
    const bool named = m_newContextName[0] != '\0';
    ImGui::BeginDisabled(!named);
    const bool create =
        Sage::UI::Button(T("Create"), Sage::UI::ButtonStyle::Primary, ImVec2(-1, 0));
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (Sage::UI::Button(T("Cancel"), Sage::UI::ButtonStyle::Secondary, ImVec2(-1, 0))) {
        m_newContextOpen = false;
        ImGui::CloseCurrentPopup();
    }
    if ((create || entered) && named) {
        input.CreateContext(m_newContextName, m_newContextPriority);
        m_selectedContext = m_newContextName;
        m_selectedAction.clear();
        m_newContextName[0] = '\0';
        m_newContextOpen = false;
        host.SetProjectInputDirty(true);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
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

// --- КЛАВИШИ ДЕЙСТВИЯ ----------------------------------------------------
//
// Плитками, а не таблицей на пять колонок. Таблица отвечала на пять вопросов
// сразу («источник, устройство, вклад, конфликт, удалить»), из которых человек
// задаёт один: какая клавиша. Плитка показывает клавишу и значок устройства;
// всё остальное — в меню по правой кнопке, там же, где и живёт.
void InputPanel::DrawControls(EditorHost& host, Action& action, const std::string& contextName) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    int removeAt = -1;

    for (int i = 0; i < (int)action.Bindings().size(); ++i) {
        const Binding& binding = action.Bindings()[(size_t)i];
        ImGui::PushID(i);

        const std::string label = binding.ToString();
        const std::string conflict = ConflictWith(host.ProjectInput(), binding, &action);
        // Спорная клавиша красится сразу: узнать, что она занята, надо в момент
        // назначения, а не когда игра повела себя странно.
        if (!conflict.empty())
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Warn));
        if (EditorIcons::Button(DeviceIcon(binding), label.c_str(),
                                T("Click to reassign, right-click for settings"))) {
            m_captureBinding = i;
            m_captureOpen = true;
            m_captureContext = contextName;
            m_captureAction = action.Name();
        }
        if (!conflict.empty()) ImGui::PopStyleColor();

        if (!conflict.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s %s\n%s", T("also:"), conflict.c_str(),
                              T("The same control is bound to another action. That is legal when "
                                "the two never run at the same time — they live in different "
                                "contexts."));
        }

        if (Sage::UI::MenuScope bindingMenu; ImGui::BeginPopupContextItem("##bindmenu")) {
            DrawBindingTuning(host, action, i);
            if (EditorIcons::MenuItem("trash", T("Remove this control"))) removeAt = i;
            ImGui::EndPopup();
        }

        // Плитки идут в строку, пока есть место: у действия их обычно две-три
        // (клавиша и кнопка геймпада), и столбец из двух строк занимал бы
        // высоту ради ничего.
        const float next = ImGui::GetItemRectMax().x + ui.SpacingXS + 90.0f;
        if (next < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
            ImGui::SameLine(0.0f, ui.SpacingXS);
        ImGui::PopID();
    }

    if (action.Bindings().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Warn));
        ImGui::TextUnformatted(T("No key assigned — the action can never fire"));
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    if (EditorIcons::Button("plus", T("Key"), T("Press a key, mouse button or gamepad button"))) {
        m_captureBinding = -1;   // −1 — добавить новую, а не заменить
        m_captureOpen = true;
        m_captureContext = contextName;
        m_captureAction = action.Name();
    }
    // Оси назначаются списком, а не ловлей: курсор шевелится всегда, и первая
    // же дрожь руки заняла бы поле (см. BindingFromEvent).
    if (action.Type() != ActionType::Digital) {
        ImGui::SameLine(0.0f, ui.SpacingXS);
        if (EditorIcons::Button("plus", T("Axis..."), T("A stick, trigger or the mouse as an axis")))
            ImGui::OpenPopup("##axisMenu");
        if (Sage::UI::MenuScope axisMenu; ImGui::BeginPopup("##axisMenu")) {
            for (const AxisItem& item : kAxes) {
                // Мышь и геймпад — разные рисунки: список из восьми ПРОПИСНЫХ
                // имён читается по буквам, а по рисунку видно семейство ещё до
                // чтения.
                const char* icon = std::strncmp(item.Label, "MOUSE", 5) == 0 ? "select" : "pilot";
                if (!EditorIcons::MenuItem(icon, item.Label)) continue;
                Binding b = item.Make();
                // Вторая ось векторного действия почти всегда Y — угадываем по
                // имени, чтобы не заставлять переставлять руками каждый раз.
                if (action.Type() == ActionType::Vector && std::strstr(item.Label, "_Y"))
                    b.Axis = Component::Y;
                action.Bind(b);
                host.SetProjectInputDirty(true);
            }
            ImGui::EndPopup();
        }
    }

    if (removeAt >= 0) {
        action.RemoveBindingAt(removeAt);
        host.SetProjectInputDirty(true);
    }
}

// Вклад и половина вектора — в меню у плитки. У кнопочного действия их нет
// вовсе: «вклад +1» у кнопки прыжка — поле, которое нечем объяснить.
void InputPanel::DrawBindingTuning(EditorHost& host, Action& action, int index) {
    if (action.Type() == ActionType::Digital) return;
    if (index < 0 || index >= (int)action.Bindings().size()) return;

    Sage::UI::MenuSection(T("This control"), true);
    Sage::UI::BeginProperties("tuning");
    Sage::UI::PropertyLabel(T("Contribution"),
                            T("Contribution to the value: W gives +1, S gives -1"));
    ImGui::SetNextItemWidth(120.0f);
    float scale = action.Bindings()[(size_t)index].Scale;
    if (ImGui::DragFloat("##scale", &scale, 0.05f, -4.0f, 4.0f, "%.2f")) {
        action.MutableBindings()[(size_t)index].Scale = scale;
        host.SetProjectInputDirty(true);
    }
    if (action.Type() == ActionType::Vector) {
        Sage::UI::PropertyLabel(T("Axis"), T("Which half of the vector this source feeds"));
        ImGui::SetNextItemWidth(120.0f);
        int axis = (action.Bindings()[(size_t)index].Axis == Component::Y) ? 1 : 0;
        const char* axes[] = {T("X"), T("Y")};
        if (ImGui::Combo("##axis", &axis, axes, 2)) {
            action.MutableBindings()[(size_t)index].Axis = axis ? Component::Y : Component::X;
            host.SetProjectInputDirty(true);
        }
    }
    Sage::UI::EndProperties();
    ImGui::Separator();
}

// --- ПОВЕДЕНИЕ -----------------------------------------------------------
//
// Свёрнуто по умолчанию, и это главное решение раздела: девять человек из
// десяти приходят сюда поменять клавишу, а не мёртвую зону. Раньше эти поля
// стояли на экране всегда — то есть человек, которому нужна была одна кнопка,
// каждый раз читал десяток чужих настроек, чтобы убедиться, что они не его.
void InputPanel::DrawBehavior(EditorHost& host, Action& action) {
    sage::input::ActionSettings& s = action.Settings();
    Sage::UI::BeginProperties("behavior");

    if (action.Type() == ActionType::Digital) {
        Sage::UI::PropertyLabel(T("Fires"));
        ImGui::SetNextItemWidth(-1);
        int trigger = (int)s.Trigger;
        const char* triggers[] = {TriggerLabel(TriggerMode::Press), TriggerLabel(TriggerMode::Release),
                                  TriggerLabel(TriggerMode::Hold), TriggerLabel(TriggerMode::Tap)};
        if (ImGui::Combo("##trigger", &trigger, triggers, 4)) {
            s.Trigger = (TriggerMode)trigger;
            host.SetProjectInputDirty(true);
        }

        if (s.Trigger == TriggerMode::Hold) {
            Sage::UI::PropertyLabel(T("Hold, sec"));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##hold", &s.HoldTime, 0.01f, 0.05f, 3.0f, "%.2f"))
                host.SetProjectInputDirty(true);
        }
        if (s.Trigger == TriggerMode::Tap) {
            Sage::UI::PropertyLabel(T("Tap shorter than, sec"));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##tap", &s.TapTime, 0.01f, 0.05f, 2.0f, "%.2f"))
                host.SetProjectInputDirty(true);
        }

        Sage::UI::PropertyLabel(T("Exact modifiers"),
                               T("Off: W keeps working while Shift is held — otherwise sprinting "
                                 "would break walking. On: for shortcuts, so Ctrl+S never fires "
                                 "from Ctrl+Shift+S."));
        if (ImGui::Checkbox("##exact", &s.ExactModifiers)) host.SetProjectInputDirty(true);
    } else {
        Sage::UI::PropertyLabel(T("Dead zone"),
                               T("Stick wobble around the centre is ignored. Does not touch the "
                                 "keyboard: a key is either pressed or not."));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat("##deadzone", &s.DeadZone, 0.01f, 0.0f, 0.9f, "%.2f"))
            host.SetProjectInputDirty(true);

        Sage::UI::PropertyLabel(T("Dead zone shape"),
                               T("Radial is the right choice for a stick: otherwise the diagonal "
                                 "leaves the zone earlier than a straight push."));
        ImGui::SetNextItemWidth(-1);
        int zone = (s.Zone == DeadZoneMode::Axial) ? 1 : 0;
        const char* zones[] = {T("Radial"), T("Per axis")};
        if (ImGui::Combo("##zone", &zone, zones, 2)) {
            s.Zone = zone ? DeadZoneMode::Axial : DeadZoneMode::Radial;
            host.SetProjectInputDirty(true);
        }

        Sage::UI::PropertyLabel(T("Smoothing, sec"),
                               T("0 is raw — the right choice for a first-person view: a smoothed "
                                 "look feels like the mouse is sinking."));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat("##smoothing", &s.Smoothing, 0.005f, 0.0f, 1.0f, "%.3f"))
            host.SetProjectInputDirty(true);

        Sage::UI::PropertyLabel(T("Sensitivity"));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat("##sensitivity", &s.Sensitivity, 0.01f, 0.01f, 10.0f, "%.2f"))
            host.SetProjectInputDirty(true);

        Sage::UI::PropertyLabel(T("Normalize diagonal"),
                               T("Without it diagonal movement is 1.41 times faster than straight "
                                 "— the oldest bug in games."));
        if (ImGui::Checkbox("##normalize", &s.Normalize)) host.SetProjectInputDirty(true);

        Sage::UI::PropertyLabel(T("Press threshold"),
                               T("From which value an analog source counts as pressed — a trigger "
                                 "used as a fire button."));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat("##pressthreshold", &s.PressThreshold, 0.01f, 0.05f, 1.0f, "%.2f"))
            host.SetProjectInputDirty(true);
    }
    Sage::UI::EndProperties();
}

// --- СВОЙСТВА ДЕЙСТВИЯ ---------------------------------------------------
bool InputPanel::DrawActionProperties(EditorHost& host, Action& action,
                                      const std::string& contextName) {
    bool deleteRequested = false;

    // Шапка: имя и контекст, в котором действие живёт. Контекст написан здесь,
    // потому что свойства открыты по щелчку в длинном списке, и «а где это
    // было» — вопрос, который возникает сразу.
    ImGui::TextUnformatted(action.Name().c_str());
    const float delW = ImGui::GetFrameHeight();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x >= delW + 8.0f
                        ? ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - delW
                        : ImGui::GetCursorPosX());
    if (EditorIcons::IconOnlyButton("trash", T("Delete this action"))) deleteRequested = true;
    Sage::UI::TextSecondary("%s", contextName.c_str());

    Sage::UI::Separator();

    // КЛАВИШИ — ПЕРВЫМИ. За ними сюда и приходят.
    Sage::UI::TextSecondary("%s", T("Keys"));
    ImGui::Spacing();
    DrawControls(host, action, contextName);

    Sage::UI::Separator();
    Sage::UI::BeginProperties("kind");
    Sage::UI::PropertyLabel(T("Kind"),
                            T("A button answers yes/no, an axis gives -1..1, a vector gives two "
                              "axes at once — movement and look."));
    ImGui::SetNextItemWidth(-1);
    int type = (int)action.Type();
    const char* types[] = {ActionTypeLabel(ActionType::Digital), ActionTypeLabel(ActionType::Axis),
                           ActionTypeLabel(ActionType::Vector)};
    if (ImGui::Combo("##kind", &type, types, 3)) {
        action.SetType((ActionType)type);
        host.SetProjectInputDirty(true);
    }
    Sage::UI::EndProperties();

    // Свёрнуто: см. DrawBehavior — это настройки для тех, кто их ищет.
    if (Sage::UI::Section(T("Behavior"), false)) DrawBehavior(host, action);

    return deleteRequested;
}

void InputPanel::Draw(EditorHost& host, bool& open) {
    if (!open) return;

    ImGui::SetNextWindowSize(ImVec2(940, 620), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(640, 380), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin(EditorIcons::WindowTitle("keyboard", T("Controls"), "Controls").c_str(),
                      &open)) {
        ImGui::End();
        return;
    }

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

    ImGui::End();
}
