#include "LauncherPanel.h"

#include "../ProjectTemplates.h"
#include "../ProjectTemplateCover.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <ctime>
#include <system_error>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "Project.h"
#include "RecentProjects.h"
#include "sage/core/Paths.h"
#include "../Localization.h"

namespace fs = std::filesystem;

namespace {

// Когда проект открывали в последний раз — по времени правки его файла.
// Отдельного журнала для этого заводить не надо: project.sageproj трогают при
// каждом сохранении настроек, и «неделю назад» из него читается достаточно
// честно, чтобы отличить вчерашний проект от прошлогоднего.
std::string HowLongAgo(const fs::path& projectFile) {
    std::error_code ec;
    const auto stamp = fs::last_write_time(projectFile, ec);
    if (ec) return {};
    const auto age = decltype(stamp)::clock::now() - stamp;
    const long long hours = std::chrono::duration_cast<std::chrono::hours>(age).count();
    if (hours < 1) return T("just now");
    if (hours < 24) return std::to_string(hours) + T(" h ago");
    const long long days = hours / 24;
    if (days == 1) return T("yesterday");
    if (days < 30) return std::to_string(days) + T(" d ago");
    const long long months = days / 30;
    return std::to_string(months) + T(" mo ago");
}

// Путь, укороченный с НАЧАЛА: конец информативнее начала. У
// «/home/вася/Документы/SAGE Projects/Игра» полезны последние сегменты, а не
// то, что она в домашней папке.
std::string ShortPath(const std::string& path, size_t maxLen) {
    if (path.size() <= maxLen) return path;
    return "…" + path.substr(path.size() - maxLen + 1);
}

} // namespace

std::string LauncherPanel::CreateBlockedReason() const {
    if (m_newName[0] == '\0') return T("Enter a project name");
    if (m_newDir[0] == '\0') return T("Choose a folder");

    // Имя проекта становится именем ПАПКИ — символы, которых файловая система
    // не примет, надо отсечь здесь, а не получить невнятную ошибку записи.
    for (const char* c = m_newName; *c; ++c) {
        if (std::strchr("/\\:*?\"<>|", *c)) return T("A name cannot contain / \\ : * ? \" < > |");
    }

    // Выбран шаблон, которого нет на диске: кнопка обязана быть выключена
    // ЗДЕСЬ, а не отказывать после нажатия. Отказ после нажатия человек читает
    // как поломку редактора — он ведь выбрал предложенное.
    if (const ProjectTemplate* tpl = FindProjectTemplate(m_templateId)) {
        if (!ProjectTemplateAvailable(*tpl))
            return T("This template is not installed next to the editor");
    }

    std::error_code ec;
    const fs::path parent(m_newDir);
    const fs::path target = parent / m_newName;
    if (fs::exists(target, ec) && !fs::is_empty(target, ec)) return T("That folder already exists and is not empty");
    return {};
}

void LauncherPanel::DrawRecent(EditorHost& host, RecentProjects& recent, float width,
                               float height) {
    ImGui::BeginChild("##recent", ImVec2(width, height), true);
    ImGui::TextDisabled("%s", T("RECENT"));
    ImGui::Separator();

    // Копия списка: открытие/удаление меняет recent прямо во время обхода.
    const std::vector<std::string> items = recent.List();
    if (items.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", T("Nothing here yet."));
        ImGui::TextWrapped("%s", T("Create a project on the right — it will appear here and open in one click."));
        ImGui::EndChild();
        return;
    }

    std::string removeAfter;   // удаляем ПОСЛЕ обхода: список меняется под нами
    for (size_t i = 0; i < items.size(); ++i) {
        const std::string& path = items[i];
        const fs::path projectFile = fs::path(path) / "project.sageproj";
        std::error_code ec;
        const bool exists = fs::exists(projectFile, ec);

        ImGui::PushID((int)i);
        // Строка целиком — одна кнопка на всю ширину: попасть по ней проще, чем
        // по слову, и выравнивание перестаёт зависеть от длины имени.
        const float rowH = ImGui::GetTextLineHeightWithSpacing() * 2.0f + 6.0f;
        const ImVec2 rowStart = ImGui::GetCursorScreenPos();
        const float rowW = ImGui::GetContentRegionAvail().x - 26.0f;

        if (!exists) ImGui::BeginDisabled();
        if (ImGui::Button("##row", ImVec2(rowW, rowH))) {
            std::string err;
            // Ранний выход отсюда запрещён (см. DrawCreate): кадр обязан
            // достроиться. Дорисовать оставшиеся строки не мешает — список
            // выше взят копией именно на этот случай.
            if (!host.OpenProject(path, err)) m_error = err;
        }
        if (!exists) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());

        // Содержимое рисуем поверх кнопки: имя крупно, путь и давность — серым.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const std::string name = fs::path(path).filename().string();
        dl->AddText(ImVec2(rowStart.x + 8, rowStart.y + 4),
                    ImGui::GetColorU32(exists ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                    name.c_str());
        const std::string sub = exists ? (ShortPath(path, 52) + "   ·   " + HowLongAgo(projectFile))
                                       : (ShortPath(path, 52) + T("   \u00b7   the folder is gone"));
        dl->AddText(ImVec2(rowStart.x + 8, rowStart.y + 4 + ImGui::GetTextLineHeight() + 2),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str());

        // Убрать из списка — отдельной кнопкой справа, с отступом: вплотную к
        // строке её задевают при открытии проекта.
        ImGui::SameLine(0.0f, 6.0f);
        if (EditorIcons::Button("trash", "")) removeAfter = path;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Remove from the list (the project is not deleted)"));
        ImGui::PopID();
    }
    if (!removeAfter.empty()) recent.Remove(removeAfter);
    ImGui::EndChild();
}

// Ширина кнопки «Обзор» вместе с зазором. Считается по шрифту и теме, а не
// зашита числом: подпись, отступы кадра и высота строки зависят от того и
// другого, и зашитое число обрезает кнопку при первой же смене масштаба
// интерфейса — ровно так она и уезжала за край окна.
static float BrowseButtonWidth() {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float iconW = ImGui::GetFrameHeight() * 0.68f;
    return ImGui::CalcTextSize(T("Browse")).x + iconW + st.FramePadding.x * 3.0f + st.ItemSpacing.x;
}

void LauncherPanel::DrawCreate(EditorHost& host) {
    // Своё пространство идентификаторов на весь раздел.
    //
    // Здесь и в DrawOpen стоит кнопка «Обзор» с ОДИНАКОВОЙ подписью, а ImGui
    // берёт идентификатор из подписи — два элемента с одним id в одном окне.
    // Сторож редактора (CheckDuplicateId) честно ругался на это в консоль
    // КАЖДЫЙ КАДР: полсотни красных строк к тому моменту, как человек успевал
    // прочитать заголовок. Хуже ругани последствия: одинаковый id означает
    // общее состояние наведения и нажатия, то есть нажатие на одну кнопку
    // подсвечивает другую.
    ImGui::PushID("create");
    ImGui::TextDisabled("%s", T("NEW PROJECT"));
    ImGui::Separator();

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##name", T("Project name"), m_newName, sizeof(m_newName));

    ImGui::SetNextItemWidth(-BrowseButtonWidth());
    ImGui::InputTextWithHint("##dir", T("Folder"), m_newDir, sizeof(m_newDir));
    ImGui::SameLine();
    if (EditorIcons::Button("folder", T("Browse"))) {
        FileBrowser::Config c;
        c.Title = T("Where to put the project");
        c.Mode = FileBrowser::PickMode::PickFolder;
        c.StartDir = m_newDir;
        m_browser.Open(c);
        m_browseTarget = m_newDir;
        m_browseTargetSize = sizeof(m_newDir);
        m_browseIsProjectFile = false;
    }

    // Итоговая папка — ПОКАЗЫВАЕМ. «Location» + «Name» человек в уме не
    // складывает, а ошибиться папкой на один уровень — обычное дело.
    std::error_code ec;
    const fs::path target = fs::path(m_newDir) / m_newName;
    ImGui::TextDisabled("%s", T("Will create:"));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.80f, 0.92f, 1.0f));
    ImGui::TextWrapped("%s", target.generic_string().c_str());
    ImGui::PopStyleColor();

    // Шаблон: с чего начнётся проект. Тот же список, что в диалоге «Новый
    // проект», — он один на редактор (см. ProjectTemplates.h).
    //
    // КАРТОЧКАМИ С ОБЛОЖКОЙ, а не радиокнопками: три подписи в столбик не
    // говорят, чем «Demo scene» отличается от «Menu and HUD», и выбирают их
    // наугад (см. ProjectTemplateCover.h). Ряд по горизонтали — чтобы
    // варианты сравнивались взглядом, а не пролистыванием.
    ImGui::Spacing();
    ImGui::TextDisabled("%s", T("START FROM"));
    const size_t count = ProjectTemplates().size();
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float cardW =
        std::floor((ImGui::GetContentRegionAvail().x - gap * (float)(count - 1)) / (float)count);
    for (size_t i = 0; i < count; ++i) {
        const ProjectTemplate& tpl = ProjectTemplates()[i];
        if (i > 0) ImGui::SameLine(0.0f, gap);
        if (ProjectTemplateCard(tpl, m_templateId == tpl.Id, cardW)) m_templateId = tpl.Id;
    }
    ImGui::Spacing();

    const std::string blocked = CreateBlockedReason();
    if (!blocked.empty()) ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), "%s", blocked.c_str());

    ImGui::BeginDisabled(!blocked.empty());
    if (ImGui::Button(T("Create"), ImVec2(-1.0f, 0.0f))) {
        std::string err;
        // Папку под проекты создаём сами: по умолчанию это «Документы/SAGE
        // Projects», которой при первом запуске ещё нет, и требовать от
        // человека создать её вручную было бы издевательством.
        fs::create_directories(m_newDir, ec);
        // ВЫХОДИТЬ ОТСЮДА НЕЛЬЗЯ, даже когда всё получилось: мы внутри
        // BeginDisabled, внутри дочернего окна, внутри окна launcher'а. Ранний
        // return пропускал EndDisabled/EndChild/End — и кадр уходил в отрисовку
        // с незакрытыми окнами. Именно так выглядела ошибка «создал проект —
        // и весь редактор стал чёрным прямоугольником». Кадр обязан
        // ДОСТРОИТЬСЯ; launcher не покажется уже следующим кадром, потому что
        // проект открыт.
        if (host.CreateProject(m_newDir, m_newName, m_templateId, err)) m_error.clear();
        else m_error = err;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

void LauncherPanel::DrawOpen(EditorHost& host) {
    ImGui::PushID("open"); // см. DrawCreate: обе кнопки обзора зовутся «Обзор»
    ImGui::Spacing();
    ImGui::TextDisabled("%s", T("OPEN ANOTHER"));
    ImGui::Separator();

    ImGui::SetNextItemWidth(-BrowseButtonWidth());
    ImGui::InputTextWithHint("##open", T("Project folder or .sageproj"), m_openPath,
                             sizeof(m_openPath));
    ImGui::SameLine();
    if (EditorIcons::Button("folder", T("Browse"))) {
        FileBrowser::Config c;
        c.Title = T("Open project");
        c.Filters = {".sageproj"};
        c.FilterLabel = T("SAGE projects (*.sageproj)");
        c.StartDir = m_openPath[0] ? fs::path(m_openPath) : sage::DefaultProjectsDir();
        m_browser.Open(c);
        m_browseTarget = m_openPath;
        m_browseTargetSize = sizeof(m_openPath);
        m_browseIsProjectFile = true;
    }

    ImGui::BeginDisabled(m_openPath[0] == '\0');
    if (ImGui::Button(T("Open"), ImVec2(-1.0f, 0.0f))) {
        std::string err;
        // Ранний выход запрещён по той же причине, что и в DrawCreate: мы
        // внутри BeginDisabled и двух окон, кадр надо достроить.
        if (host.OpenProject(m_openPath, err)) m_error.clear();
        else m_error = err;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}

void LauncherPanel::Draw(EditorHost& host, RecentProjects& recent) {
    // Папка по умолчанию — «Документы/SAGE Projects», а НЕ текущая: текущая —
    // это папка, из которой запустили редактор, то есть его собственная
    // установка (см. sage::DefaultProjectsDir).
    if (m_newDir[0] == '\0') {
        std::snprintf(m_newDir, sizeof(m_newDir), "%s",
                      sage::DefaultProjectsDir().string().c_str());
    }

    // Ответ диалога приходит через кадр после нажатия.
    if (m_browser.Draw() && m_browseTarget) {
        fs::path picked = m_browser.Result();
        // Выбрали сам .sageproj — в поле кладём ПАПКУ: открывается проект, а не
        // файл, и человеку показывать надо то, что он получит.
        if (m_browseIsProjectFile && picked.extension() == ".sageproj")
            picked = picked.parent_path();
        std::snprintf(m_browseTarget, m_browseTargetSize, "%s", picked.string().c_str());
        m_browseTarget = nullptr;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // ВЫСОТА ОКНА СЧИТАЕТСЯ, А НЕ НАЗНАЧЕНА ЧИСЛОМ.
    //
    // Числом она и была — 940x470, — и правый столбец в неё не влезал: раздел
    // «Открыть другой» обрезался нижним краем дочернего окна, то есть поле пути
    // и кнопка «Открыть» не были видны вовсе. Зашитое число живёт ровно до
    // первой смены шрифта, масштаба интерфейса или языка (по-русски подписи
    // длиннее и переносятся), а обложки шаблонов добавили в столбец целый ряд.
    //
    // Считается по тому, что в столбце лежит: девять строк форм и подписей,
    // ряд карточек и запас под строку ошибки и нижнюю кнопку.
    const float windowW = 980.0f;
    const float columnW = windowW * 0.5f;   // столбцы делят окно пополам (см. ниже)
    const float cardW = columnW / 3.0f - ImGui::GetStyle().ItemSpacing.x;
    const float frameH = ImGui::GetFrameHeightWithSpacing();
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    // Правый столбец: семь рамок (два поля и кнопка обзора формы создания, поле
    // и кнопка формы открытия, кнопка «Создать», кнопка «Открыть») и шесть
    // строк текста (два заголовка разделов, «Будет создано», сам путь,
    // «Начать с», строка запрета), плюс ряд карточек.
    const float columnH = frameH * 7.0f + lineH * 6.0f + ProjectTemplateCardHeight(cardW);
    // Сверх столбца: заголовок окна и две строки пояснения над ними.
    const float chromeH = ImGui::GetFrameHeight() + lineH * 2.0f + frameH;
    ImGui::SetNextWindowSize(ImVec2(windowW, columnH + chromeH), ImGuiCond_Appearing);
    ImGui::Begin(T("SAGE Engine" "###SAGE Engine"), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking);

    ImGui::TextUnformatted(T("A project keeps scenes, assets and materials in one folder."));
    ImGui::TextDisabled("%s", T("Everything in the editor works inside a project: asset paths, "
                                "building the game, templates."));
    ImGui::Spacing();

    // Два столбца: слева самое частое действие (открыть вчерашнее), справа —
    // редкое. Раньше всё шло простынёй сверху вниз, и список недавних ничем не
    // выделялся среди двух форм ввода.
    // Столбцы одной высоты, и высота эта — остаток окна за вычетом строки
    // ошибки под ними.
    const float footer = ImGui::GetFrameHeightWithSpacing() * 1.5f;
    const float colH = std::max(260.0f, ImGui::GetContentRegionAvail().y - footer);
    const float leftW = ImGui::GetContentRegionAvail().x * 0.50f;
    DrawRecent(host, recent, leftW, colH);

    ImGui::SameLine();
    ImGui::BeginChild("##actions", ImVec2(0, colH), false);
    DrawCreate(host);
    DrawOpen(host);
    ImGui::EndChild();

    if (!m_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
    }

    ImGui::End();
}
