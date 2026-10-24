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



// Модальные окна панели «Управление»: захват клавиши, новое действие, новый контекст.
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
