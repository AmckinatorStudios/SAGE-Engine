#include "ConsolePanel.h"
#include "EditorTheme.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "imgui.h"

#include "EditorIcons.h"
#include "../Localization.h"

void ConsolePanel::Attach() {
    Log::SetSink([this](LogLevel level, const std::string& cat, const std::string& msg) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (level == LogLevel::Warn) ++m_warnCount;
        if (level >= LogLevel::Error) ++m_errorCount;

        // Сворачивание повторов. Один кадр может выдать одно и то же
        // предупреждение тысячу раз (шейдер не собрался — и так на каждый
        // объект), и без свёртки такая строка вытесняет из окна всё остальное,
        // включая причину.
        if (!m_entries.empty()) {
            Entry& last = m_entries.back();
            if (last.Level == level && last.Category == cat && last.Message == msg) {
                ++last.Repeats;
                return;
            }
        }
        if (m_entries.size() > 4000) m_entries.erase(m_entries.begin(), m_entries.begin() + 1000);
        m_entries.push_back({level, cat, msg, 1});
    });
}

void ConsolePanel::Detach() {
    Log::SetSink(nullptr);
}

bool ConsolePanel::Passes(const Entry& e) const {
    if (e.Level >= LogLevel::Error) { if (!m_showError) return false; }
    else if (e.Level == LogLevel::Warn) { if (!m_showWarn) return false; }
    else if (e.Level == LogLevel::Info) { if (!m_showInfo) return false; }
    else if (!m_showDebug) return false;

    if (m_filter[0]) {
        // Ищем и по категории, и по тексту: искать «Shader» и не найти
        // сообщение категории Shader было бы неожиданностью.
        if (e.Message.find(m_filter) == std::string::npos &&
            e.Category.find(m_filter) == std::string::npos) {
            return false;
        }
    }
    return true;
}

void ConsolePanel::Draw(bool* open) {
    ImGui::Begin(T("Console" "###Console"), open);
    if (ImGui::IsWindowFocused()) MarkSeen();

    if (EditorIcons::Button("trash", T("Clear"), T("Clear the console"))) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
    }
    ImGui::SameLine();
    if (EditorIcons::Button("copy", T("Copy"), T("Copy the visible lines to the clipboard"))) {
        std::string all;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const Entry& e : m_entries) {
                if (!Passes(e)) continue;
                all += "[" + e.Category + "] " + e.Message;
                if (e.Repeats > 1) all += " (x" + std::to_string(e.Repeats) + ")";
                all += "\n";
            }
        }
        ImGui::SetClipboardText(all.c_str());
    }

    // Счётчики совмещены с переключателями уровня намеренно: «вижу, что есть
    // три ошибки» и «хочу видеть только их» — одно и то же движение мышью.
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Checkbox(T("Debug"), &m_showDebug);
    ImGui::SameLine();
    ImGui::Checkbox(T("Info"), &m_showInfo);
    ImGui::SameLine();
    char warnLabel[48];
    std::snprintf(warnLabel, sizeof(warnLabel), T("Warn (%d)"), m_warnCount);
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Warn));
    ImGui::Checkbox(warnLabel, &m_showWarn);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    char errLabel[48];
    std::snprintf(errLabel, sizeof(errLabel), T("Error (%d)"), m_errorCount);
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Danger));
    ImGui::Checkbox(errLabel, &m_showError);
    ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Checkbox(T("Collapse"), &m_collapse);
    ImGui::SameLine();
    ImGui::Checkbox(T("Auto-scroll"), &m_autoScroll);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##filter", T("Search..."), m_filter, sizeof(m_filter));
    ImGui::Separator();

    ImGui::BeginChild("##console_scroll", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        int shown = 0;
        for (const Entry& e : m_entries) {
            if (!Passes(e)) continue;
            ++shown;
            ImVec4 color(0.85f, 0.85f, 0.85f, 1.0f);
            const char* icon = "info";
            if (e.Level == LogLevel::Warn) { color = EditorTheme::Color(EditorTheme::Role::Warn); icon = "warn"; }
            if (e.Level >= LogLevel::Error) { color = EditorTheme::Color(EditorTheme::Role::Danger); icon = "error"; }
            if (e.Level <= LogLevel::Debug) { color = EditorTheme::Color(EditorTheme::Role::TextDim); icon = "debug"; }

            EditorIcons::Inline(icon, glm::vec3(color.x, color.y, color.z));
            ImGui::SameLine();
            if (m_collapse && e.Repeats > 1) {
                ImGui::TextColored(color, T("[%s] %s  (x%d)"), e.Category.c_str(), e.Message.c_str(),
                                   e.Repeats);
            } else {
                ImGui::TextColored(color, "[%s] %s", e.Category.c_str(), e.Message.c_str());
            }
            // Правый клик по строке копирует её одну: в отчёт обычно нужна
            // ровно одна строка, а не весь лог.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                ImGui::SetClipboardText(("[" + e.Category + "] " + e.Message).c_str());
            }
        }
        if (shown == 0) {
            ImGui::TextDisabled("%s", m_filter[0] ? T("The filter matched nothing")
                                                  : T("No messages of the selected level"));
        }
    }
    if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}
