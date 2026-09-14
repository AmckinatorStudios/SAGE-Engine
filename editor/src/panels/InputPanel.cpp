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

// Короткая подпись — для бейджа в списке действий, где полная не влезает.
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

// Подстрока без учёта регистра — тот же приём, что и в поиске других панелей:
// действий в раскладке легко набирается два-три десятка (движение, обзор,
// боевые команды, инвентарь), и пролистывать их глазами дольше, чем набрать
// три буквы.
bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };
    return lower(haystack).find(lower(needle)) != std::string::npos;
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

bool InputPanel::HasAnyConflict(InputSystem& input, const Action& action) const {
    for (const Binding& b : action.Bindings()) {
        if (!ConflictWith(input, b, &action).empty()) return true;
    }
    return false;
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
        Sage::UI::TextSecondary("%s", T("Hold Ctrl / Shift / Alt to record a combination. Escape cancels."));
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

        if (Sage::UI::Button(T("Cancel"), Sage::UI::ButtonStyle::Secondary, ImVec2(120, 0))) {
            m_captureOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return captured;
}

bool InputPanel::DrawBindingRow(EditorHost& host, Action& action, int index,
                                const std::string& contextName) {
    const std::vector<Binding>& bindings = action.Bindings();
    if (index < 0 || index >= (int)bindings.size()) return false;
    const Binding& binding = bindings[(size_t)index];

    ImGui::PushID(index);
    bool remove = false;
    ImGui::TableNextRow();

    // --- Источник: он же «переназначить» — то, куда человек и целится: видит
    // «SPACE» и хочет нажать по нему, чтобы поменять. ---
    ImGui::TableSetColumnIndex(0);
    const std::string label = binding.ToString();
    if (Sage::UI::Button(label.c_str(), Sage::UI::ButtonStyle::Secondary, ImVec2(-1, 0))) {
        m_captureBinding = index;
        m_captureOpen = true;
        m_captureContext = contextName;
        m_captureAction = action.Name();
    }
    Sage::UI::Tooltip(T("Click to reassign"));

    // --- Устройство ---
    ImGui::TableSetColumnIndex(1);
    ImGui::AlignTextToFramePadding();
    Sage::UI::TextSecondary("%s", DeviceLabel(binding));

    // --- Вклад и половина вектора — только там, где они имеют смысл. У
    // обычной кнопки этих полей нет вовсе — а значит, у кнопочного действия и
    // КОЛОНКИ «Настройка» вовсе нет (см. DrawBindingsSection): резервировать
    // под неё место всегда значило бы отбирать его у колонки «Конфликт»,
    // которая кнопочному действию как раз и важна. ---
    const bool hasTuning = action.Type() != ActionType::Digital;
    int col = 2;
    if (hasTuning) {
        ImGui::TableSetColumnIndex(col++);
        const float half = action.Type() == ActionType::Vector
                                ? (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f
                                : -1.0f;
        ImGui::SetNextItemWidth(half);
        float scale = binding.Scale;
        if (ImGui::DragFloat("##scale", &scale, 0.05f, -4.0f, 4.0f, "%.2f")) {
            action.MutableBindings()[(size_t)index].Scale = scale;
            host.SetProjectInputDirty(true);
        }
        Sage::UI::Tooltip(T("Contribution to the value: W gives +1, S gives -1"));
        if (action.Type() == ActionType::Vector) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            int axis = (binding.Axis == Component::Y) ? 1 : 0;
            const char* axes[] = {T("X"), T("Y")};
            if (ImGui::Combo("##axis", &axis, axes, 2)) {
                action.MutableBindings()[(size_t)index].Axis = axis ? Component::Y : Component::X;
                host.SetProjectInputDirty(true);
            }
            Sage::UI::Tooltip(T("Which half of the vector this source feeds"));
        }
    }

    // --- Конфликт: показывается ЗДЕСЬ, а не при сохранении — узнать, что
    // клавиша занята, надо в момент назначения, а не когда игра повела себя
    // странно. ---
    ImGui::TableSetColumnIndex(col++);
    const std::string conflict = ConflictWith(host.ProjectInput(), binding, &action);
    if (!conflict.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Warn));
        ImGui::TextUnformatted(T("also:"));
        ImGui::SameLine();
        const std::string shown = Sage::UI::Truncate(conflict.c_str(), ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(shown.c_str());
        ImGui::PopStyleColor();
        Sage::UI::Tooltip(T("The same control is bound to another action. That is legal when "
                            "the two never run at the same time — they live in different contexts."));
    }

    // --- Удалить ---
    ImGui::TableSetColumnIndex(col);
    if (EditorIcons::IconOnlyButton("trash", T("Remove this control"))) remove = true;

    ImGui::PopID();
    return remove;
}

void InputPanel::DrawBindingsSection(EditorHost& host, Action& action, const std::string& contextName) {
    if (action.Bindings().empty()) {
        Sage::UI::EmptyState(T("No controls yet"), T("The action can never fire. Add one below."));
        return;
    }
    // Кнопочное действие не тратит колонку на «Настройку» — ей нечего туда
    // класть, а конфликту (самому частому вопросу к этой таблице) остаётся
    // намного больше места.
    const bool hasTuning = action.Type() != ActionType::Digital;
    const int columns = hasTuning ? 5 : 4;
    if (ImGui::BeginTable("##bindings", columns,
                          ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(T("Control"), ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn(T("Device"), ImGuiTableColumnFlags_WidthFixed, 68.0f);
        if (hasTuning) ImGui::TableSetupColumn(T("Tuning"), ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn(T("Conflict"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##rm", ImGuiTableColumnFlags_WidthFixed, 28.0f);

        int removeAt = -1;
        for (int i = 0; i < (int)action.Bindings().size(); ++i) {
            if (DrawBindingRow(host, action, i, contextName)) removeAt = i;
        }
        ImGui::EndTable();
        if (removeAt >= 0) {
            action.RemoveBindingAt(removeAt);
            host.SetProjectInputDirty(true);
        }
    }

    ImGui::Spacing();
    if (EditorIcons::Button("plus", T("Control"), T("Press a key, mouse button or gamepad button"))) {
        m_captureBinding = -1;   // −1 — добавить новую, а не заменить
        m_captureOpen = true;
        m_captureContext = contextName;
        m_captureAction = action.Name();
    }
    // Оси назначаются списком, а не ловлей: курсор шевелится всегда, и первая
    // же дрожь руки заняла бы поле (см. BindingFromEvent).
    if (action.Type() != ActionType::Digital) {
        ImGui::SameLine();
        if (EditorIcons::Button("plus", T("Axis..."), T("A stick, trigger or the mouse as an axis")))
            ImGui::OpenPopup("##axisMenu");
        // Отступы темы для меню: всплывающее окно наследует стиль, действующий в
        // момент открытия (см. Sage::UI::MenuScope).
        if (Sage::UI::MenuScope axisMenu; ImGui::BeginPopup("##axisMenu")) {
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

void InputPanel::DrawBehaviorSection(EditorHost& host, Action& action) {
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

bool InputPanel::DrawActionDetail(EditorHost& host, Action& action, const std::string& contextName) {
    bool deleteRequested = false;

    // --- Шапка: имя, вид, удаление. Вид — здесь, а не в «Поведении»: от него
    // зависит, какие поля вообще покажутся ниже, и увидеть его первым важнее,
    // чем найти в списке настроек. ---
    ImGui::TextUnformatted(action.Name().c_str());
    // Кнопка-значок, а не «значок + подпись»: у той ширина зависит от шрифта и
    // языка перевода, и точный отступ под неё посчитать нельзя было бы, не
    // нарисовав её дважды. У кнопки-значка ширина ВСЕГДА равна высоте строки.
    const float delW = ImGui::GetFrameHeight();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x >= delW + 8.0f
                        ? ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - delW
                        : ImGui::GetCursorPosX());
    if (EditorIcons::IconOnlyButton("trash", T("Delete this action")))
        deleteRequested = true;

    Sage::UI::BeginProperties("kind");
    Sage::UI::PropertyLabel(T("Kind"));
    ImGui::SetNextItemWidth(-1);
    int type = (int)action.Type();
    const char* types[] = {ActionTypeLabel(ActionType::Digital), ActionTypeLabel(ActionType::Axis),
                           ActionTypeLabel(ActionType::Vector)};
    if (ImGui::Combo("##kind", &type, types, 3)) {
        action.SetType((ActionType)type);
        host.SetProjectInputDirty(true);
    }
    Sage::UI::EndProperties();

    Sage::UI::Separator();

    if (Sage::UI::Section(T("Controls"), true)) DrawBindingsSection(host, action, contextName);
    if (Sage::UI::Section(T("Behavior"), true)) DrawBehaviorSection(host, action);

    return deleteRequested;
}

void InputPanel::DrawContextsColumn(EditorHost& host, InputSystem& input) {
    std::vector<Context*> contexts = input.ContextsByPriority();
    if (m_selectedContext.empty() && !contexts.empty()) m_selectedContext = contexts.front()->Name();

    ImGui::BeginChild("##contexts", ImVec2(200, 0), true);
    Sage::UI::TextSecondary("%s", T("Contexts"));
    Sage::UI::Tooltip(T("One key means different things in the game, in the inventory and in a "
                        "dialogue. A context with a higher priority that used a device takes it "
                        "away from the ones below — a click on a menu button must not fire the "
                        "weapon."));
    ImGui::Separator();
    for (Context* ctx : contexts) {
        const bool selected = (ctx->Name() == m_selectedContext);
        ImGui::PushID(ctx->Name().c_str());
        if (ImGui::Selectable(ctx->Name().c_str(), selected)) {
            if (ctx->Name() != m_selectedContext) m_selectedAction.clear();
            m_selectedContext = ctx->Name();
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x >= 24.0f
                            ? ImGui::GetWindowContentRegionMax().x - 28.0f
                            : ImGui::GetCursorPosX());
        Sage::UI::Badge(std::to_string(ctx->Priority()).c_str(), EditorTheme::Role::TextDim);
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##newContext", T("new context"), m_newContextName,
                             sizeof(m_newContextName));
    // Приоритет — подписан ВИДИМО, а не только подсказкой при наведении: число
    // без подписи рядом с полем имени читается как чужая, необъяснённая деталь.
    ImGui::AlignTextToFramePadding();
    Sage::UI::TextSecondary("%s", T("Priority"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::DragInt("##prio", &m_newContextPriority, 1.0f, -1000, 1000);
    Sage::UI::Tooltip(T("Higher priority takes a device away from contexts below it."));
    if (EditorIcons::Button("plus", T("Context"), T("A new input context")) &&
        m_newContextName[0] != '\0') {
        input.CreateContext(m_newContextName, m_newContextPriority);
        m_selectedContext = m_newContextName;
        m_selectedAction.clear();
        m_newContextName[0] = '\0';
        host.SetProjectInputDirty(true);
    }
    ImGui::EndChild();
}

void InputPanel::DrawActionsColumn(EditorHost& host, Context& ctx) {
    // Первое действие контекста выбирается само: иначе, открыв панель, человек
    // упирается в пустую правую колонку и не знает, с чего вообще начинать
    // смотреть, — надо ДОГАДАТЬСЯ, что щёлкнуть по списку слева.
    if (m_selectedAction.empty() || !ctx.Find(m_selectedAction)) {
        const std::vector<std::string> names = ctx.ActionNames();
        if (!names.empty()) m_selectedAction = names.front();
    }

    ImGui::BeginChild("##actionsList", ImVec2(290, 0), true);

    ImGui::TextUnformatted(ctx.Name().c_str());
    Sage::UI::BeginProperties("ctxHeader");
    Sage::UI::PropertyLabel(T("Priority"),
                           T("Higher priority takes a device away from contexts below it."));
    ImGui::SetNextItemWidth(-1);
    int priority = ctx.Priority();
    if (ImGui::DragInt("##priority", &priority, 1.0f, -1000, 1000)) {
        ctx.SetPriority(priority);
        host.SetProjectInputDirty(true);
    }
    Sage::UI::PropertyLabel(T("Takes"),
                           T("Taken from the contexts below only when this context actually used "
                             "the device that frame."));
    bool blocksMouse = (ctx.Blocks() & DeviceMouse) != 0;
    bool blocksKeys = (ctx.Blocks() & DeviceKeyboard) != 0;
    if (ImGui::Checkbox(T("Mouse"), &blocksMouse)) {
        ctx.SetBlocks((uint8_t)((ctx.Blocks() & ~DeviceMouse) | (blocksMouse ? DeviceMouse : 0)));
        host.SetProjectInputDirty(true);
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(T("Keyboard"), &blocksKeys)) {
        ctx.SetBlocks((uint8_t)((ctx.Blocks() & ~DeviceKeyboard) | (blocksKeys ? DeviceKeyboard : 0)));
        host.SetProjectInputDirty(true);
    }
    Sage::UI::EndProperties();

    if (ctx.Name() != sage::input::kDefaultContext) {
        if (Sage::UI::Button(T("Delete context"), Sage::UI::ButtonStyle::Danger, ImVec2(-1, 0))) {
            host.ProjectInput().RemoveContext(ctx.Name());
            m_selectedContext.clear();
            m_selectedAction.clear();
            host.SetProjectInputDirty(true);
            ImGui::EndChild();
            return;
        }
    }

    ImGui::Separator();
    Sage::UI::SearchField("##actionFilter", m_actionFilter, sizeof(m_actionFilter), T("Search actions..."));

    // --- Список действий: ВЫБОР, а не раскрытие. Раньше каждое действие было
    // раскрывающимся узлом со всеми настройками внутри — раскрыл два и уже не
    // видно, какое поле чьё. Здесь строка отвечает только на вопрос «что это и
    // на чём висит», а настройки живут в соседней колонке, ровно у ОДНОГО
    // выбранного действия за раз. ---
    //
    // Высота — с ОТРИЦАТЕЛЬНЫМ запасом снизу: три строки «новое действие» идут
    // ПОСЛЕ этого дочернего окна в том же родителе, а окно нулевой высоты
    // забрало бы весь остаток места и вытолкнуло их за нижний край панели —
    // именно так это и не показывалось до правки.
    const float newActionRowH = ImGui::GetFrameHeightWithSpacing() * 3.4f;
    ImGui::BeginChild("##actionRows", ImVec2(0, -newActionRowH));
    const float lineH = ImGui::GetTextLineHeight();
    const float rowH = lineH * 2.0f + Sage::UI::Get().SpacingXS * 2.0f;
    const std::vector<std::string> allNames = ctx.ActionNames();
    const bool anyMatch = std::any_of(allNames.begin(), allNames.end(), [&](const std::string& n) {
        return ContainsCaseInsensitive(n, m_actionFilter);
    });
    if (!anyMatch) {
        Sage::UI::EmptyState(allNames.empty() ? T("No actions yet") : T("Nothing matches"),
                             allNames.empty() ? T("Add one below.") : T("Try another search."));
    }
    for (const std::string& name : allNames) {
        if (!ContainsCaseInsensitive(name, m_actionFilter)) continue;
        Action* action = ctx.Find(name);
        if (!action) continue;
        ImGui::PushID(name.c_str());

        std::string summary;
        for (const Binding& b : action->Bindings()) {
            if (!summary.empty()) summary += ", ";
            summary += b.ToString();
        }
        if (summary.empty()) summary = T("no controls");

        const ImVec2 rowPos = ImGui::GetCursorScreenPos();
        const float rowW = ImGui::GetContentRegionAvail().x;
        const bool selected = (name == m_selectedAction);
        if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None, ImVec2(rowW, rowH)))
            m_selectedAction = name;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float pad = Sage::UI::Get().SpacingSM;
        dl->AddText(ImVec2(rowPos.x + pad, rowPos.y + 1.0f),
                   ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
        dl->AddText(ImVec2(rowPos.x + pad, rowPos.y + lineH + 2.0f),
                   ImGui::GetColorU32(ImGuiCol_TextDisabled), summary.c_str());

        // Бейдж вида действия и предупреждение о конфликте — у правого края,
        // видно СРАЗУ, без раскрытия.
        const char* kindText = ActionTypeShort(action->Type());
        const ImVec2 kindSize = ImGui::CalcTextSize(kindText);
        float rightX = rowPos.x + rowW - pad - kindSize.x;
        dl->AddText(ImVec2(rightX, rowPos.y + 1.0f),
                   ImGui::GetColorU32(ImGuiCol_TextDisabled), kindText);
        if (HasAnyConflict(host.ProjectInput(), *action)) {
            const char* warn = T("conflict");
            const ImVec2 warnSize = ImGui::CalcTextSize(warn);
            dl->AddText(ImVec2(rowPos.x + rowW - pad - warnSize.x, rowPos.y + lineH + 2.0f),
                       ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Warn)), warn);
        }
        Sage::UI::Tooltip(name.c_str());

        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##newAction", T("new action"), m_newActionName, sizeof(m_newActionName));
    ImGui::SetNextItemWidth(-1);
    const char* types[] = {ActionTypeLabel(ActionType::Digital), ActionTypeLabel(ActionType::Axis),
                           ActionTypeLabel(ActionType::Vector)};
    ImGui::Combo("##newActionType", &m_newActionType, types, 3);
    if (EditorIcons::Button("plus", T("Action"), T("A new action in this context")) &&
        m_newActionName[0] != '\0') {
        ctx.Add(m_newActionName, (ActionType)m_newActionType);
        m_selectedAction = m_newActionName;
        m_newActionName[0] = '\0';
        host.SetProjectInputDirty(true);
    }

    ImGui::EndChild();
}

void InputPanel::Draw(EditorHost& host, bool& open) {
    if (!open) return;

    ImGui::SetNextWindowSize(ImVec2(1010, 640), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(720, 420), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin(T("Controls" "###Controls"), &open)) {
        ImGui::End();
        return;
    }

    InputSystem& input = host.ProjectInput();

    // --- Верхняя панель: сохранение и типовая раскладка. Пояснение — одной
    // строкой, а не абзацем: подробности живут в подсказках на местах, а не в
    // тексте, который читают один раз и потом прокручивают мимо. ---
    Sage::UI::TextSecondary("%s", T("Actions of the game and what fires them — the game code asks "
                                    "for the action, never the key."));

    if (Sage::UI::Button(T("Save to project"), Sage::UI::ButtonStyle::Primary)) {
        if (host.SaveProjectInput()) host.SetStatusMessage(T("Controls saved: input.sageinput"));
        else host.SetStatusMessage(T("Controls not saved — no project open?"));
    }
    ImGui::SameLine();
    if (Sage::UI::Button(T("Reload"), Sage::UI::ButtonStyle::Secondary)) {
        if (host.ReloadProjectInput()) host.SetStatusMessage(T("Controls reloaded from disk"));
    }
    ImGui::SameLine();
    if (Sage::UI::Button(T("Standard layout..."), Sage::UI::ButtonStyle::Secondary))
        ImGui::OpenPopup("##presetMenu");
    if (host.ProjectInputDirty()) {
        ImGui::SameLine();
        Sage::UI::Badge(T("unsaved changes"), EditorTheme::Role::Warn);
    }

    // Отступы темы для меню: всплывающее окно наследует стиль, действующий в
    // момент открытия (см. Sage::UI::MenuScope). Время жизни MenuScope
    // держится в границах if — иначе деструктор снялся бы после EndChild()
    // ниже, и ImGui решил бы, что стиль не сняли вовсе.
    if (Sage::UI::MenuScope presetMenu; ImGui::BeginPopup("##presetMenu")) {
        Sage::UI::TextSecondary("%s", T("Adds the usual set of actions. Existing ones are left alone."));
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

    // --- Три колонки: контексты -> действия выбранного контекста ->
    // подробности выбранного действия. ---
    DrawContextsColumn(host, input);
    ImGui::SameLine();

    Context* ctx = input.FindContext(m_selectedContext);
    if (!ctx) {
        ImGui::BeginChild("##noContext", ImVec2(0, 0), true);
        Sage::UI::EmptyState(T("No context selected"), T("Pick one on the left, or create a new one."));
        ImGui::EndChild();
        ImGui::End();
        return;
    }
    DrawActionsColumn(host, *ctx);
    ImGui::SameLine();

    ImGui::BeginChild("##actionDetail", ImVec2(0, 0), true);
    Action* action = m_selectedAction.empty() ? nullptr : ctx->Find(m_selectedAction);
    if (!action) {
        Sage::UI::EmptyState(T("No action selected"),
                             T("Pick one from the list, or create a new one on the left."));
    } else if (DrawActionDetail(host, *action, ctx->Name())) {
        ctx->Remove(m_selectedAction);
        m_selectedAction.clear();
        host.SetProjectInputDirty(true);
    }
    ImGui::EndChild();

    // --- Ловля назначаемой клавиши ------------------------------------------
    Binding caught;
    if (DrawCaptureModal(host, caught)) {
        Context* target = input.FindContext(m_captureContext);
        Action* target_action = target ? target->Find(m_captureAction) : nullptr;
        if (target_action) {
            if (m_captureBinding >= 0 && m_captureBinding < (int)target_action->Bindings().size()) {
                // Переназначение сохраняет вклад и половину вектора: человек
                // менял КЛАВИШУ, а не роль привязки в оси.
                const Binding& old = target_action->Bindings()[(size_t)m_captureBinding];
                caught.Scale = old.Scale;
                caught.Axis = old.Axis;
                target_action->MutableBindings()[(size_t)m_captureBinding] = caught;
            } else {
                target_action->Bind(caught);
            }
            host.SetProjectInputDirty(true);
        }
    }

    ImGui::End();
}
