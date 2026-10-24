#include "ProblemsView.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorTheme.h"
#include "Project.h"
#include "sage/core/Log.h"
#include "sage/core/Version.h"
#include "sage/scene/Scene.h"
#include "ui/UI.h"
#include "../Localization.h"

namespace fs = std::filesystem;
namespace pg = sage::editor::problems;

// Кнопка уровня — та же, что у журнала (ConsolePanel.cpp): значок и число.
bool ConsoleLevelChip(const char* id, LogLevel level, int count, bool on, const char* tooltip);

namespace {

ImVec4 ColorOf(sage::Severity s) {
    switch (s) {
        case sage::Severity::Fatal:
        case sage::Severity::Error: return EditorTheme::Color(EditorTheme::Role::Danger);
        case sage::Severity::Warning: return EditorTheme::Color(EditorTheme::Role::Warn);
        default: return EditorTheme::Color(EditorTheme::Role::Text);
    }
}

const char* IconOf(sage::Severity s) {
    switch (s) {
        case sage::Severity::Fatal:
        case sage::Severity::Error: return "error";
        case sage::Severity::Warning: return "warn";
        default: return "info";
    }
}

const char* LevelLabel(sage::Severity s) {
    switch (s) {
        case sage::Severity::Fatal: return T("Fatal");
        case sage::Severity::Error: return T("Error");
        case sage::Severity::Warning: return T("Warning");
        default: return T("Info");
    }
}

std::string Where(const sage::Problem& p) {
    std::string w = p.File;
    if (!w.empty() && p.Line > 0) w += ":" + std::to_string(p.Line);
    return w;
}

// Заголовок раздела подробностей — приглушённый, как у свойств инспектора.
void Section(const char* text) {
    ImGui::Spacing();
    ImGui::TextDisabled("%s", text);
}

// Блок кода на подложке: строки кода и пример читаются как код, а не как
// продолжение объяснения.
void CodeBlock(const char* id, const std::string& text) {
    const float lines = (float)std::count(text.begin(), text.end(), '\n') + 1.0f;
    const float h = std::min(lines, 12.0f) * ImGui::GetTextLineHeightWithSpacing() +
                    ImGui::GetStyle().WindowPadding.y * 2.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::BeginChild(id, ImVec2(0.0f, h), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(text.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

double Now() { return ImGui::GetTime(); }

} // namespace

int ProblemsView::OpenErrors() {
    Refresh();
    int n = 0;
    for (const pg::Group& g : m_groups)
        if ((int)g.Worst >= (int)sage::Severity::Error) ++n;
    return n;
}

void ProblemsView::Refresh() {
    const uint64_t rev = sage::Problems::Revision();
    if (rev == m_revision && m_appliedSearch == m_search) return;
    m_revision = rev;
    m_appliedSearch = m_search;
    m_snapshot = sage::Problems::Snapshot();
    m_filter.Text = m_search;
    m_groups = pg::Build(m_snapshot, m_filter);
    // Выбранная проблема снята (исправили) — выбор не висит на пустоте.
    if (m_selected &&
        std::none_of(m_snapshot.begin(), m_snapshot.end(), [&](const sage::Problem& p) { return p.Id == m_selected; }))
        m_selected = 0;
}

void ProblemsView::Draw(EditorHost* host) {
    Refresh();
    // Ничего не выбрано — показываем верхнюю группу: вкладку открывают, чтобы
    // увидеть, ЧТО сломалось, и лишний щелчок до первого ответа не нужен.
    if (m_selected == 0 && !m_groups.empty()) m_selected = m_groups.front().Root->Id;
    DrawToolbar(host);
    ImGui::Separator();

    const sage::Problem* selected = nullptr;
    const pg::Group* group = nullptr;
    for (const pg::Group& g : m_groups) {
        if (g.Root->Id == m_selected) { selected = g.Root; group = &g; }
        for (const sage::Problem* e : g.Effects)
            if (e->Id == m_selected) selected = e;
    }

    // Список сверху, подробности снизу — когда есть что показать. Высота
    // подробностей — почти половина: в них код, шаги и пример, и прокручивать
    // их в щели высотой в три строки бессмысленно.
    const float avail = ImGui::GetContentRegionAvail().y;
    const float detailsH = selected ? std::floor(avail * 0.52f) : 0.0f;
    ImGui::BeginChild("##problems_list", ImVec2(0.0f, avail - detailsH), ImGuiChildFlags_None);
    DrawList();
    ImGui::EndChild();
    if (selected) {
        ImGui::Separator();
        ImGui::BeginChild("##problem_details", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        DrawDetails(host, *selected, group);
        ImGui::EndChild();
    }
}

void ProblemsView::DrawToolbar(EditorHost* host) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    int counts[3] = {0, 0, 0};
    for (const sage::Problem& p : m_snapshot) {
        if ((int)p.Level >= (int)sage::Severity::Error) ++counts[2];
        else if (p.Level == sage::Severity::Warning) ++counts[1];
        else ++counts[0];
    }
    auto chip = [&](const char* id, LogLevel level, int count, bool& flag, const char* tip) {
        if (!ConsoleLevelChip(id, level, count, flag, tip)) return;
        flag = !flag;
        m_revision = 0;   // перестроить группы под новый фильтр
    };
    chip("p_err", LogLevel::Error, counts[2], m_filter.Errors, T("Errors: show or hide"));
    ImGui::SameLine(0.0f, ui.SpacingXS);
    chip("p_warn", LogLevel::Warn, counts[1], m_filter.Warnings, T("Warnings: show or hide"));
    ImGui::SameLine(0.0f, ui.SpacingXS);
    chip("p_info", LogLevel::Info, counts[0], m_filter.Info, T("Notes: show or hide"));

    const float button = ImGui::GetFrameHeight();
    const float right = button * 2.0f + ui.SpacingXS;
    ImGui::SameLine(0.0f, ui.SpacingMD);
    const float searchW = ImGui::GetContentRegionAvail().x - right - ui.SpacingMD;
    if (searchW >= button * 2.5f) {
        Sage::UI::SearchField("problems_filter", m_search, sizeof(m_search), T("Search problems..."), searchW);
        ImGui::SameLine(0.0f, ui.SpacingMD);
    } else {
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - right));
    }

    // Главное действие вкладки — отчёт для помощи: одной кнопкой в буфер.
    if (EditorIcons::IconOnlyButton("copy", T("Copy a diagnostic report (to send for help)"))) {
        ImGui::SetClipboardText(ReportText(host).c_str());
        m_notice = T("The report is copied: paste it into a message");
        m_noticeUntil = Now() + 3.0;
    }
    ImGui::SameLine(0.0f, ui.SpacingXS);
    if (EditorIcons::IconOnlyButton("dots", T("More problem options"))) ImGui::OpenPopup("##problems_more");
    Sage::UI::MenuScope moreMenu;
    if (ImGui::BeginPopup("##problems_more")) {
        if (EditorIcons::MenuItem("save", T("Save the diagnostic report to a file"), nullptr, host != nullptr)) {
            char stamp[32];
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
            const fs::path dir = host->CurrentProject().Loaded() ? host->CurrentProject().Dir() : fs::path(".");
            const fs::path file = dir / (std::string("sage-report-") + stamp + ".md");
            std::ofstream out(file, std::ios::binary);
            out << ReportText(host);
            host->SetStatusMessage(out ? T("Report saved: ") + file.string() : T("The report was not saved"));
        }
        ImGui::Separator();
        if (EditorIcons::MenuItem("trash", T("Clear the list (problems that repeat come back)")))
            sage::Problems::Clear();
        ImGui::EndPopup();
    }
}

void ProblemsView::DrawList() {
    if (m_groups.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", m_snapshot.empty() ? T("No problems: everything works")
                                                      : T("Nothing matches the filter"));
        return;
    }
    const float line = ImGui::GetTextLineHeight();
    auto row = [&](const sage::Problem& p, bool root, const pg::Group* g, float indent) {
        ImGui::PushID((int)(p.Id & 0x7fffffff));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##problem", m_selected == p.Id, ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, line)))
            m_selected = p.Id;
        if (Sage::UI::MenuScope rowMenu; ImGui::BeginPopupContextItem("##problem_menu")) {
            m_selected = p.Id;
            if (EditorIcons::MenuItem("copy", T("Copy the problem"))) ImGui::SetClipboardText(p.Format().c_str());
            ImGui::EndPopup();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float x = at.x + indent;
        // Стрелка раскрытия — только у группы с последствиями; щелчок по ней
        // раскрывает, щелчок по строке — выбирает.
        if (root && g && !g->Effects.empty()) {
            const bool open = m_expanded.count(p.Id) != 0;
            const float cx = x + line * 0.5f, cy = at.y + line * 0.5f, r = line * 0.22f;
            const ImU32 arrow = ImGui::GetColorU32(ImGuiCol_Text);
            if (open) dl->AddTriangleFilled(ImVec2(cx - r, cy - r * 0.6f), ImVec2(cx + r, cy - r * 0.6f), ImVec2(cx, cy + r * 0.8f), arrow);
            else dl->AddTriangleFilled(ImVec2(cx - r * 0.6f, cy - r), ImVec2(cx + r * 0.8f, cy), ImVec2(cx - r * 0.6f, cy + r), arrow);
            const ImVec2 mouse = ImGui::GetMousePos();
            if (ImGui::IsItemClicked() && mouse.x < x + line) {
                if (open) m_expanded.erase(p.Id);
                else m_expanded.insert(p.Id);
            }
        }
        x += line + 2.0f;
        const ImVec4 col = ColorOf(p.Level);
        EditorIcons::Overlay(dl, x, at.y, line, IconOf(p.Level), glm::vec3(col.x, col.y, col.z));
        x += line + EditorIcons::TextGap();

        // Справа: место и счётчики; заголовок занимает всё, что осталось.
        std::string tail = Where(p);
        if (p.Count > 1) tail += (tail.empty() ? "" : "  ") + std::string("×") + std::to_string(p.Count);
        const float tailW = ImGui::CalcTextSize(tail.c_str()).x;
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        std::string badge;
        if (root && g && !g->Effects.empty()) {
            char text[96];
            std::snprintf(text, sizeof(text), T("root cause · %d more"), (int)g->Effects.size());
            badge = text;
        }
        const float badgeW = badge.empty() ? 0.0f : ImGui::CalcTextSize(badge.c_str()).x + 14.0f;
        const float titleMax = right - x - tailW - badgeW - 16.0f;
        std::string title = p.Title;
        const size_t nl = title.find('\n');
        if (nl != std::string::npos) title = title.substr(0, nl);
        while (title.size() > 4 && ImGui::CalcTextSize(title.c_str()).x > titleMax) {
            // Обрезаем по символу UTF-8, а не по байту: половина буквы — мусор.
            size_t cut = title.size() - 1;
            while (cut > 0 && ((unsigned char)title[cut] & 0xC0) == 0x80) --cut;
            title = title.substr(0, cut);
            if (ImGui::CalcTextSize((title + "…").c_str()).x <= titleMax) { title += "…"; break; }
        }
        dl->AddText(ImVec2(x, at.y), ImGui::ColorConvertFloat4ToU32(root ? col : ImGui::GetStyleColorVec4(ImGuiCol_Text)),
                    title.c_str());
        float bx = x + ImGui::CalcTextSize(title.c_str()).x + 10.0f;
        if (!badge.empty()) {
            // ПЕРВОПРИЧИНА ВЫДЕЛЕНА: «чинить начинать отсюда», а последствия —
            // счётчиком рядом, не двадцатью равноправными строками.
            const ImVec4 accent = EditorTheme::Color(EditorTheme::Role::Accent);
            dl->AddRectFilled(ImVec2(bx, at.y), ImVec2(bx + badgeW - 4.0f, at.y + line),
                              ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 0.18f)), 4.0f);
            dl->AddText(ImVec2(bx + 5.0f, at.y), ImGui::ColorConvertFloat4ToU32(accent), badge.c_str());
        }
        if (!tail.empty())
            dl->AddText(ImVec2(right - tailW, at.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), tail.c_str());
        ImGui::PopID();
    };
    for (const pg::Group& g : m_groups) {
        row(*g.Root, true, &g, 0.0f);
        if (!g.Effects.empty() && m_expanded.count(g.Root->Id))
            for (const sage::Problem* e : g.Effects) row(*e, false, &g, ImGui::GetTextLineHeight() * 1.5f);
    }
}

void ProblemsView::DrawDetails(EditorHost* host, const sage::Problem& p, const pg::Group* group) {
    const ImVec4 col = ColorOf(p.Level);
    EditorIcons::Inline(IconOf(p.Level), glm::vec3(col.x, col.y, col.z));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(col, "%s", LevelLabel(p.Level));
    if (p.Count > 1) {
        ImGui::SameLine();
        ImGui::TextDisabled(T("· repeated %d times"), p.Count);
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(p.Title.c_str());

    if (!p.File.empty() || !p.Subject.empty()) {
        Section(T("Where"));
        if (!p.File.empty()) {
            std::string where = Where(p);
            if (p.Col > 0) where += ":" + std::to_string(p.Col);
            ImGui::TextUnformatted(where.c_str());
            if (host) {
                ImGui::SameLine();
                if (EditorIcons::Button("open", T("Open"), T("Open the file at this line in the code editor")))
                    OpenFile(host, p);
            }
        }
        if (!p.Subject.empty()) ImGui::Text(T("Object: %s"), p.Subject.c_str());
    }
    if (!p.Excerpt.empty()) CodeBlock("##excerpt", p.Excerpt);
    if (!p.Detail.empty()) {
        Section(T("What happened"));
        ImGui::TextUnformatted(p.Detail.c_str());
    }
    if (!p.Hints.empty()) {
        Section(T("How to fix"));
        for (const std::string& h : p.Hints) ImGui::BulletText("%s", h.c_str());
    }
    if (!p.Example.empty()) {
        Section(T("Fixed example"));
        CodeBlock("##example", p.Example);
        ImGui::PushID("example");
        if (EditorIcons::Button("copy", T("Copy the example"))) ImGui::SetClipboardText(p.Example.c_str());
        ImGui::PopID();
    }
    if (group && !group->Effects.empty()) {
        Section(T("Consequences of this problem"));
        for (const sage::Problem* e : group->Effects) {
            std::string text = e->Title;
            if (!e->Subject.empty() && text.find(e->Subject) == std::string::npos) text += " (" + e->Subject + ")";
            ImGui::BulletText("%s", text.c_str());
        }
        ImGui::TextDisabled("%s", T("They usually go away together with the root cause."));
    }
    ImGui::PopTextWrapPos();
    if (!p.Technical.empty()) {
        ImGui::Spacing();
        if (ImGui::CollapsingHeader(T("Technical details###problem_tech"))) CodeBlock("##technical", p.Technical);
    }
    ImGui::Spacing();
    ImGui::PushID("problem_copy");
    if (EditorIcons::Button("copy", T("Copy the problem"), T("Everything above as text: for a chat or an issue")))
        ImGui::SetClipboardText(p.Format().c_str());
    ImGui::PopID();
    if (!m_notice.empty() && Now() < m_noticeUntil) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_notice.c_str());
    }
}

void ProblemsView::OpenFile(EditorHost* host, const sage::Problem& p) {
    fs::path file = p.File;
    std::error_code ec;
    // Путь ошибки — как его видит проект («assets/scripts/x.lua»): открываем
    // от папки проекта, а не от рабочей папки редактора.
    if (file.is_relative() && host->CurrentProject().Loaded()) {
        const fs::path inProject = host->CurrentProject().Dir() / file;
        if (fs::exists(inProject, ec)) file = inProject;
    }
    if (!host->OpenFileInSystemEditor(file, p.Line)) host->SetStatusMessage(T("The file did not open: ") + file.string());
}

std::string ProblemsView::ReportText(EditorHost* host) {
    std::string context;
    if (host) {
        const Project& project = host->CurrentProject();
        if (project.Loaded()) context += "Проект: " + project.Name() + " (" + project.Dir().string() + ")\n";
        context += "Сцена: " + host->CurrentScene().Name() + ", объектов: " + std::to_string(host->CurrentScene().Count()) + "\n";
        context += std::string("Режим: ") + (host->InPlayMode() ? "Play" : "редактирование") + "\n";
    }
    return sage::Problems::BuildReport(std::string("SAGE Editor ") + kSageEngineVersion, context);
}
