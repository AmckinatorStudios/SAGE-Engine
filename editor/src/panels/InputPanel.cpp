#include "InputPanel.h"

#include <cfloat>
#include <cstring>
#include <vector>

#include <imgui.h>

#include "../EditorHost.h"
#include "../EditorTheme.h"
#include "../Localization.h"

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

const char* TriggerLabel(TriggerMode mode) {
    switch (mode) {
        case TriggerMode::Press:   return T("On press");
        case TriggerMode::Release: return T("On release");
        case TriggerMode::Hold:    return T("On hold");
        case TriggerMode::Tap:     return T("On short tap");
    }
    return "";
}

// Человеческое имя устройства привязки — для колонки «откуда».
const char* DeviceLabel(const Binding& b) {
    switch (b.Kind) {
        case SourceKind::Key: return T("Keyboard");
        case SourceKind::MouseButton:
        case SourceKind::MouseWheel:
        case SourceKind::MouseAxis: return T("Mouse");
        case SourceKind::GamepadButton:
        case SourceKind::GamepadAxis: return T("Gamepad");
        case SourceKind::None: return "";
    }
    return "";
}

// Из события ввода — привязка, которую человек только что нажал. Пусто, если
// событие не про назначение (движение мыши, отпускание, набранный символ).
//
// Движение мыши сюда НЕ попадает намеренно: назначить «ось мыши» ловлей
// нельзя — курсор шевелится всегда, и первая же дрожь руки заняла бы поле.
// Оси мыши и стиков назначаются кнопкой «ось…» рядом, списком из шести
// пунктов, где выбор осмыслен.
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

// Приглушённая поясняющая строка с переносом. Пояснения здесь длиннее строки,
// и без переноса обрезался бы ровно тот текст, ради которого их пишут.
void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

} // namespace

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

bool InputPanel::DrawCaptureModal(EditorHost& host, Binding& caught) {
    if (!m_captureOpen) return false;

    ImGui::OpenPopup(T("Assign a control###AssignControl"));
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Always);
    bool captured = false;
    if (ImGui::BeginPopupModal(T("Assign a control###AssignControl"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted(T("Press a key, a mouse button, the wheel or a gamepad button."));
        ImGui::Spacing();
        Hint(T("Hold Ctrl / Shift / Alt to record a combination. Escape cancels."));
        ImGui::Separator();

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

        if (ImGui::Button(T("Cancel"), ImVec2(120, 0))) {
            m_captureOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return captured;
}

bool InputPanel::DrawBinding(EditorHost& host, Action& action, int index,
                             const std::string& contextName) {
    const std::vector<Binding>& bindings = action.Bindings();
    if (index < 0 || index >= (int)bindings.size()) return false;
    const Binding& binding = bindings[(size_t)index];

    ImGui::PushID(index);
    bool remove = false;

    // Кнопка с самим источником — она же «переназначить». Это то, куда человек
    // и целится: он видит «SPACE» и хочет нажать по нему, чтобы поменять.
    const std::string label = binding.ToString();
    if (ImGui::Button(label.c_str(), ImVec2(180, 0))) {
        m_captureBinding = index;
        m_captureOpen = true;
        m_captureContext = contextName;
        m_captureAction = action.Name();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Click to reassign"));

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", DeviceLabel(binding));

    // Вклад и половина вектора — только там, где они имеют смысл. У обычной
    // кнопки этих полей нет вовсе: показывать «вклад +1» у прыжка значит
    // предлагать настроить то, чего у прыжка не бывает.
    if (action.Type() != ActionType::Digital) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        float scale = binding.Scale;
        if (ImGui::DragFloat(T("##scale"), &scale, 0.05f, -4.0f, 4.0f, "%.2f")) {
            action.MutableBindings()[(size_t)index].Scale = scale;
            host.SetProjectInputDirty(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Contribution to the value: W gives +1, S gives -1"));
    }
    if (action.Type() == ActionType::Vector) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        int axis = (binding.Axis == Component::Y) ? 1 : 0;
        const char* axes[] = {T("X"), T("Y")};
        if (ImGui::Combo(T("##axis"), &axis, axes, 2)) {
            action.MutableBindings()[(size_t)index].Axis = axis ? Component::Y : Component::X;
            host.SetProjectInputDirty(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Which half of the vector this source feeds"));
    }

    ImGui::SameLine();
    if (ImGui::SmallButton(T("Remove##binding"))) remove = true;

    // Конфликт показывается ЗДЕСЬ, а не при сохранении: узнать, что клавиша
    // занята, надо в момент назначения, а не когда игра повела себя странно.
    const std::string conflict = ConflictWith(host.ProjectInput(), binding, &action);
    if (!conflict.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.25f, 1.0f), "%s", T("also:"));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", conflict.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("The same control is bound to another action. That is "
                                      "legal when the two never run at the same time — they "
                                      "live in different contexts."));
    }

    ImGui::PopID();
    return remove;
}

void InputPanel::DrawAction(EditorHost& host, Context& context, Action& action,
                            const std::string& contextName) {
    sage::input::ActionSettings& s = action.Settings();

    // Вид действия. Менять его после назначения привязок законно: человек
    // начал с кнопки, потом понял, что нужна ось.
    ImGui::SetNextItemWidth(230);
    int type = (int)action.Type();
    const char* types[] = {ActionTypeLabel(ActionType::Digital), ActionTypeLabel(ActionType::Axis),
                           ActionTypeLabel(ActionType::Vector)};
    if (ImGui::Combo(T("Kind"), &type, types, 3)) {
        action.SetType((ActionType)type);
        host.SetProjectInputDirty(true);
    }

    if (action.Type() == ActionType::Digital) {
        ImGui::SetNextItemWidth(230);
        int trigger = (int)s.Trigger;
        const char* triggers[] = {TriggerLabel(TriggerMode::Press), TriggerLabel(TriggerMode::Release),
                                  TriggerLabel(TriggerMode::Hold), TriggerLabel(TriggerMode::Tap)};
        if (ImGui::Combo(T("Fires"), &trigger, triggers, 4)) {
            s.Trigger = (TriggerMode)trigger;
            host.SetProjectInputDirty(true);
        }
        if (s.Trigger == TriggerMode::Hold) {
            ImGui::SetNextItemWidth(160);
            if (ImGui::DragFloat(T("Hold, sec"), &s.HoldTime, 0.01f, 0.05f, 3.0f, "%.2f"))
                host.SetProjectInputDirty(true);
        }
        if (s.Trigger == TriggerMode::Tap) {
            ImGui::SetNextItemWidth(160);
            if (ImGui::DragFloat(T("Tap shorter than, sec"), &s.TapTime, 0.01f, 0.05f, 2.0f, "%.2f"))
                host.SetProjectInputDirty(true);
        }
        if (ImGui::Checkbox(T("Exact modifiers"), &s.ExactModifiers)) host.SetProjectInputDirty(true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Off: W keeps working while Shift is held — otherwise "
                                      "sprinting would break walking. On: for shortcuts, so "
                                      "Ctrl+S never fires from Ctrl+Shift+S."));
    } else {
        ImGui::SetNextItemWidth(160);
        if (ImGui::DragFloat(T("Dead zone"), &s.DeadZone, 0.01f, 0.0f, 0.9f, "%.2f"))
            host.SetProjectInputDirty(true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Stick wobble around the centre is ignored. Does not "
                                      "touch the keyboard: a key is either pressed or not."));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        int zone = (s.Zone == DeadZoneMode::Axial) ? 1 : 0;
        const char* zones[] = {T("Radial"), T("Per axis")};
        if (ImGui::Combo(T("##zone"), &zone, zones, 2)) {
            s.Zone = zone ? DeadZoneMode::Axial : DeadZoneMode::Radial;
            host.SetProjectInputDirty(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Radial is the right choice for a stick: otherwise the "
                                      "diagonal leaves the zone earlier than a straight push."));

        ImGui::SetNextItemWidth(160);
        if (ImGui::DragFloat(T("Smoothing, sec"), &s.Smoothing, 0.005f, 0.0f, 1.0f, "%.3f"))
            host.SetProjectInputDirty(true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("0 is raw — the right choice for a first-person view: "
                                      "a smoothed look feels like the mouse is sinking."));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        if (ImGui::DragFloat(T("Sensitivity"), &s.Sensitivity, 0.01f, 0.01f, 10.0f, "%.2f"))
            host.SetProjectInputDirty(true);

        if (ImGui::Checkbox(T("Normalize"), &s.Normalize)) host.SetProjectInputDirty(true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Without it diagonal movement is 1.41 times faster than "
                                      "straight — the oldest bug in games."));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        if (ImGui::DragFloat(T("Press threshold"), &s.PressThreshold, 0.01f, 0.05f, 1.0f, "%.2f"))
            host.SetProjectInputDirty(true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("From which value an analog source counts as pressed — "
                                      "a trigger used as a fire button."));
    }

    ImGui::Spacing();
    ImGui::TextDisabled("%s", T("Controls"));
    ImGui::Separator();

    if (action.Bindings().empty()) {
        Hint(T("No controls yet — the action can never fire. Add one."));
    }
    int removeAt = -1;
    for (int i = 0; i < (int)action.Bindings().size(); ++i) {
        if (DrawBinding(host, action, i, contextName)) removeAt = i;
    }
    if (removeAt >= 0) {
        action.RemoveBindingAt(removeAt);
        host.SetProjectInputDirty(true);
    }

    if (ImGui::Button(T("+ Control"))) {
        m_captureBinding = -1;   // −1 — добавить новую, а не заменить
        m_captureOpen = true;
        m_captureContext = contextName;
        m_captureAction = action.Name();
    }
    // Оси назначаются списком, а не ловлей: курсор шевелится всегда, и первая
    // же дрожь руки заняла бы поле (см. BindingFromEvent).
    if (action.Type() != ActionType::Digital) {
        ImGui::SameLine();
        if (ImGui::Button(T("+ Axis..."))) ImGui::OpenPopup("##axisMenu");
        if (ImGui::BeginPopup("##axisMenu")) {
            struct AxisItem { const char* Label; Binding (*Make)(); };
            static const AxisItem kAxes[] = {
                {"MOUSE_X", [] { return Binding::MouseAxisX(); }},
                {"MOUSE_Y", [] { return Binding::MouseAxisY(); }},
                {"PAD_LEFT_X", [] { return Binding::OfPadAxis(sage::input::GamepadAxis::LeftX); }},
                {"PAD_LEFT_Y", [] { return Binding::OfPadAxis(sage::input::GamepadAxis::LeftY); }},
                {"PAD_RIGHT_X", [] { return Binding::OfPadAxis(sage::input::GamepadAxis::RightX); }},
                {"PAD_RIGHT_Y", [] { return Binding::OfPadAxis(sage::input::GamepadAxis::RightY); }},
                {"PAD_LEFT_TRIGGER", [] { return Binding::OfPadAxis(sage::input::GamepadAxis::LeftTrigger); }},
                {"PAD_RIGHT_TRIGGER", [] { return Binding::OfPadAxis(sage::input::GamepadAxis::RightTrigger); }},
            };
            for (const AxisItem& item : kAxes) {
                if (!ImGui::MenuItem(item.Label)) continue;
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
}

void InputPanel::Draw(EditorHost& host, bool& open) {
    if (!open) return;

    ImGui::SetNextWindowSize(ImVec2(760, 620), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(560, 360), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin(T("Controls" "###Controls"), &open)) {
        ImGui::End();
        return;
    }

    InputSystem& input = host.ProjectInput();

    Hint(T("Actions of the game and what fires them. The game code asks for the ACTION "
           "(\"Jump\"), never for the key — so reassigning a control changes nothing in the "
           "code. Saved into the project as input.sageinput and applied both in Play mode "
           "and in the built game."));
    ImGui::Separator();

    // --- Сохранение --------------------------------------------------------
    if (ImGui::Button(T("Save to project"))) {
        if (host.SaveProjectInput()) host.SetStatusMessage(T("Controls saved: input.sageinput"));
        else host.SetStatusMessage(T("Controls not saved — no project open?"));
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Reload"))) {
        if (host.ReloadProjectInput()) host.SetStatusMessage(T("Controls reloaded from disk"));
    }
    if (host.ProjectInputDirty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.25f, 1.0f), "%s", T("unsaved changes"));
    }

    ImGui::SameLine();
    if (ImGui::Button(T("Standard layout..."))) ImGui::OpenPopup("##presetMenu");
    if (ImGui::BeginPopup("##presetMenu")) {
        Hint(T("Adds the usual set of actions. Existing ones are left alone."));
        ImGui::Separator();
        if (ImGui::MenuItem(T("First-person / third-person game"))) {
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
                {"Jump", "SPACE", "PAD_A"},      {"Sprint", "LEFT_SHIFT", "PAD_LEFT_THUMB"},
                {"Crouch", "LEFT_CONTROL", "PAD_B"}, {"Interact", "E", "PAD_X"},
                {"Attack", "MOUSE_LEFT", "PAD_RB"},  {"Aim", "MOUSE_RIGHT", "PAD_LB"},
                {"Reload", "R", "PAD_Y"},        {"Pause", "ESCAPE", "PAD_START"},
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

    ImGui::Separator();

    // --- Контексты ---------------------------------------------------------
    std::vector<Context*> contexts = input.ContextsByPriority();
    if (m_selectedContext.empty() && !contexts.empty()) m_selectedContext = contexts.front()->Name();

    ImGui::BeginChild("##contexts", ImVec2(210, 0), true);
    ImGui::TextDisabled("%s", T("Contexts"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("One key means different things in the game, in the "
                                  "inventory and in a dialogue. A context with a higher "
                                  "priority that used a device takes it away from the ones "
                                  "below — a click on a menu button must not fire the weapon."));
    ImGui::Separator();
    for (Context* ctx : contexts) {
        const bool selected = (ctx->Name() == m_selectedContext);
        ImGui::PushID(ctx->Name().c_str());
        if (ImGui::Selectable(ctx->Name().c_str(), selected)) m_selectedContext = ctx->Name();
        ImGui::SameLine();
        ImGui::TextDisabled("%d", ctx->Priority());
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##newContext", T("new context"), m_newContextName,
                             sizeof(m_newContextName));
    ImGui::SetNextItemWidth(80);
    ImGui::DragInt(T("prio"), &m_newContextPriority, 1.0f, -1000, 1000);
    if (ImGui::Button(T("+ Context"), ImVec2(-1, 0)) && m_newContextName[0] != '\0') {
        input.CreateContext(m_newContextName, m_newContextPriority);
        m_selectedContext = m_newContextName;
        m_newContextName[0] = '\0';
        host.SetProjectInputDirty(true);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // --- Действия выбранного контекста -------------------------------------
    ImGui::BeginChild("##actions", ImVec2(0, 0), true);
    Context* ctx = input.FindContext(m_selectedContext);
    if (!ctx) {
        Hint(T("Pick a context on the left."));
        ImGui::EndChild();
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted(ctx->Name().c_str());
    ImGui::SameLine();
    int priority = ctx->Priority();
    ImGui::SetNextItemWidth(110);
    if (ImGui::DragInt(T("Priority"), &priority, 1.0f, -1000, 1000)) {
        ctx->SetPriority(priority);
        host.SetProjectInputDirty(true);
    }

    bool blocksMouse = (ctx->Blocks() & DeviceMouse) != 0;
    bool blocksKeys = (ctx->Blocks() & DeviceKeyboard) != 0;
    if (ImGui::Checkbox(T("Takes the mouse"), &blocksMouse)) {
        ctx->SetBlocks((uint8_t)((ctx->Blocks() & ~DeviceMouse) | (blocksMouse ? DeviceMouse : 0)));
        host.SetProjectInputDirty(true);
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(T("Takes the keyboard"), &blocksKeys)) {
        ctx->SetBlocks((uint8_t)((ctx->Blocks() & ~DeviceKeyboard) | (blocksKeys ? DeviceKeyboard : 0)));
        host.SetProjectInputDirty(true);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Taken from the contexts below only when this context "
                                  "actually used the device that frame."));

    if (ctx->Name() != sage::input::kDefaultContext) {
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Delete context"))) {
            input.RemoveContext(ctx->Name());
            m_selectedContext.clear();
            host.SetProjectInputDirty(true);
            ImGui::EndChild();
            ImGui::End();
            return;
        }
    }

    ImGui::Separator();

    std::string removeAction;
    for (const std::string& name : ctx->ActionNames()) {
        Action* action = ctx->Find(name);
        if (!action) continue;
        ImGui::PushID(name.c_str());

        // Сводка привязок в заголовке: главный вопрос к этому окну — «что на
        // чём висит», и ради ответа не должно приходиться раскрывать каждое
        // действие по очереди.
        std::string summary;
        for (const Binding& b : action->Bindings()) {
            if (!summary.empty()) summary += ", ";
            summary += b.ToString();
        }
        const std::string header =
            name + (summary.empty() ? std::string("  —  ") + T("no controls")
                                    : std::string("  —  ") + summary);
        const bool opened = ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::SameLine(ImGui::GetWindowWidth() - 70);
        if (ImGui::SmallButton(T("Delete##action"))) removeAction = name;
        if (opened) {
            DrawAction(host, *ctx, *action, ctx->Name());
            ImGui::TreePop();
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    if (!removeAction.empty()) {
        ctx->Remove(removeAction);
        host.SetProjectInputDirty(true);
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##newAction", T("new action"), m_newActionName,
                             sizeof(m_newActionName));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(230);
    const char* types[] = {ActionTypeLabel(ActionType::Digital), ActionTypeLabel(ActionType::Axis),
                           ActionTypeLabel(ActionType::Vector)};
    ImGui::Combo("##newActionType", &m_newActionType, types, 3);
    ImGui::SameLine();
    if (ImGui::Button(T("+ Action")) && m_newActionName[0] != '\0') {
        ctx->Add(m_newActionName, (ActionType)m_newActionType);
        m_newActionName[0] = '\0';
        host.SetProjectInputDirty(true);
    }

    ImGui::EndChild();

    // --- Ловля назначаемой клавиши ------------------------------------------
    Binding caught;
    if (DrawCaptureModal(host, caught)) {
        Context* target = input.FindContext(m_captureContext);
        Action* action = target ? target->Find(m_captureAction) : nullptr;
        if (action) {
            if (m_captureBinding >= 0 && m_captureBinding < (int)action->Bindings().size()) {
                // Переназначение сохраняет вклад и половину вектора: человек
                // менял КЛАВИШУ, а не роль привязки в оси.
                const Binding& old = action->Bindings()[(size_t)m_captureBinding];
                caught.Scale = old.Scale;
                caught.Axis = old.Axis;
                action->MutableBindings()[(size_t)m_captureBinding] = caught;
            } else {
                action->Bind(caught);
            }
            host.SetProjectInputDirty(true);
        }
    }

    ImGui::End();
}
