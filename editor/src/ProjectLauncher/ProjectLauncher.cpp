#include "ProjectLauncher.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>

#include "imgui_internal.h"

#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorPrefs.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../Project.h"
#include "../ProjectTemplateCover.h"
#include "../ProjectTemplates.h"
#include "../ui/UI.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/core/Version.h"

namespace fs = std::filesystem;

#include "ProjectLauncherDetail.h"

namespace Sage::Launcher {


// ---------------------------------------------------------------------------
//  Настройки окна (переживают перезапуск)
// ---------------------------------------------------------------------------

void ProjectLauncher::LoadPrefs() {
    namespace prefs = sage::editor::prefs;
    const int section = prefs::GetInt("launcher.section", (int)LauncherSection::Projects);
    const int filter = prefs::GetInt("launcher.filter", (int)ProjectFilter::All);
    const int sort = prefs::GetInt("launcher.sort", (int)ProjectSort::Recent);
    m_section = (section >= 0 && section < (int)LauncherSection::Count) ? (LauncherSection)section
                                                                       : LauncherSection::Projects;
    m_filter = (filter >= 0 && filter < (int)ProjectFilter::Count) ? (ProjectFilter)filter
                                                                  : ProjectFilter::All;
    m_sort = (sort >= 0 && sort < (int)ProjectSort::Count) ? (ProjectSort)sort : ProjectSort::Recent;
    m_layout = prefs::GetBool("launcher.list_view", false) ? CardLayout::List : CardLayout::Grid;
    m_templateId = DefaultProjectTemplate();

    // Папка для новых проектов: своя, если человек её выбирал, иначе
    // «Документы/SAGE Projects». НЕ текущая папка процесса — это установка
    // редактора, куда проект класть нельзя (снесёт следующей распаковкой).
    const std::string dir = prefs::GetString("launcher.projects_dir",
                                             sage::PathToUtf8(sage::DefaultProjectsDir()));
    std::snprintf(m_dirBuf, sizeof(m_dirBuf), "%s", dir.c_str());

    // ЭКРАН ИЗ ОКРУЖЕНИЯ — для проверки скриншотом (SAGE_LAUNCHER_SCREEN).
    //
    // Разделы и диалог создания проекта открываются мышью, а прогон без
    // человека мышью не владеет: увидеть «Шаблоны» или окно создания в кадре
    // иначе нельзя вообще, и любая их поломка обнаруживалась бы только у того,
    // кто скачал сборку. Тот же приём, что у остальных окон редактора
    // (SAGE_EDITOR_SHOW_SETTINGS, SAGE_EDITOR_SHOW_PROFILER).
    const std::string screen = sage::EnvString("SAGE_LAUNCHER_SCREEN");
    if (screen == "projects") m_section = LauncherSection::Projects;
    else if (screen == "recent") m_section = LauncherSection::Recent;
    else if (screen == "templates") m_section = LauncherSection::Templates;
    else if (screen == "settings") m_section = LauncherSection::Settings;
    else if (screen == "create") {
        m_section = LauncherSection::Projects;
        m_dialog = Dialog::Create;
        m_dialogOpening = true;
    }
    m_prefsLoaded = true;
}

void ProjectLauncher::SavePrefs() const {
    namespace prefs = sage::editor::prefs;
    prefs::SetInt("launcher.section", (int)m_section);
    prefs::SetInt("launcher.filter", (int)m_filter);
    prefs::SetInt("launcher.sort", (int)m_sort);
    prefs::SetBool("launcher.list_view", m_layout == CardLayout::List);
    prefs::SetString("launcher.projects_dir", m_dirBuf);
}

// ---------------------------------------------------------------------------
//  Действия над проектом
// ---------------------------------------------------------------------------

void ProjectLauncher::OpenProject(EditorHost& host, ProjectDatabase& db,
                                  const std::string& path) {
    std::string err;
    // ВЫХОДИТЬ ИЗ КАДРА ЗДЕСЬ НЕЛЬЗЯ (и потому это не return из Draw): мы
    // внутри дочерних окон, и ранний выход оставил бы кадр с незакрытыми
    // Begin/End — весь редактор превращается в чёрный прямоугольник. Кадр
    // обязан достроиться; окно не покажется уже следующим кадром, потому что
    // проект открыт.
    if (host.OpenProject(path, err)) {
        m_error.clear();
        m_status.clear();
    } else {
        m_error = err;
        db.Refresh(path);   // возможно, папку унесли — пометим запись
    }
}

int ProjectLauncher::AcceptDroppedFiles(const std::vector<std::string>& paths,
                                        ProjectDatabase& db) {
    int added = 0;
    for (const std::string& raw : paths) {
        std::string err;
        std::string imported;
        if (db.Import(raw, err, &imported)) {
            ++added;
            // Выбранным становится тот проект, который принесли, а не
            // последний в списке: Import ничего не добавляет, если проект уже
            // был, и «последний» оказался бы чужим.
            m_selected = imported;
        } else {
            // Отказ ГОВОРИТ, что не так. «Ничего не произошло» — худший ответ
            // на перетаскивание: человек не знает, промахнулся он мимо окна или
            // принёс не то.
            m_error = err;
        }
    }
    if (added > 0) {
        m_error.clear();
        char buf[128];
        std::snprintf(buf, sizeof(buf), T("Added to the list: %d"), added);
        m_status = buf;
    }
    return added;
}

void ProjectLauncher::RunAction(EditorHost& host, ProjectDatabase& db, ProjectAction action,
                                const std::string& path) {
    const ProjectEntry* entry = db.Find(path);
    switch (action) {
        case ProjectAction::Open:
            OpenProject(host, db, path);
            break;
        case ProjectAction::OpenFolder:
            if (!RevealInFileManager(fs::path(path)))
                m_error = std::string(T("Failed to open the folder: ")) + path;
            break;
        case ProjectAction::Rename:
        case ProjectAction::Duplicate:
        case ProjectAction::Delete:
            // Всё, что меняет проект, спрашивает подтверждение или имя —
            // диалогом, а не сразу по нажатию пункта меню.
            m_dialog = action == ProjectAction::Rename ? Dialog::Rename
                       : action == ProjectAction::Duplicate ? Dialog::Duplicate
                                                            : Dialog::Delete;
            m_dialogOpening = true;
            m_dialogTarget = path;
            std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s",
                          entry ? entry->Name.c_str() : fs::path(path).filename().string().c_str());
            if (action == ProjectAction::Duplicate) {
                const std::string copy = std::string(m_nameBuf) + " " + T("copy");
                std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", copy.c_str());
            }
            break;
        case ProjectAction::Forget:
            db.Forget(path);
            if (m_selected == path) m_selected.clear();
            m_status = T("Removed from the list. The project itself is still on disk.");
            break;
        case ProjectAction::None:
        default:
            break;
    }
}

std::string ProjectLauncher::CreateBlockedReason() const {
    if (m_nameBuf[0] == '\0') return T("Enter a project name");
    if (m_dirBuf[0] == '\0') return T("Choose a folder");
    // Имя проекта становится именем ПАПКИ: символы, которых не примет файловая
    // система, надо отсечь здесь, а не получить невнятную ошибку записи.
    for (const char* c = m_nameBuf; *c; ++c) {
        if (std::strchr("/\\:*?\"<>|", *c)) return T("A name cannot contain / \\ : * ? \" < > |");
    }
    if (const ProjectTemplate* tpl = FindProjectTemplate(m_templateId)) {
        if (!ProjectTemplateAvailable(*tpl))
            return T("This template is not installed next to the editor");
    }
    std::error_code ec;
    const fs::path target = fs::path(m_dirBuf) / m_nameBuf;
    if (fs::exists(target, ec) && !fs::is_empty(target, ec))
        return T("That folder already exists and is not empty");
    return {};
}

// ---------------------------------------------------------------------------
//  Каркас экрана
// ---------------------------------------------------------------------------

bool ProjectLauncher::Draw(EditorHost& host, ProjectDatabase& db) {
    if (!m_prefsLoaded) LoadPrefs();
    // Готовые обложки забираем ДО отрисовки карточек: иначе картинка,
    // приехавшая в этом кадре, показалась бы только в следующем.
    m_thumbs.Pump();

    const Sage::UI::Style& ui = Sage::UI::Get();
    // Закрыть окно можно, только когда есть куда возвращаться. Без проекта
    // «закрыть» означало бы пустой редактор — состояние, которого больше нет.
    const bool canClose = host.CurrentProject().Loaded();
    bool closeRequested = false;

    // ОКНО НА ВЕСЬ ВЬЮПОРТ. Стартовое окно — это экран приложения, а не
    // диалог поверх редактора: плавающее окно с заголовком, которое можно
    // подвинуть и уменьшить, читается как «одна из панелей», хотя за ним
    // ничего нет.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::Color(Role::Bg));
    // Подписи у окна нет — заголовок скрыт, и в строке остаётся только
    // идентификатор ImGui. Переводить нечего: то, что видно человеку, рисует
    // верхняя панель.
    ImGui::Begin("###SAGE Launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(3);

    DrawTopBar(canClose, closeRequested);

    // НИЖНЯЯ СТРОКА ЗАНИМАЕТ МЕСТО, ТОЛЬКО КОГДА ЕЙ ЕСТЬ ЧТО СКАЗАТЬ.
    //
    // Раньше место под неё резервировалось всегда и по высоте строки состояния
    // редактора (24 плюс отступ), а рисовалась там одна строка текста — да и то
    // пустая, пока нет ни ошибки, ни сообщения. Под карточками оставалась
    // пустая полоса во всю ширину: место, которое ни на что не отвечает и
    // читается как обрезанный список.
    const bool hasFooter = !m_error.empty() || !m_status.empty();
    const float footerH =
        hasFooter ? std::floor(ui.SpacingXS * 2.0f + ImGui::GetTextLineHeight()) : 0.0f;
    const float bodyH = std::max(ui.ControlHeight, ImGui::GetContentRegionAvail().y - footerH);
    const float availW = ImGui::GetContentRegionAvail().x;
    const float sideW = std::floor(196.0f * Sage::UI::Scale());
    const float detailW = std::floor(316.0f * Sage::UI::Scale());
    // Подробности показываем только там, где есть выбранный проект: в шаблонах
    // и настройках эта колонка была бы пустым столбцом в треть экрана.
    const bool showDetails = (m_section == LauncherSection::Projects ||
                              m_section == LauncherSection::Recent) &&
                             availW > sideW + detailW * 2.0f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(Role::Surface));
    ImGui::BeginChild("##sidebar", ImVec2(sideW, bodyH));
    DrawSidebar();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::SameLine(0.0f, 0.0f);
    const float centerW = availW - sideW - (showDetails ? detailW : 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(Role::Bg));
    ImGui::BeginChild("##center", ImVec2(centerW, bodyH));
    switch (m_section) {
        case LauncherSection::Templates: DrawTemplates(); break;
        case LauncherSection::Settings: DrawSettings(db); break;
        case LauncherSection::Projects:
        case LauncherSection::Recent:
        default: DrawBrowser(host, db); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    if (showDetails) {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(Role::Surface));
        ImGui::BeginChild("##details", ImVec2(0.0f, bodyH));
        DrawDetails(host, db);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // Нижняя строка: ошибка красным, сообщение — приглушённо. Одна на окно —
    // отказ импорта и отказ открытия говорят в одно и то же место.
    if (hasFooter) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ui.SpacingXS);
        ImGui::Indent(ui.PaddingPanel);
        if (!m_error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::Danger));
            ImGui::TextUnformatted(m_error.c_str());
            ImGui::PopStyleColor();
        } else {
            Sage::UI::TextSecondary("%s", m_status.c_str());
        }
        ImGui::Unindent(ui.PaddingPanel);
    }

    DrawDialogs(host, db);

    // Ответ файлового диалога приходит через кадр после нажатия.
    if (m_browser.Draw()) {
        fs::path picked = m_browser.Result();
        if (m_browseImport) {
            std::string err;
            std::string imported;
            if (db.Import(picked, err, &imported)) {
                m_error.clear();
                m_selected = imported;
                m_status = T("Project added to the list");
            } else {
                m_error = err;
            }
            m_browseImport = false;
        } else if (m_browseTarget) {
            std::snprintf(m_browseTarget, m_browseTargetSize, "%s",
                          sage::PathToUtf8(picked).c_str());
            m_browseTarget = nullptr;
        }
    }

    ImGui::End();
    ImGui::PopStyleColor();
    return !closeRequested;
}

void ProjectLauncher::DrawTopBar(bool canClose, bool& closeRequested) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    const float barH = std::floor(ui.ToolbarHeight + ui.SpacingSM);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(Role::Surface));
    ImGui::BeginChild("##topbar", ImVec2(0.0f, barH));
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 p1(p0.x + ImGui::GetWindowWidth(), p0.y + barH);

    ImGui::SetCursorPos(ImVec2(ui.PaddingPanel, (barH - ImGui::GetFrameHeight()) * 0.5f));
    const ImVec2 markPos = ImGui::GetCursorScreenPos();
    const ImVec4 accent = EditorTheme::Color(Role::Accent);
    EditorIcons::Overlay(markPos.x, markPos.y + (ImGui::GetFrameHeight() - ui.IconSizeLarge) * 0.5f,
                         ui.IconSizeLarge, "cube", glm::vec3(accent.x, accent.y, accent.z));
    // «SAGE» — ЛОГОТИП, а не подпись интерфейса: он не переводится и рисуется
    // списком отрисовки, чтобы не попадать в каталог переводов пустой строкой,
    // равной самой себе.
    const float markX = ui.PaddingPanel + ui.IconSizeLarge + EditorIcons::TextGap();
    const ImVec2 wordPos(p0.x + markX, markPos.y + (ImGui::GetFrameHeight() -
                                                    ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ui.FontTitle, wordPos,
                                        EditorTheme::Color32(Role::Text), "SAGE");
    const float wordW = ImGui::GetFont()->CalcTextSizeA(ui.FontTitle, FLT_MAX, 0.0f, "SAGE").x;
    ImGui::SetCursorPosX(markX + wordW + ui.SpacingXS);
    ImGui::AlignTextToFramePadding();
    Sage::UI::TextSecondary("%s", T("Editor"));

    // Справа: поиск, настройки и (если есть куда возвращаться) выход из окна.
    const float searchW = std::floor(280.0f * Sage::UI::Scale());
    const float iconW = ImGui::GetFrameHeight() + ui.SpacingSM;
    float rightX = ImGui::GetWindowWidth() - ui.PaddingPanel - iconW - (canClose ? iconW : 0.0f) -
                   searchW;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightX));
    if (m_focusSearch) {
        // Ctrl+K ставит курсор в поиск. Фокус запрашивается ДО поля — ImGui
        // применяет его к следующему элементу.
        ImGui::SetKeyboardFocusHere();
        m_focusSearch = false;
    }
    Sage::UI::SearchField("##search", m_search, sizeof(m_search),
                          T("Search projects...   Ctrl+K"), searchW);
    ImGui::SameLine(0.0f, ui.SpacingSM);
    if (Sage::UI::IconButton("gear", T("Settings"), m_section == LauncherSection::Settings)) {
        m_section = LauncherSection::Settings;
        SavePrefs();
    }
    if (canClose) {
        ImGui::SameLine(0.0f, ui.SpacingXS);
        if (Sage::UI::IconButton("open", T("Back to the editor"))) closeRequested = true;
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    // Линия под шапкой: слои разделяет тонкая граница, а не тень.
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p1.y),
                                        EditorTheme::Color32(Role::Line), 1.0f);
}

void ProjectLauncher::DrawSidebar() {
    const Sage::UI::Style& ui = Sage::UI::Get();
    ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
    ImGui::Indent(ui.SpacingSM);
    ImGui::PushItemWidth(-ui.SpacingSM);

    struct Item { LauncherSection Section; const char* Icon; const char* Label; };
    static const Item kItems[] = {
        {LauncherSection::Projects, "folder", "Projects"},
        {LauncherSection::Recent, "clock", "Recent"},
        {LauncherSection::Templates, "layout", "Templates"},
        {LauncherSection::Settings, "gear", "Settings"},
    };
    for (const Item& item : kItems) {
        if (NavItem(item.Icon, T(item.Label), m_section == item.Section)) {
            m_section = item.Section;
            SavePrefs();
        }
    }
    ImGui::PopItemWidth();
    ImGui::Unindent(ui.SpacingSM);

    // Внизу — марка и версия движка: тот же угол, где её ищут в редакторе.
    const float bottom = ImGui::GetWindowHeight() - ui.ControlHeight * 2.0f;
    if (bottom > ImGui::GetCursorPosY()) ImGui::SetCursorPosY(bottom);
    ImGui::Indent(ui.PaddingPanel);
    const ImVec4 dim = EditorTheme::Color(Role::TextDim);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    EditorIcons::Overlay(pos.x, pos.y, ui.IconSize, "cube", glm::vec3(dim.x, dim.y, dim.z));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui.IconSize + EditorIcons::TextGap());
    Sage::UI::TextSecondary("SAGE Engine");
    ImGui::Indent(ui.IconSize + EditorIcons::TextGap());
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::TextFaint));
    ImGui::Text("v%s", kSageEngineVersion);
    ImGui::PopStyleColor();
    ImGui::Unindent(ui.IconSize + ui.SpacingSM);
    ImGui::Unindent(ui.PaddingPanel);
}

// ---------------------------------------------------------------------------
//  Список проектов
// ---------------------------------------------------------------------------

void ProjectLauncher::DrawBrowser(EditorHost& host, ProjectDatabase& db) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    const bool recentOnly = m_section == LauncherSection::Recent;

    ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
    ImGui::Indent(ui.PaddingPanel);
    const float contentW = ImGui::GetContentRegionAvail().x - ui.PaddingPanel;

    // --- шапка: название раздела слева, действия справа --------------------
    const float headTop = ImGui::GetCursorPosY();
    PushFontSize(ui.FontDisplay);
    ImGui::TextUnformatted(recentOnly ? T("Recent") : T("Projects"));
    ImGui::PopFont();
    Sage::UI::TextSecondary("%s", recentOnly ? T("Projects you opened lately")
                                             : T("Manage your games and worlds"));

    // Кнопки стоят в правом верхнем углу шапки — там же, где в редакторе стоят
    // действия панелей. Ширина считается по подписям: по-русски они длиннее, и
    // зашитое число обрезало бы «Новый проект» на первом же переводе.
    const float importW = ImGui::CalcTextSize(T("Import")).x + ui.IconSize + ui.SpacingXL;
    const float newW = ImGui::CalcTextSize(T("New Project")).x + ui.IconSize + ui.SpacingXL;
    const float buttonsY = headTop + ui.SpacingXS;
    ImGui::SetCursorPos(ImVec2(contentW - importW - newW - ui.SpacingSM, buttonsY));
    if (EditorIcons::Button("import", T("Import"), T("Add an existing project to the list"))) {
        FileBrowser::Config c;
        c.Title = T("Import project");
        c.Filters = {".sageproj"};
        c.FilterLabel = T("SAGE projects (*.sageproj)");
        c.StartDir = m_dirBuf[0] ? fs::path(m_dirBuf) : sage::DefaultProjectsDir();
        m_browser.Open(c);
        m_browseImport = true;
        m_browseTarget = nullptr;
    }
    ImGui::SameLine(0.0f, ui.SpacingSM);
    ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(Role::Accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Color(Role::AccentHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::Color(Role::AccentActive));
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::TextOnAccent));
    if (EditorIcons::Button("plus", T("New Project"), T("Create a new project (Ctrl+Shift+N)"))) {
        m_dialog = Dialog::Create;
        m_dialogOpening = true;
        m_dialogTarget.clear();
    }
    ImGui::PopStyleColor(4);

    // --- отбор и вид -------------------------------------------------------
    ImGui::Dummy(ImVec2(0.0f, ui.SpacingMD));
    for (int i = 0; i < (int)ProjectFilter::Count; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, ui.SpacingXS);
        const ProjectFilter filter = (ProjectFilter)i;
        if (FilterChip(ProjectFilterLabel(filter), m_filter == filter)) {
            m_filter = filter;
            SavePrefs();
        }
    }

    const float toggleW = (ImGui::GetFrameHeight() + ui.SpacingXS) * 2.0f;
    const float sortW = std::floor(190.0f * Sage::UI::Scale());
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                  ui.PaddingPanel + contentW - toggleW - sortW - ui.SpacingSM));
    ImGui::SetNextItemWidth(sortW);
    if (ImGui::BeginCombo("##sort", ProjectSortLabel(m_sort))) {
        for (int i = 0; i < (int)ProjectSort::Count; ++i) {
            const ProjectSort sort = (ProjectSort)i;
            if (ImGui::Selectable(ProjectSortLabel(sort), m_sort == sort)) {
                m_sort = sort;
                SavePrefs();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0.0f, ui.SpacingSM);
    if (Sage::UI::IconButton("grid", T("Grid"), m_layout == CardLayout::Grid)) {
        m_layout = CardLayout::Grid;
        SavePrefs();
    }
    ImGui::SameLine(0.0f, ui.SpacingXS);
    if (Sage::UI::IconButton("list", T("List"), m_layout == CardLayout::List)) {
        m_layout = CardLayout::List;
        SavePrefs();
    }

    // --- сам список --------------------------------------------------------
    std::vector<int> shown = db.Query(m_filter, m_search, recentOnly ? ProjectSort::Recent : m_sort);
    if (recentOnly) {
        shown.erase(std::remove_if(shown.begin(), shown.end(),
                                   [&](int i) { return db.All()[(size_t)i].Opened == 0; }),
                    shown.end());
    }

    // ПЕРВЫЙ ПРОЕКТ ВЫБРАН САМ. Панель подробностей — половина смысла экрана, и
    // встречать человека пустым столбцом «выберите проект» незачем: список уже
    // отсортирован так, что сверху лежит самое нужное.
    if (!shown.empty() && db.Find(m_selected) == nullptr)
        m_selected = db.All()[(size_t)shown.front()].Path;

    ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
    ImGui::BeginChild("##cards", ImVec2(contentW, 0.0f));
    if (shown.empty()) {
        if (db.Empty()) {
            Sage::UI::EmptyState(T("No projects yet"),
                                 T("Create one with «New Project» or bring an existing folder "
                                   "in with «Import» — you can also just drop it into this "
                                   "window."));
        } else {
            Sage::UI::EmptyState(T("Nothing matches"),
                                 T("Try another search or another tab."));
        }
    } else {
        // ВИРТУАЛИЗАЦИЯ. Рисуются только те строки, что попадают в видимую
        // полосу. Без этого пятьсот проектов означают пятьсот карточек с
        // текстом и обложками КАЖДЫЙ КАДР — и окно, которое «тормозит на
        // ровном месте», хотя показывает дюжину карточек.
        const float gap = ui.SpacingMD;
        const float avail = ImGui::GetContentRegionAvail().x;
        const int columns = m_layout == CardLayout::Grid
                                ? CardColumns(avail, 250.0f * Sage::UI::Scale(), gap)
                                : 1;
        const float cardW = CardWidth(avail, columns, gap);
        const float cardH = CardHeight(m_layout, cardW);
        const float stride = cardH + gap;
        const int rows = ((int)shown.size() + columns - 1) / columns;

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(avail, std::max(0.0f, (float)rows * stride - gap)));

        const float viewTop = ImGui::GetWindowPos().y;
        const float viewBottom = viewTop + ImGui::GetWindowHeight();
        const int firstRow = std::max(0, (int)std::floor((viewTop - origin.y) / stride));
        const int lastRow = std::min(rows, (int)std::ceil((viewBottom - origin.y) / stride) + 1);

        for (int row = firstRow; row < lastRow; ++row) {
            for (int col = 0; col < columns; ++col) {
                const int slot = row * columns + col;
                if (slot >= (int)shown.size()) break;
                const ProjectEntry& entry = db.All()[(size_t)shown[(size_t)slot]];
                ImGui::SetCursorScreenPos(ImVec2(origin.x + (float)col * (cardW + gap),
                                                 origin.y + (float)row * stride));
                const CardEvent ev = DrawProjectCard(m_layout, entry, m_selected == entry.Path,
                                                     ImVec2(cardW, cardH), m_thumbs);
                if (ev.Selected) m_selected = entry.Path;
                if (ev.MenuRequested) {
                    m_menuTarget = entry.Path;
                    m_menuRequested = true;
                }
                if (ev.Activated) OpenProject(host, db, entry.Path);
            }
        }
        // Курсор после карточек стоит там, где кончилась последняя, — то есть
        // где угодно. Возвращаем его под весь список и подаём элемент: иначе
        // ImGui считает, что окно растягивают курсором, и ругается окном
        // поверх экрана.
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + (float)rows * stride));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }

    // Контекстное меню одно на список: открытым может быть только оно, а над
    // каким проектом — помнит m_menuTarget.
    if (m_menuRequested) {
        ImGui::OpenPopup("##projectmenu");
        m_menuRequested = false;
    }
    ProjectAction action = ProjectAction::None;
    // Отступы темы для меню: всплывающее окно наследует стиль, действующий в
    // момент открытия (см. Sage::UI::MenuScope). Время жизни MenuScope
    // держится в границах if — иначе деструктор снялся бы после EndChild()
    // ниже, и ImGui решил бы, что стиль не сняли вовсе.
    if (Sage::UI::MenuScope projectMenu; ImGui::BeginPopup("##projectmenu")) {
        if (const ProjectEntry* entry = db.Find(m_menuTarget)) action = DrawProjectMenuItems(*entry);
        else ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (action != ProjectAction::None) RunAction(host, db, action, m_menuTarget);

    HandleShortcuts(host, db, shown);
    ImGui::EndChild();
    ImGui::Unindent(ui.PaddingPanel);
}

void ProjectLauncher::HandleShortcuts(EditorHost& host, ProjectDatabase& db,
                                      const std::vector<int>& shown) {
    // Клавиши работают, только когда окно списка в фокусе и человек не набирает
    // текст: иначе Delete в поле поиска удалял бы проект.
    if (ImGui::IsAnyItemActive() || ImGui::GetIO().WantTextInput) return;

    ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_K, false)) m_focusSearch = true;
    if (ctrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        m_dialog = Dialog::Create;
        m_dialogOpening = true;
        m_dialogTarget.clear();
    }
    if (shown.empty()) return;

    // Стрелки ходят по ОТОБРАННОМУ списку, а не по всей базе: человек видит
    // именно его, и «следующий» обязан значить следующий на экране.
    int current = -1;
    for (size_t i = 0; i < shown.size(); ++i) {
        if (db.All()[(size_t)shown[i]].Path == m_selected) { current = (int)i; break; }
    }
    const int last = (int)shown.size() - 1;
    int next = current;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) next = current < 0 ? 0 : std::min(last, current + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) next = current <= 0 ? 0 : current - 1;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) && m_layout == CardLayout::Grid)
        next = current < 0 ? 0 : std::min(last, current + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) && m_layout == CardLayout::Grid)
        next = current <= 0 ? 0 : current - 1;
    if (next != current && next >= 0) m_selected = db.All()[(size_t)shown[(size_t)next]].Path;

    if (m_selected.empty()) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
        OpenProject(host, db, m_selected);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        // Delete — это «убрать из списка» с подтверждением, а не удаление с
        // диска: клавиша слишком близко к промаху, чтобы стирать чужую работу.
        RunAction(host, db, ProjectAction::Forget, m_selected);
    }
}


} // namespace Sage::Launcher
