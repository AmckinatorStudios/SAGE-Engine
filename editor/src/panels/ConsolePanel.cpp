#include "../PanelWindowId.h"
#include "../PanelWindows.h"
#include "ConsolePanel.h"

#include <ctime>
#include "ui/UI.h"
#include "EditorTheme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"

#include "EditorIcons.h"
#include "../Localization.h"

namespace {

// Цвет и значок уровня — одни и те же у кнопки фильтра и у строки журнала:
// жёлтый треугольник наверху обязан значить то же, что жёлтый треугольник в
// списке.
struct LevelLook {
    const char* Icon;
    ImVec4 Color;
};

LevelLook LookOf(LogLevel level) {
    if (level >= LogLevel::Error) return {"error", EditorTheme::Color(EditorTheme::Role::Danger)};
    if (level == LogLevel::Warn) return {"warn", EditorTheme::Color(EditorTheme::Role::Warn)};
    if (level == LogLevel::Info) return {"info", EditorTheme::Color(EditorTheme::Role::Text)};
    return {"debug", EditorTheme::Color(EditorTheme::Role::TextDim)};
}

// КНОПКА УРОВНЯ: значок цвета уровня и число строк. Без подписи-слова: три
// кнопки «Сообщения (120)», «Предупреждения (3)», «Ошибки (1)» не помещались
// в узкую панель, и поиск с остальными кнопками уезжал за край. Значок и цвет
// уровень узнают и так, а слово — в подсказке.
bool LevelChip(const char* id, LogLevel level, int count, bool on, const char* tooltip) {
    const LevelLook look = LookOf(level);
    const float h = ImGui::GetFrameHeight();
    const float icon = std::floor(h * 0.62f);
    char num[16];
    std::snprintf(num, sizeof(num), "%d", count > 9999 ? 9999 : count);
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    const float gap = EditorIcons::TextGap();
    const float textW = ImGui::CalcTextSize(num).x;
    const ImVec2 size(std::floor(pad.x * 2.0f + icon + gap + textW), h);

    ImGui::PushID(id);
    // Выключенный уровень — без подложки и бледный: видно, что его строки
    // сейчас спрятаны. Включённый — с подложкой, как нажатая кнопка.
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ImGui::GetStyleColorVec4(ImGuiCol_FrameBg)
                                              : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::Button("##chip", size);
    ImGui::PopStyleColor();

    const float alpha = on ? 1.0f : 0.38f;
    // Нет строк этого уровня — значок приглушён и у включённой кнопки: серый
    // ноль не должен кричать красным, что «есть ошибки».
    const float tintAlpha = count > 0 ? alpha : alpha * 0.55f;
    EditorIcons::Overlay(ImGui::GetWindowDrawList(), at.x + pad.x, std::floor(at.y + (h - icon) * 0.5f),
                         icon, look.Icon,
                         glm::vec3(look.Color.x, look.Color.y, look.Color.z) * tintAlpha +
                             glm::vec3(0.5f) * (1.0f - tintAlpha) * 0.4f);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(at.x + pad.x + icon + gap, at.y + pad.y),
        ImGui::GetColorU32(ImGuiCol_Text, count > 0 ? alpha : alpha * 0.6f), num);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return pressed;
}

std::string NowStamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char stamp[16];
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return stamp;
}

} // namespace

void ConsolePanel::Attach() {
    Log::SetSink([this](LogLevel level, const std::string& cat, const std::string& msg) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (level == LogLevel::Warn) ++m_warnTotal;
        if (level >= LogLevel::Error) ++m_errorTotal;
        // Время берётся ЗДЕСЬ, а не при отрисовке: строка могла прийти час
        // назад, и «время показа» ответило бы не на тот вопрос.
        m_log.Add(level, cat, msg, NowStamp());
    });
}

void ConsolePanel::Detach() {
    Log::SetSink(nullptr);
}

int ConsolePanel::WarningsShown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_log.Count(LogLevel::Warn);
}

int ConsolePanel::ErrorsShown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_log.Count(LogLevel::Error);
}

void ConsolePanel::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_log.Clear();
    m_selected = -1;
    MarkSeen();
}

void ConsolePanel::Draw(bool* open, const std::string& windowId) {
    ImGui::Begin(sage::editor::panelid::Title("console", T("Console"), windowId).c_str(), open,
                 panelwindows::WindowFlags("Console"));
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) MarkSeen();
    DrawToolbar();
    ImGui::Separator();
    DrawRows();
    ImGui::End();
}

// --- ПАНЕЛЬ НАД СПИСКОМ: ОДНА СТРОКА, КОТОРАЯ ВСЕГДА ВЛЕЗАЕТ -----------------
//
// Было: «Все», четыре кнопки уровней со словами, поле поиска фиксированной
// ширины и ещё четыре значка. Ширина ряда была посчитана «на глаз» и в узкой
// панели кнопки уезжали за край — до корзины было не дотянуться. Значки
// подобраны случайно: «список» означал и «Все», и «сворачивать повторы», а
// прокрутку к новому показывала «капля».
//
// Стало: слева три уровня (значок + число), посередине поиск — он РАСТЯГИВАЕТСЯ
// на всё, что осталось, справа — очистить и «ещё» (⋯). Редкое — сворачивание
// повторов, автопрокрутка, время, отладочные сообщения, копирование всего —
// живёт в меню «ещё»: оно нужно раз в неделю и не должно занимать место у
// того, что нажимают каждую минуту.
void ConsolePanel::DrawToolbar() {
    const Sage::UI::Style& ui = Sage::UI::Get();
    int counts[3];
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        counts[0] = m_log.Count(LogLevel::Info) + (m_filter.Debug ? m_log.Count(LogLevel::Debug) : 0);
        counts[1] = m_log.Count(LogLevel::Warn);
        counts[2] = m_log.Count(LogLevel::Error);
    }

    // Щелчок включает или прячет уровень. Ctrl+щелчок — показать ТОЛЬКО его:
    // так разбирают лог («покажи мне ошибки»), не выключая по одной остальные.
    auto chip = [&](const char* id, LogLevel level, int count, bool& flag, const char* tip) {
        if (!LevelChip(id, level, count, flag, tip)) return;
        if (ImGui::GetIO().KeyCtrl) {
            m_filter.Info = m_filter.Warn = m_filter.Error = false;
            flag = true;
        } else {
            flag = !flag;
        }
    };
    chip("info", LogLevel::Info, counts[0], m_filter.Info,
         T("Messages: show or hide. Ctrl+click shows only them."));
    ImGui::SameLine(0.0f, ui.SpacingXS);
    chip("warn", LogLevel::Warn, counts[1], m_filter.Warn,
         T("Warnings: show or hide. Ctrl+click shows only them."));
    ImGui::SameLine(0.0f, ui.SpacingXS);
    chip("error", LogLevel::Error, counts[2], m_filter.Error,
         T("Errors: show or hide. Ctrl+click shows only them."));

    // Поиск — всё, что осталось между уровнями и двумя кнопками справа.
    const float button = ImGui::GetFrameHeight();
    const float right = button * 2.0f + ui.SpacingXS;
    ImGui::SameLine(0.0f, ui.SpacingMD);
    const float searchW = ImGui::GetContentRegionAvail().x - right - ui.SpacingMD;
    if (searchW >= button * 2.5f) {
        if (Sage::UI::SearchField("filter", m_search, sizeof(m_search), T("Search in console..."), searchW))
            m_selected = -1;
        ImGui::SameLine(0.0f, ui.SpacingMD);
    } else {
        // Совсем узко — поиск уступает место кнопкам: без корзины и меню
        // консоль не очистить, а искать в окне шириной с ладонь всё равно нечего.
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - right));
    }
    m_filter.Text = m_search;

    if (EditorIcons::IconOnlyButton("trash", T("Clear the console"))) Clear();
    ImGui::SameLine(0.0f, ui.SpacingXS);
    if (EditorIcons::IconOnlyButton("dots", T("More console options"))) ImGui::OpenPopup("##console_more");

    Sage::UI::MenuScope moreMenu;
    if (ImGui::BeginPopup("##console_more")) {
        EditorIcons::MenuItemToggle("list", T("Collapse repeated lines"), &m_collapse);
        EditorIcons::MenuItemToggle("align-bottom", T("Follow new messages"), &m_autoScroll);
        EditorIcons::MenuItemToggle("clock", T("Show time"), &m_showTime);
        EditorIcons::MenuItemToggle("debug", T("Show debug messages"), &m_filter.Debug);
        ImGui::Separator();
        if (EditorIcons::MenuItem("copy", T("Copy visible lines"))) {
            std::lock_guard<std::mutex> lock(m_mutex);
            ImGui::SetClipboardText(m_log.Text(m_filter).c_str());
        }
        ImGui::EndPopup();
    }
}

void ConsolePanel::DrawRows() {
    ImGui::BeginChild("##console_scroll", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::vector<ConsoleLog::Entry>& entries = m_log.Entries();
    const float line = ImGui::GetTextLineHeight();
    const float stampW = m_showTime ? ImGui::CalcTextSize("00:00:00").x + 10.0f : 0.0f;
    int shown = 0;
    for (int i = 0; i < (int)entries.size(); ++i) {
        const ConsoleLog::Entry& e = entries[(size_t)i];
        if (!m_log.Passes(e, m_filter)) continue;
        // Повторы без свёртки — каждой строкой, но не больше пятисот: окно
        // из десяти тысяч одинаковых строк не читает никто.
        const int copies = m_collapse ? 1 : std::min(e.Repeats, 500);
        const LevelLook look = LookOf(e.Level);
        for (int c = 0; c < copies; ++c) {
            ++shown;
            ImGui::PushID(i * 512 + c);
            // СТРОКА — ВЫДЕЛЯЕМАЯ ЦЕЛИКОМ. Раньше копирование висело на
            // невидимом правом щелчке по тексту; теперь у строки обычное
            // меню, и видно, какая строка выбрана.
            const ImVec2 rowAt = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##row", m_selected == i, ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0.0f, line)))
                m_selected = i;
            if (Sage::UI::MenuScope rowMenu; ImGui::BeginPopupContextItem("##row_menu")) {
                m_selected = i;
                if (EditorIcons::MenuItem("copy", T("Copy message")))
                    ImGui::SetClipboardText(e.Message.c_str());
                if (EditorIcons::MenuItem("copy", T("Copy with time and source")))
                    ImGui::SetClipboardText(ConsoleLog::LineText(e).c_str());
                ImGui::EndPopup();
            }
            // Длинное сообщение в строку не влезает — целиком оно в подсказке.
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) &&
                ImGui::CalcTextSize(e.Message.c_str()).x > ImGui::GetContentRegionAvail().x * 0.6f)
                ImGui::SetTooltip("%s", e.Message.c_str());

            // ТРИ КОЛОНКИ: когда — что — откуда. Время слева, текст по центру
            // и на своём месте у всех строк, источник прижат вправо и
            // приглушён: это ответ на второй вопрос («где искать»), и он не
            // должен спорить с первым («что»).
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float x = rowAt.x;
            if (m_showTime) {
                dl->AddText(ImVec2(x, rowAt.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), e.Stamp.c_str());
                x += stampW;
            }
            EditorIcons::Overlay(dl, x, rowAt.y, line, look.Icon,
                                 glm::vec3(look.Color.x, look.Color.y, look.Color.z));
            x += line + EditorIcons::TextGap();
            std::string text = e.Message;
            const size_t nl = text.find('\n');
            if (nl != std::string::npos) text = text.substr(0, nl) + " …";
            if (m_collapse && e.Repeats > 1) text += "  ×" + std::to_string(e.Repeats);
            dl->AddText(ImVec2(x, rowAt.y), ImGui::ColorConvertFloat4ToU32(look.Color), text.c_str());
            const float textEnd = x + ImGui::CalcTextSize(text.c_str()).x;
            const float catW = ImGui::CalcTextSize(e.Category.c_str()).x;
            const float catX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - catW;
            if (catX > textEnd + 16.0f)
                dl->AddText(ImVec2(catX, rowAt.y), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                            e.Category.c_str());
            ImGui::PopID();
        }
    }
    if (shown == 0) {
        ImGui::TextDisabled("%s", entries.empty() ? T("The console is empty")
                                  : m_search[0]   ? T("The search matched nothing")
                                                  : T("All lines are hidden by the level buttons above"));
    }
    if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}
