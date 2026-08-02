#include "ConfirmDialog.h"

#include "EditorIcons.h"
#include "imgui.h"

bool ConfirmDialog::IsSuppressed(const char* kind) const {
    auto it = m_suppressed.find(kind);
    return it != m_suppressed.end() && it->second;
}

void ConfirmDialog::SetSuppressed(const char* kind, bool suppressed) {
    m_suppressed[kind] = suppressed;
}

void ConfirmDialog::Ask(const char* kind, std::string title, std::string message,
                        std::function<void()> onConfirm, const char* confirmLabel) {
    if (!onConfirm) return;
    if (IsSuppressed(kind)) {
        onConfirm();
        return;
    }
    m_kind = kind;
    m_title = std::move(title);
    m_message = std::move(message);
    m_confirmLabel = confirmLabel ? confirmLabel : "Продолжить";
    m_action = std::move(onConfirm);
    m_dontAskAgain = false;
    m_open = true;
    m_needsOpen = true;
}

void ConfirmDialog::Draw() {
    if (!m_open) return;
    if (m_needsOpen) {
        ImGui::OpenPopup("###confirm");
        m_needsOpen = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    const std::string id = m_title + "###confirm";
    if (ImGui::BeginPopupModal(id.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        EditorIcons::Inline("warn");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", m_message.c_str());
        ImGui::Spacing();
        ImGui::Checkbox("Больше не спрашивать об этом действии", &m_dontAskAgain);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Вернуть вопрос можно в Window -> Settings.");
        }
        ImGui::Separator();

        // Опасное действие — НЕ кнопка по умолчанию. Фокус стоит на «Отмене»,
        // и Enter отменяет: человек, у которого сработала мышечная память на
        // «Enter = согласен», не должен этим удалять свою работу.
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button("Отмена", ImVec2(140, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_open = false;
            m_action = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.22f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.14f, 0.14f, 1.0f));
        const bool go = ImGui::Button(m_confirmLabel.c_str(), ImVec2(140, 0));
        ImGui::PopStyleColor(3);
        if (go) {
            if (m_dontAskAgain) SetSuppressed(m_kind.c_str(), true);
            // Копируем действие ДО закрытия: оно может открыть другой диалог, и
            // обнулять поле после вызова значило бы затирать уже новое состояние.
            std::function<void()> action = m_action;
            m_action = nullptr;
            m_open = false;
            ImGui::CloseCurrentPopup();
            if (action) action();
        }
        ImGui::EndPopup();
    }
}
