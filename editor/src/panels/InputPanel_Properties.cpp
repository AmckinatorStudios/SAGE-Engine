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

// Редактируемое в окне действия: клавиши и «Дополнительно».

// --- КЛАВИШИ ДЕЙСТВИЯ ----------------------------------------------------
//
// Каждая привязка — отдельный элемент: нажатие запускает переназначение прямо
// в этом же окне (без второй модалки), рядом — удаление. Спорная привязка
// красится и подписывается: узнать, что клавиша занята, надо в момент
// назначения, а не когда игра повела себя странно.
void InputPanel::DrawDraftControls(EditorHost& host, InputSystem& input) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    const Action* self = nullptr;
    if (!m_draft.IsNew) {
        if (Context* oc = input.FindContext(m_draft.Original.Context))
            self = oc->Find(m_draft.Original.Name);
    }
    const float h = ImGui::GetFrameHeight();
    const float avail = ImGui::GetContentRegionAvail().x;
    float lineX = 0.0f;
    int removeAt = -1;
    std::vector<std::string> warnings;

    // Элемент переносится на новую строку, если не помещается: окно не
    // растягивается и не прячет привязку за краем.
    auto place = [&](float w) {
        if (lineX > 0.0f && lineX + ui.SpacingXS + w > avail) lineX = 0.0f;
        else if (lineX > 0.0f) ImGui::SameLine(0.0f, ui.SpacingXS);
        lineX = (lineX > 0.0f ? lineX + ui.SpacingXS : 0.0f) + w;
    };
    const float padX = ImGui::GetStyle().FramePadding.x * 2.0f;
    const float iconS = std::floor(h * 0.68f);

    for (int i = 0; i < (int)m_draft.Bindings.size(); ++i) {
        const Binding& binding = m_draft.Bindings[(size_t)i];
        const bool capturing = m_draft.Capture == i;
        const std::string label = capturing ? std::string(T("Press a control...")) : binding.ToString();
        const std::string conflict = ConflictWith(input, binding, self);
        place(EditorIcons::LabeledWidth(iconS, label.c_str()) + padX + h);
        ImGui::PushID(i);
        if (!conflict.empty())
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Warn));
        if (EditorIcons::Button(DeviceIcon(binding), label.c_str(), T("Click to reassign"), capturing))
            m_draft.Capture = capturing ? Draft::kNoCapture : i;
        if (!conflict.empty()) {
            ImGui::PopStyleColor();
            warnings.push_back(binding.ToString() + " — " + T("also used by:") + " " + conflict);
        }
        ImGui::SameLine(0.0f, 0.0f);
        if (EditorIcons::IconOnlyButton("trash", T("Remove this control"))) removeAt = i;
        ImGui::PopID();
    }

    if (m_draft.Capture == -1) {
        const char* label = T("Press a control...");
        place(EditorIcons::LabeledWidth(iconS, label) + padX);
        EditorIcons::Button("keyboard", label, T("Escape cancels"), true);
    }

    if (m_draft.Bindings.empty() && m_draft.Capture != -1) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Warn));
        ImGui::TextUnformatted(T("No key assigned — the action can never fire"));
        ImGui::PopStyleColor();
    }

    // + Add Control. У булева действия — сразу ловля. У осевого ещё и список
    // осей: курсор шевелится всегда, и первая же дрожь руки заняла бы поле
    // (см. BindingFromEvent).
    ImGui::Spacing();
    const bool bool_ = (ActionType)m_draft.Type == ActionType::Digital;
    if (EditorIcons::Button("plus", T("Add Control"), T("Press a key, mouse button or gamepad button"))) {
        if (bool_) m_draft.Capture = -1;
        else ImGui::OpenPopup("##addControl");
    }
    if (Sage::UI::MenuScope addMenu; ImGui::BeginPopup("##addControl")) {
        if (EditorIcons::MenuItem("keyboard", T("Press a key or button...")))
            m_draft.Capture = -1;
        ImGui::Separator();
        for (const AxisItem& item : kAxes) {
            const char* icon = std::strncmp(item.Label, "MOUSE", 5) == 0 ? "select" : "pilot";
            if (!EditorIcons::MenuItem(icon, item.Label)) continue;
            Binding b = item.Make();
            // Вторая ось векторного действия почти всегда Y — угадываем по имени.
            if ((ActionType)m_draft.Type == ActionType::Vector && std::strstr(item.Label, "_Y"))
                b.Axis = Component::Y;
            m_draft.Bindings.push_back(b);
        }
        ImGui::EndPopup();
    }

    for (const std::string& w : warnings) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Warn));
        ImGui::TextWrapped("%s", w.c_str());
        ImGui::PopStyleColor();
    }
    if (!warnings.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        Sage::UI::TextSecondary("%s", T("That is legal when the two never run at the same time — "
                                        "they live in different contexts."));
        ImGui::PopTextWrapPos();
    }

    if (removeAt >= 0) {
        m_draft.Bindings.erase(m_draft.Bindings.begin() + removeAt);
        m_draft.Capture = Draft::kNoCapture;
    }
    (void)host;
}

// --- ДОПОЛНИТЕЛЬНО -------------------------------------------------------
//
// Свёрнуто по умолчанию (Section(..., false) в окне действия): девять человек
// из десяти приходят сюда поменять клавишу, а не мёртвую зону. Показывается
// только то, что относится к виду значения: у булева действия нет мёртвой зоны,
// у осевого — режима срабатывания. Правится ЧЕРНОВИК (m_draft).
void InputPanel::DrawAdvanced(EditorHost&) {
    sage::input::ActionSettings& s = m_draft.Settings;
    const ActionType type = (ActionType)m_draft.Type;
    Sage::UI::BeginProperties("behavior");

    if (type == ActionType::Digital) {
        Sage::UI::PropertyLabel(T("Fires"));
        ImGui::SetNextItemWidth(-1);
        int trigger = (int)s.Trigger;
        const char* triggers[] = {TriggerLabel(TriggerMode::Press), TriggerLabel(TriggerMode::Release),
                                  TriggerLabel(TriggerMode::Hold), TriggerLabel(TriggerMode::Tap)};
        if (ImGui::Combo("##trigger", &trigger, triggers, 4)) s.Trigger = (TriggerMode)trigger;

        if (s.Trigger == TriggerMode::Hold) {
            Sage::UI::PropertyLabel(T("Hold, sec"));
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##hold", &s.HoldTime, 0.01f, 0.05f, 3.0f, "%.2f");
        }
        if (s.Trigger == TriggerMode::Tap) {
            Sage::UI::PropertyLabel(T("Tap shorter than, sec"));
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##tap", &s.TapTime, 0.01f, 0.05f, 2.0f, "%.2f");
        }

        Sage::UI::PropertyLabel(T("Exact modifiers"),
                               T("Off: W keeps working while Shift is held — otherwise sprinting "
                                 "would break walking. On: for shortcuts, so Ctrl+S never fires "
                                 "from Ctrl+Shift+S."));
        ImGui::Checkbox("##exact", &s.ExactModifiers);
    } else {
        Sage::UI::PropertyLabel(T("Dead zone"),
                               T("Stick wobble around the centre is ignored. Does not touch the "
                                 "keyboard: a key is either pressed or not."));
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##deadzone", &s.DeadZone, 0.01f, 0.0f, 0.9f, "%.2f");
        Sage::UI::PropertyLabel(T("Dead zone shape"),
                               T("Radial is the right choice for a stick: otherwise the diagonal "
                                 "leaves the zone earlier than a straight push."));
        ImGui::SetNextItemWidth(-1);
        int zone = (s.Zone == DeadZoneMode::Axial) ? 1 : 0;
        const char* zones[] = {T("Radial"), T("Per axis")};
        if (ImGui::Combo("##zone", &zone, zones, 2))
            s.Zone = zone ? DeadZoneMode::Axial : DeadZoneMode::Radial;

        Sage::UI::PropertyLabel(T("Smoothing, sec"),
                               T("0 is raw — the right choice for a first-person view: a smoothed "
                                 "look feels like the mouse is sinking."));
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##smoothing", &s.Smoothing, 0.005f, 0.0f, 1.0f, "%.3f");
        Sage::UI::PropertyLabel(T("Sensitivity"));
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##sensitivity", &s.Sensitivity, 0.01f, 0.01f, 10.0f, "%.2f");
        Sage::UI::PropertyLabel(T("Normalize diagonal"),
                               T("Without it diagonal movement is 1.41 times faster than straight "
                                 "— the oldest bug in games."));
        ImGui::Checkbox("##normalize", &s.Normalize);

        Sage::UI::PropertyLabel(T("Press threshold"),
                               T("From which value an analog source counts as pressed — a trigger "
                                 "used as a fire button."));
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat("##pressthreshold", &s.PressThreshold, 0.01f, 0.05f, 1.0f, "%.2f");
    }
    Sage::UI::EndProperties();

    // ВКЛАД И ПОЛОВИНА ВЕКТОРА — только у осевых действий и только здесь: у
    // булева действия этих полей нет вовсе («вклад +1» у кнопки прыжка нечем
    // объяснить), а у осевого их трогают один раз при настройке.
    if (type != ActionType::Digital && !m_draft.Bindings.empty()) {
        Sage::UI::MenuSection(T("Controls"), true);
        Sage::UI::BeginProperties("tuning");
        for (size_t i = 0; i < m_draft.Bindings.size(); ++i) {
            Binding& b = m_draft.Bindings[i];
            ImGui::PushID((int)i);
            const std::string name = b.ToString();
            Sage::UI::PropertyLabel(name.c_str(),
                                    T("Contribution to the value: W gives +1, S gives -1"));
            ImGui::SetNextItemWidth(type == ActionType::Vector ? -90.0f : -1.0f);
            ImGui::DragFloat("##scale", &b.Scale, 0.05f, -4.0f, 4.0f, "%.2f");
            if (type == ActionType::Vector) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                int axis = (b.Axis == Component::Y) ? 1 : 0;
                const char* axes[] = {T("X"), T("Y")};
                if (ImGui::Combo("##axis", &axis, axes, 2)) b.Axis = axis ? Component::Y : Component::X;
            }
            ImGui::PopID();
        }
        Sage::UI::EndProperties();
    }
}
