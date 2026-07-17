#include "ConsolePanel.h"

#include "imgui.h"

void ConsolePanel::Attach() {
    Log::SetSink([this](LogLevel level, const std::string& cat, const std::string& msg) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_entries.size() > 2000) m_entries.erase(m_entries.begin(), m_entries.begin() + 500);
        m_entries.push_back({level, cat, msg});
    });
}

void ConsolePanel::Detach() {
    Log::SetSink(nullptr);
}

void ConsolePanel::Draw() {
    ImGui::Begin("Console");
    if (ImGui::SmallButton("Clear")) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_autoScroll);
    ImGui::Separator();

    ImGui::BeginChild("##console_scroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const Entry& e : m_entries) {
            ImVec4 color = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
            if (e.Level == LogLevel::Warn) color = ImVec4(0.95f, 0.80f, 0.30f, 1.0f);
            if (e.Level == LogLevel::Error) color = ImVec4(0.95f, 0.40f, 0.40f, 1.0f);
            if (e.Level <= LogLevel::Debug) color = ImVec4(0.55f, 0.60f, 0.65f, 1.0f);
            ImGui::TextColored(color, "[%s] %s", e.Category.c_str(), e.Message.c_str());
        }
    }
    if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}
