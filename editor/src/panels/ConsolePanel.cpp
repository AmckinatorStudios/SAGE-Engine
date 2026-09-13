#include "../PanelWindows.h"
#include "ConsolePanel.h"

#include <ctime>
#include "ui/UI.h"
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
        // Время берётся ЗДЕСЬ, а не при отрисовке: строка могла прийти час
        // назад, и «время показа» ответило бы не на тот вопрос.
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        char stamp[16];
        std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        m_entries.push_back({level, cat, msg, 1, stamp});
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
    ImGui::Begin(T("Console" "###Console"), open, panelwindows::WindowFlags("Console"));
    if (ImGui::IsWindowFocused()) MarkSeen();

    // --- ФИЛЬТРЫ ОДНОЙ ЛЕНТОЙ ----------------------------------------------
    //
    // Было пять галок подряд («Debug», «Info», «Warn (2)», «Error (1)»,
    // «Collapse», «Auto-scroll») вперемешку с кнопками: строка из шести
    // квадратиков, в которой каждый читают по подписи. Уровень лога — это ОДИН
    // выбор из пяти, и выглядеть он должен как выбор: лента кнопок, где нажата
    // та, что смотрят. «Все» возвращает всё разом — именно этого хотят, закрыв
    // разбор конкретной ошибки.
    const Sage::UI::Style& ui = Sage::UI::Get();
    struct Chip { const char* Icon; const char* Label; bool* Flag; int Count; EditorTheme::Role Tint; };
    const bool all = m_showDebug && m_showInfo && m_showWarn && m_showError;
    if (EditorIcons::Button("list", T("All"), T("Show messages of every level"), all)) {
        m_showDebug = m_showInfo = m_showWarn = m_showError = true;
    }
    // Каждая следующая кнопка оставляет ТОЛЬКО свой уровень: разбирают лог
    // всегда так — «покажи мне ошибки». Чтобы добавить уровень к текущему
    // набору, по кнопке щёлкают с Ctrl.
    auto level = [&](const char* icon, const char* label, bool& flag, int count,
                     EditorTheme::Role tint) {
        char text[64];
        if (count > 0) std::snprintf(text, sizeof(text), "%s (%d)", label, count);
        else std::snprintf(text, sizeof(text), "%s", label);
        const bool only = flag && !all;
        ImGui::SameLine(0.0f, ui.SpacingXS);
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(tint));
        const bool pressed = EditorIcons::Button(icon, text, nullptr, only);
        ImGui::PopStyleColor();
        if (!pressed) return;
        if (ImGui::GetIO().KeyCtrl) {
            flag = !flag;   // добавить/убрать уровень к текущему набору
        } else {
            m_showDebug = m_showInfo = m_showWarn = m_showError = false;
            flag = true;
        }
    };
    level("info", T("Log"), m_showInfo, 0, EditorTheme::Role::Text);
    level("warn", T("Warning"), m_showWarn, m_warnCount, EditorTheme::Role::Warn);
    level("error", T("Error"), m_showError, m_errorCount, EditorTheme::Role::Danger);
    level("debug", T("Debug"), m_showDebug, 0, EditorTheme::Role::TextDim);

    // Поиск и служебные кнопки — справа, у края: слева живёт то, чем
    // пользуются постоянно, справа то, к чему обращаются точечно.
    const float rightW = ui.ControlHeight * 4.0f + ui.ControlHeight * 9.0f;
    const float rightX = std::max(ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ui.SpacingMD,
                                  ImGui::GetWindowWidth() - rightW - ui.PaddingPanel);
    ImGui::SameLine(rightX);
    Sage::UI::SearchField("filter", m_filter, sizeof(m_filter), T("Search in console..."),
                          ui.ControlHeight * 9.0f);
    ImGui::SameLine(0.0f, ui.SpacingXS);
    if (EditorIcons::IconOnlyButton("list", T("Collapse repeats"), m_collapse))
        m_collapse = !m_collapse;
    ImGui::SameLine(0.0f, ui.SpacingXS);
    if (EditorIcons::IconOnlyButton("drop", T("Scroll to the newest line"), m_autoScroll))
        m_autoScroll = !m_autoScroll;
    ImGui::SameLine(0.0f, ui.SpacingXS);
    if (EditorIcons::IconOnlyButton("copy", T("Copy the visible lines to the clipboard"))) {
        std::string all_text;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const Entry& e : m_entries) {
                if (!Passes(e)) continue;
                all_text += e.Stamp + " [" + e.Category + "] " + e.Message;
                if (e.Repeats > 1) all_text += " (x" + std::to_string(e.Repeats) + ")";
                all_text += "\n";
            }
        }
        ImGui::SetClipboardText(all_text.c_str());
    }
    ImGui::SameLine(0.0f, ui.SpacingXS);
    if (EditorIcons::IconOnlyButton("trash", T("Clear the console"))) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
    }
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

            // ТРИ КОЛОНКИ: когда — что — откуда. Раньше всё шло одной строкой
            // «[Категория] сообщение», и категория стояла ПЕРЕД сообщением —
            // то есть глаз, ищущий текст ошибки, каждый раз перепрыгивал через
            // служебное слово. Теперь время слева, текст по центру и на своём
            // месте у всех строк, а источник прижат вправо и не мешает.
            ImGui::TextDisabled("%s", e.Stamp.c_str());
            ImGui::SameLine(0.0f, 10.0f);
            EditorIcons::Inline(icon, glm::vec3(color.x, color.y, color.z));
            ImGui::SameLine(0.0f, 0.0f);
            if (m_collapse && e.Repeats > 1) {
                ImGui::TextColored(color, T("%s  (x%d)"), e.Message.c_str(), e.Repeats);
            } else {
                ImGui::TextColored(color, "%s", e.Message.c_str());
            }
            // Правый клик по строке копирует её одну: в отчёт обычно нужна
            // ровно одна строка, а не весь лог.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                ImGui::SetClipboardText((e.Stamp + " [" + e.Category + "] " + e.Message).c_str());
            }
            // Откуда пришло — у правого края и приглушённо: это ответ на второй
            // вопрос («где искать»), и он не должен спорить с первым («что»).
            const float catW = ImGui::CalcTextSize(e.Category.c_str()).x;
            const float catX = ImGui::GetWindowContentRegionMax().x - catW;
            if (catX > ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 12.0f) {
                ImGui::SameLine(catX);
                ImGui::TextDisabled("%s", e.Category.c_str());
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
