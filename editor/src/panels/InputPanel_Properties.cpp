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
#include "InputPanelDetail.h"

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



// Свойства выбранного действия: привязки, тонкая настройка, поведение.
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
