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



// Окна панели «Управление»: окно действия (создание и правка) и новый контекст.

void InputPanel::OpenNewAction(const std::string& context) {
    m_draft = Draft{};
    m_draft.Open = true;
    m_draft.IsNew = true;
    m_draft.Context = context.empty() ? std::string(sage::input::kDefaultContext) : context;
    m_draft.Type = (int)ActionType::Digital;
}

void InputPanel::OpenEditAction(InputSystem& input, const Ref& ref) {
    Context* ctx = input.FindContext(ref.Context);
    Action* action = ctx ? ctx->Find(ref.Name) : nullptr;
    if (!action) return;
    m_draft = Draft{};
    m_draft.Open = true;
    m_draft.IsNew = false;
    m_draft.Original = ref;
    std::snprintf(m_draft.Name, sizeof(m_draft.Name), "%s", ref.Name.c_str());
    m_draft.Context = ref.Context;
    m_draft.Type = (int)action->Type();
    m_draft.Bindings = action->Bindings();
    m_draft.Settings = action->Settings();
}

// Применить черновик. Тот же контекст и имя — правка на месте (порядок строк
// сохраняется); иначе создаётся действие в целевом контексте, а старое
// удаляется: переименования в движке нет, и это одна операция для человека.
bool InputPanel::ApplyDraft(EditorHost& host, InputSystem& input) {
    if (m_draft.Name[0] == '\0') return false;
    Context* target = input.FindContext(m_draft.Context);
    if (!target) return false;

    const bool sameSpot = !m_draft.IsNew && m_draft.Original.Context == m_draft.Context &&
                          m_draft.Original.Name == m_draft.Name;
    Action* action = nullptr;
    if (sameSpot) {
        action = target->Find(m_draft.Name);
        if (!action) return false;
    } else {
        if (target->Find(m_draft.Name)) return false;   // имя занято — не затираем молча
        action = &target->Add(m_draft.Name, (ActionType)m_draft.Type);
    }
    action->SetType((ActionType)m_draft.Type);
    action->MutableBindings() = m_draft.Bindings;
    action->Settings() = m_draft.Settings;

    if (!m_draft.IsNew && !sameSpot) {
        if (Context* old = input.FindContext(m_draft.Original.Context)) old->Remove(m_draft.Original.Name);
    }
    host.SetProjectInputDirty(true);
    return true;
}

// --- ОКНО ДЕЙСТВИЯ -------------------------------------------------------
//
// ОДНО окно на создание и правку: имя, контекст, вид значения, клавиши, и
// свёрнутое «Дополнительно». Ловля клавиши — внутри него же: вторая модалка
// поверх первой заставляла человека держать в голове два окна ради одного
// назначения.
void InputPanel::DrawActionModal(EditorHost& host, InputSystem& input) {
    if (!m_draft.Open) return;

    const char* title = m_draft.IsNew ? T("New action###ActionEdit") : T("Edit action###ActionEdit");
    ImGui::OpenPopup(title);
    ImGui::SetNextWindowSize(ImVec2(520.0f * Sage::UI::Scale(), 0), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal(title, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;

    // Ловим только если ловля была включена ДО этого кадра: щелчок, который её
    // включил, сам лежит в событиях кадра и иначе назначил бы «левую кнопку мыши».
    const bool wasCapturing = m_draft.Capture != Draft::kNoCapture;
    if (wasCapturing) {
        for (const InputEvent& e : host.FrameInputEvents()) {
            // Escape отменяет ловлю, а не назначается: иначе первым же действием
            // человек назначил бы Escape на то, что пытался отменить.
            if (e.Type == InputEventType::KeyPressed && e.Keyboard == sage::input::Key::Escape) {
                m_draft.Capture = Draft::kNoCapture;
                break;
            }
            Binding caught;
            if (!BindingFromEvent(e, caught)) continue;
            if (m_draft.Capture >= 0 && m_draft.Capture < (int)m_draft.Bindings.size()) {
                // Переназначение сохраняет вклад и половину вектора: человек
                // менял КЛАВИШУ, а не роль привязки в оси.
                const Binding& old = m_draft.Bindings[(size_t)m_draft.Capture];
                caught.Scale = old.Scale;
                caught.Axis = old.Axis;
                m_draft.Bindings[(size_t)m_draft.Capture] = caught;
            } else {
                m_draft.Bindings.push_back(caught);
            }
            m_draft.Capture = Draft::kNoCapture;
            break;
        }
    }

    // Фокус в поле имени сразу у нового действия: окно открыли, чтобы набрать имя.
    Sage::UI::BeginProperties("actionedit");
    Sage::UI::PropertyLabel(T("Name"));
    if (m_draft.IsNew && ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##name", T("Action name, e.g. Jump"), m_draft.Name, sizeof(m_draft.Name));

    Sage::UI::PropertyLabel(T("Context"),
                            T("A group of actions that work at the same time: the game, the "
                              "inventory, a dialogue."));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##ctx", m_draft.Context.c_str())) {
        for (Context* c : input.ContextsByPriority()) {
            if (ImGui::Selectable(c->Name().c_str(), c->Name() == m_draft.Context))
                m_draft.Context = c->Name();
        }
        ImGui::EndCombo();
    }

    Sage::UI::PropertyLabel(T("Value Type"), ActionTypeHint((ActionType)m_draft.Type));
    ImGui::SetNextItemWidth(-1);
    const char* types[] = {ActionTypeLabel(ActionType::Digital), ActionTypeLabel(ActionType::Axis),
                           ActionTypeLabel(ActionType::Vector)};
    char typeCombo[96];
    std::snprintf(typeCombo, sizeof(typeCombo), "%s — %s", types[m_draft.Type],
                  ActionTypeHint((ActionType)m_draft.Type));
    if (ImGui::BeginCombo("##kind", typeCombo)) {
        for (int i = 0; i < 3; ++i) {
            char item[96];
            std::snprintf(item, sizeof(item), "%s — %s", types[i], ActionTypeHint((ActionType)i));
            if (ImGui::Selectable(item, i == m_draft.Type)) {
                m_draft.Type = i;
                // Привязки осей к булеву действию бессмысленны, а вклад/половина
                // вектора сбрасываются тем же SetType в движке — здесь только
                // ловля, которой не на что указывать.
                m_draft.Capture = Draft::kNoCapture;
            }
        }
        ImGui::EndCombo();
    }
    Sage::UI::EndProperties();

    Sage::UI::Separator();
    Sage::UI::TextSecondary("%s", T("Controls"));
    ImGui::Spacing();
    DrawDraftControls(host, input);
    if (m_draft.Capture != Draft::kNoCapture) {
        ImGui::PushTextWrapPos(0.0f);
        Sage::UI::TextSecondary("%s", T("Press a key, a mouse button, the wheel or a gamepad button. "
                                        "Escape cancels."));
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    if (Sage::UI::Section(T("Advanced"), false)) DrawAdvanced(host);

    // Имя занято в целевом контексте — говорим об этом рядом с кнопкой, а не
    // молча отключаем её.
    Context* target = input.FindContext(m_draft.Context);
    const bool sameSpot = !m_draft.IsNew && m_draft.Original.Context == m_draft.Context &&
                          m_draft.Original.Name == m_draft.Name;
    const bool taken = target && !sameSpot && m_draft.Name[0] && target->Find(m_draft.Name);
    const bool valid = m_draft.Name[0] != '\0' && target && !taken;
    if (taken) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Warn));
        ImGui::TextUnformatted(T("An action with this name already exists in this context"));
        ImGui::PopStyleColor();
    }

    Sage::UI::Separator();
    const float bw = (ImGui::GetContentRegionAvail().x - Sage::UI::Get().SpacingSM) * 0.5f;
    if (Sage::UI::Button(T("Cancel"), Sage::UI::ButtonStyle::Secondary, ImVec2(bw, 0))) {
        m_draft.Open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0.0f, Sage::UI::Get().SpacingSM);
    ImGui::BeginDisabled(!valid);
    if (Sage::UI::Button(m_draft.IsNew ? T("Create") : T("Done"), Sage::UI::ButtonStyle::Primary,
                         ImVec2(bw, 0)) && ApplyDraft(host, input)) {
        m_draft.Open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
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
        m_newContextName[0] = '\0';
        m_newContextOpen = false;
        host.SetProjectInputDirty(true);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
