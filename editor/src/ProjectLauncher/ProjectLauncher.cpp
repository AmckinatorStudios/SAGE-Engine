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

namespace Sage::Launcher {

namespace {

using EditorTheme::Role;

// Кегль из токенов — в единицах ДО масштаба интерфейса: ImGui умножает на
// style.FontScaleMain сам, а токены приходят уже умноженными (см. UIStyle.h).
// Без деления крупные надписи росли бы вдвое на экране со 150 %.
void PushFontSize(float scaledToken) {
    ImGui::PushFont(nullptr, scaledToken / std::max(0.01f, Sage::UI::Scale()));
}

// Пункт левой навигации: тонкий жёлтый указатель слева, подсвеченный фон,
// значок и светлый текст. Одинаковый для всех разделов — иначе «активный»
// начинает выглядеть по-разному в разных местах списка.
bool NavItem(const char* icon, const char* label, bool active) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    const float h = std::floor(ui.ControlHeight * 1.5f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;

    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##nav", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + w, p0.y + h);
    if (active) dl->AddRectFilled(p0, p1, EditorTheme::Color32(Role::Selection), ui.CornerRadius);
    else if (hovered) dl->AddRectFilled(p0, p1, EditorTheme::Color32(Role::Hover), ui.CornerRadius);
    if (active) {
        // Указатель — ТОНКИЙ. Толстая полоса и заливка сразу дают два акцента
        // на один пункт, и жёлтого на экране становится больше, чем смысла.
        dl->AddRectFilled(ImVec2(p0.x, p0.y + ui.SpacingXS),
                          ImVec2(p0.x + 2.0f * Sage::UI::Scale(), p1.y - ui.SpacingXS),
                          EditorTheme::Color32(Role::Accent), 1.0f);
    }

    const ImVec4 col = EditorTheme::Color(active ? Role::Text : Role::TextDim);
    EditorIcons::Overlay(p0.x + ui.SpacingMD, p0.y + (h - ui.IconSize) * 0.5f, ui.IconSize, icon,
                         glm::vec3(col.x, col.y, col.z));
    dl->AddText(ImVec2(p0.x + ui.SpacingMD + ui.IconSize + ui.SpacingSM,
                       p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                ImGui::GetColorU32(col), label);
    return pressed;
}

// Вкладка отбора («Все», «Игры», …). Не ImGui::TabBar: у вкладок ImGui свой
// вид, который не сходится с остальным окном, и своё состояние, которое живёт
// в imgui.ini, — а отбор списка это не «раскладка окон».
bool FilterChip(const char* label, bool active) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    ImGui::PushStyleColor(ImGuiCol_Button,
                          active ? EditorTheme::Color(Role::AccentMuted) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Color(Role::Hover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::Color(Role::Elevated));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          EditorTheme::Color(active ? Role::Accent : Role::TextDim));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui.SpacingMD, ui.PaddingControlY));
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return pressed;
}

// Строка «подпись — значение» в панели подробностей: значок, подпись слева,
// значение справа по краю. Таблицей, а не пробелами: значения обязаны
// начинаться на одной координате, иначе колонка «плывёт».
// width — ширина КОЛОНКИ подробностей. Передаётся, а не берётся из
// GetContentRegionAvail: доступное место тянется до края окна, и значение
// прижималось бы к самому краю панели, без поля.
void DetailRow(const char* icon, const char* label, const std::string& value, float width) {
    if (value.empty()) return;
    const Sage::UI::Style& ui = Sage::UI::Get();
    const ImVec4 dim = EditorTheme::Color(Role::TextDim);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    EditorIcons::Overlay(p0.x, p0.y + (ImGui::GetTextLineHeight() - ui.IconSize) * 0.5f,
                         ui.IconSize, icon, glm::vec3(dim.x, dim.y, dim.z));
    const float labelX = ImGui::GetCursorPosX() + ui.IconSize + ui.SpacingSM;
    ImGui::SetCursorPosX(labelX);
    ImGui::PushStyleColor(ImGuiCol_Text, dim);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    // Значение — правым краем к правому краю колонки. Длинное укорачивается:
    // строка, вылезающая за панель, ломает колонку целиком.
    const std::string shown = Sage::UI::Truncate(value.c_str(), width * 0.6f);
    const float valueW = ImGui::CalcTextSize(shown.c_str()).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(labelX, ImGui::GetCursorPosX()) +
                         std::max(0.0f, (p0.x + width - valueW) - ImGui::GetCursorScreenPos().x));
    ImGui::TextUnformatted(shown.c_str());
}

} // namespace

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

    const float footerH = std::floor(ui.StatusBarHeight + ui.SpacingSM);
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
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ui.SpacingXS);
    ImGui::Indent(ui.PaddingPanel);
    if (!m_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::Danger));
        ImGui::TextUnformatted(m_error.c_str());
        ImGui::PopStyleColor();
    } else if (!m_status.empty()) {
        Sage::UI::TextSecondary("%s", m_status.c_str());
    } else {
        // ПУСТАЯ строка состояния — это тоже элемент. Сдвинуть курсор и ничего
        // не подать значит «расширить окно курсором»: ImGui отвечает на это
        // окном с красной руганью поверх экрана, и на стартовом окне оно
        // встречало бы человека первым.
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
    }
    ImGui::Unindent(ui.PaddingPanel);

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
    const float markX = ui.PaddingPanel + ui.IconSizeLarge + ui.SpacingSM;
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
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui.IconSize + ui.SpacingSM);
    Sage::UI::TextSecondary("SAGE Engine");
    ImGui::Indent(ui.IconSize + ui.SpacingSM);
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
    if (ImGui::BeginPopup("##projectmenu")) {
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

// ---------------------------------------------------------------------------
//  Подробности выбранного проекта
// ---------------------------------------------------------------------------

void ProjectLauncher::DrawDetails(EditorHost& host, ProjectDatabase& db) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
    ImGui::Indent(ui.PaddingPanel);
    const float width = ImGui::GetContentRegionAvail().x - ui.PaddingPanel;

    const ProjectEntry* entry = db.Find(m_selected);
    if (!entry) {
        Sage::UI::EmptyState(T("No project selected"),
                             T("Pick a project on the left to see what is inside."));
        ImGui::Unindent(ui.PaddingPanel);
        return;
    }

    // Крупная обложка — тот же рисунок, что на карточке: узнаётся с одного
    // взгляда, что выбран именно тот проект.
    const ImVec2 shot0 = ImGui::GetCursorScreenPos();
    const ImVec2 shot1(shot0.x + width, shot0.y + std::floor(width * 9.0f / 16.0f));
    m_thumbs.Draw(*entry, shot0, shot1, ui.CornerRadius);
    ImGui::GetWindowDrawList()->AddRect(shot0, shot1, EditorTheme::Color32(Role::Line),
                                        ui.CornerRadius);
    ImGui::Dummy(ImVec2(width, shot1.y - shot0.y + ui.SpacingMD));

    const ImVec4 dim = EditorTheme::Color(Role::TextDim);
    const ImVec2 iconPos = ImGui::GetCursorScreenPos();
    EditorIcons::Overlay(iconPos.x, iconPos.y + (ImGui::GetTextLineHeight() - ui.IconSize) * 0.5f,
                         ui.IconSize, "cube", glm::vec3(dim.x, dim.y, dim.z));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui.IconSize + ui.SpacingSM);
    PushFontSize(ui.FontTitle);
    ImGui::TextWrapped("%s", entry->Name.c_str());
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::TextFaint));
    ImGui::TextWrapped("%s", entry->Path.c_str());
    ImGui::PopStyleColor();
    if (!entry->Description.empty()) {
        ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
        Sage::UI::TextSecondary("%s", entry->Description.c_str());
    }
    if (entry->Missing) {
        ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::Warn));
        ImGui::TextWrapped("%s", T("The folder is gone. The disk may be disconnected — the "
                                   "entry is kept, nothing is deleted."));
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0.0f, ui.SpacingMD));
    DetailRow("plus", T("Created"), FormatStamp(entry->Created), width);
    DetailRow("clock", T("Last modified"), HumanStamp(entry->Modified), width);
    DetailRow("gear", T("Engine version"),
              entry->EngineVersion.empty() ? std::string(T("unknown")) : entry->EngineVersion,
              width);
    DetailRow("cube", T("Project type"), ProjectKindLabel(entry->Kind), width);

    // ГЛАВНАЯ КНОПКА ЭКРАНА — и единственная крупная жёлтая. Всё остальное
    // рядом с ней намеренно тише: если акцентных кнопок две, акцента нет.
    ImGui::Dummy(ImVec2(0.0f, ui.SpacingLG));
    ImGui::BeginDisabled(entry->Missing);
    // Подпись с отступом слева под значок: он рисуется поверх кнопки ровно на
    // месте этих пробелов, поэтому текст и значок стоят как одно целое при
    // любой длине перевода.
    const std::string openLabel = std::string("     ") + T("Open Project");
    if (Sage::UI::Button(openLabel.c_str(), Sage::UI::ButtonStyle::Primary,
                         ImVec2(width, ImGui::GetFrameHeight() * 1.6f))) {
        OpenProject(host, db, entry->Path);
    }
    const ImVec2 btnMin = ImGui::GetItemRectMin();
    const ImVec2 btnMax = ImGui::GetItemRectMax();
    const ImVec4 onAccent = EditorTheme::Color(Role::TextOnAccent);
    EditorIcons::Overlay(btnMin.x + width * 0.5f -
                             ImGui::CalcTextSize(openLabel.c_str()).x * 0.5f + ui.SpacingXS,
                         (btnMin.y + btnMax.y) * 0.5f - ui.IconSize * 0.5f, ui.IconSize, "play",
                         glm::vec3(onAccent.x, onAccent.y, onAccent.z));
    ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
    if (Sage::UI::Button(T("Open Folder"), Sage::UI::ButtonStyle::Secondary,
                         ImVec2(width, 0.0f))) {
        RunAction(host, db, ProjectAction::OpenFolder, entry->Path);
    }
    ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0.0f, ui.SpacingLG));
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::TextDim));
    ImGui::TextUnformatted(T("Quick Actions"));
    ImGui::PopStyleColor();
    Sage::UI::Separator();

    struct Quick { ProjectAction Action; const char* Icon; const char* Label; bool Danger; };
    static const Quick kQuick[] = {
        {ProjectAction::Duplicate, "copy", "Duplicate", false},
        {ProjectAction::Rename, "pencil", "Rename", false},
        {ProjectAction::Delete, "trash", "Delete", true},
    };
    for (const Quick& q : kQuick) {
        ImGui::PushID(q.Label);
        ImGui::BeginDisabled(entry->Missing && q.Action != ProjectAction::Delete);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              EditorTheme::Color(q.Danger ? Role::Danger : Role::Text));
        // Подпись прижата ВЛЕВО, под значком: по центру она читалась бы как
        // отдельная кнопка, а не как строка списка действий.
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        const std::string label = std::string("     ") + T(q.Label);
        const bool pressed = Sage::UI::Button(label.c_str(), Sage::UI::ButtonStyle::Ghost,
                                              ImVec2(width, 0.0f));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec4 col = EditorTheme::Color(q.Danger ? Role::Danger : Role::TextDim);
        EditorIcons::Overlay(rowMin.x + ui.SpacingMD,
                             rowMin.y + (ImGui::GetFrameHeight() - ui.IconSize) * 0.5f, ui.IconSize,
                             q.Icon, glm::vec3(col.x, col.y, col.z));
        ImGui::EndDisabled();
        ImGui::PopID();
        if (pressed) RunAction(host, db, q.Action, entry->Path);
    }
    ImGui::Unindent(ui.PaddingPanel);
}

// ---------------------------------------------------------------------------
//  Шаблоны и настройки
// ---------------------------------------------------------------------------

void ProjectLauncher::DrawTemplates() {
    const Sage::UI::Style& ui = Sage::UI::Get();
    ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
    ImGui::Indent(ui.PaddingPanel);
    PushFontSize(ui.FontDisplay);
    ImGui::TextUnformatted(T("Templates"));
    ImGui::PopFont();
    Sage::UI::TextSecondary("%s", T("What a new project starts from"));
    ImGui::Dummy(ImVec2(0.0f, ui.SpacingMD));

    ImGui::BeginChild("##templates", ImVec2(ImGui::GetContentRegionAvail().x - ui.PaddingPanel,
                                            0.0f));
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = ui.SpacingMD;
    const int columns = CardColumns(avail, 260.0f * Sage::UI::Scale(), gap);
    const float cardW = CardWidth(avail, columns, gap);
    int index = 0;
    for (const ProjectTemplate& tpl : ProjectTemplates()) {
        if (index % columns != 0) ImGui::SameLine(0.0f, gap);
        // Та же карточка с обложкой, что и в диалоге создания: список шаблонов
        // один на редактор, и вид у него обязан быть один (ProjectTemplateCover.h).
        if (ProjectTemplateCard(tpl, m_templateId == tpl.Id, cardW)) {
            m_templateId = tpl.Id;
            m_dialog = Dialog::Create;
            m_dialogOpening = true;
            m_dialogTarget.clear();
        }
        ++index;
    }
    if (index == 0) {
        Sage::UI::EmptyState(T("No templates installed"),
                             T("Built-in templates come with the editor; extra ones are "
                               "installed from the editor's Templates window."));
    }
    ImGui::EndChild();
    ImGui::Unindent(ui.PaddingPanel);
}

void ProjectLauncher::DrawSettings(ProjectDatabase& db) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
    ImGui::Indent(ui.PaddingPanel);
    PushFontSize(ui.FontDisplay);
    ImGui::TextUnformatted(T("Settings"));
    ImGui::PopFont();
    Sage::UI::TextSecondary("%s", T("How the editor looks and where new projects go"));
    ImGui::Dummy(ImVec2(0.0f, ui.SpacingMD));

    ImGui::BeginChild("##settings", ImVec2(ImGui::GetContentRegionAvail().x - ui.PaddingPanel,
                                           0.0f));
    const float fieldW = std::min(ImGui::GetContentRegionAvail().x,
                                  std::floor(420.0f * Sage::UI::Scale()));

    // Язык и оформление живут ЗДЕСЬ ЖЕ, а не только в меню редактора: до
    // открытия проекта меню не существует, а человек, у которого интерфейс на
    // чужом языке, должен иметь возможность это исправить на первом экране.
    ImGui::TextUnformatted(T("Language"));
    ImGui::SetNextItemWidth(fieldW);
    const std::string current = sage::editor::CurrentLanguageCode();
    std::string currentName = current;
    for (const auto& lang : sage::editor::AvailableLanguages())
        if (lang.Code == current) currentName = lang.Name;
    if (ImGui::BeginCombo("##lang", currentName.c_str())) {
        for (const auto& lang : sage::editor::AvailableLanguages()) {
            if (ImGui::Selectable(lang.Name.c_str(), lang.Code == current))
                sage::editor::SetLanguage(lang.Code);
        }
        ImGui::EndCombo();
    }

    ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
    ImGui::TextUnformatted(T("Theme"));
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::BeginCombo("##theme", EditorTheme::Current().Name.c_str())) {
        for (const EditorTheme::Theme& theme : EditorTheme::Themes()) {
            if (ImGui::Selectable(theme.Name.c_str(), theme.Id == EditorTheme::CurrentId()))
                EditorTheme::SetTheme(theme.Id);
        }
        ImGui::EndCombo();
    }

    ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
    ImGui::TextUnformatted(T("Interface scale"));
    float scale = EditorTheme::UiScale();
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::SliderFloat("##scale", &scale, 0.75f, 2.0f, "%.2fx")) EditorTheme::SetUiScale(scale);

    ImGui::Dummy(ImVec2(0.0f, ui.SpacingMD));
    ImGui::TextUnformatted(T("Where new projects go"));
    const float browseW = ImGui::CalcTextSize(T("Browse")).x + ui.IconSize + ui.SpacingXL;
    ImGui::SetNextItemWidth(fieldW - browseW - ui.SpacingSM);
    if (ImGui::InputText("##projects-dir", m_dirBuf, sizeof(m_dirBuf))) SavePrefs();
    ImGui::SameLine(0.0f, ui.SpacingSM);
    if (EditorIcons::Button("folder", T("Browse"))) {
        FileBrowser::Config c;
        c.Title = T("Where to put new projects");
        c.Mode = FileBrowser::PickMode::PickFolder;
        c.StartDir = m_dirBuf;
        m_browser.Open(c);
        m_browseTarget = m_dirBuf;
        m_browseTargetSize = sizeof(m_dirBuf);
        m_browseImport = false;
    }

    ImGui::Dummy(ImVec2(0.0f, ui.SpacingLG));
    Sage::UI::Separator();
    // Где лежат данные самого стартового окна. Не украшение: когда список
    // проектов ведёт себя не так, как ждут, первый вопрос — «а где он вообще».
    Sage::UI::TextSecondary("%s", T("Launcher data"));
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::TextFaint));
    ImGui::TextWrapped("%s", ProjectDatabase::StoragePath().c_str());
    ImGui::TextWrapped("%s", sage::PathToUtf8(ProjectThumbnail::CacheDir()).c_str());
    ImGui::PopStyleColor();
    char counts[128];
    std::snprintf(counts, sizeof(counts), T("Projects in the list: %d"), (int)db.All().size());
    Sage::UI::TextSecondary("%s", counts);
    ImGui::EndChild();
    ImGui::Unindent(ui.PaddingPanel);
}

// ---------------------------------------------------------------------------
//  Диалоги: создание, переименование, дубликат, удаление
// ---------------------------------------------------------------------------

void ProjectLauncher::DrawDialogs(EditorHost& host, ProjectDatabase& db) {
    if (m_dialog == Dialog::None) return;   // ни одного Begin не открыто — выход честный
    const Sage::UI::Style& ui = Sage::UI::Get();

    // Идентификатор окна ОДИН на все четыре диалога (часть после «###»):
    // открытым может быть только один, а подпись у каждого своя.
    const char* title = T("Create New Project");
    switch (m_dialog) {
        case Dialog::Rename: title = T("Rename Project"); break;
        case Dialog::Duplicate: title = T("Duplicate Project"); break;
        case Dialog::Delete: title = T("Delete Project"); break;
        default: break;
    }
    const std::string id = std::string(title) + "###launcher-dialog";
    if (m_dialogOpening) {
        ImGui::OpenPopup(id.c_str());
        m_dialogOpening = false;
    }
    // Диалог ЖИВЁТ В ГЛАВНОМ ОКНЕ, а не в своём окне системы.
    //
    // При включённом multi-viewport (он включён — панели вытаскивают из дока)
    // ImGui выносит всплывающее окно в отдельное окно ОС, стоит ему не влезть в
    // главное. Для панели это удобство, для стартового окна — беда: диалог
    // создания проекта уезжает вторым окном на панель задач, отдельно от
    // затемнённого экрана, который его ждёт.
    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(mainViewport->ID);
    ImGui::SetNextWindowPos(mainViewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // И не выше главного окна: длинный перевод или мелкий экран иначе прячут
    // кнопку «Создать проект» за нижним краем.
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                        ImVec2(mainViewport->WorkSize.x * 0.9f,
                                               mainViewport->WorkSize.y * 0.9f));
    const float fieldW = std::floor(420.0f * Sage::UI::Scale());
    if (!ImGui::BeginPopupModal(id.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // Окно закрыли крестиком или Esc — состояние диалога надо сбросить,
        // иначе следующий вызов откроет его же с чужой целью.
        m_dialog = Dialog::None;
        return;
    }

    const ProjectEntry* target = db.Find(m_dialogTarget);
    bool close = false;

    if (m_dialog == Dialog::Create) {
        ImGui::TextUnformatted(T("Project Name"));
        ImGui::SetNextItemWidth(fieldW);
        ImGui::InputTextWithHint("##name", T("My Game"), m_nameBuf, sizeof(m_nameBuf));

        ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
        ImGui::TextUnformatted(T("Location"));
        const float browseW = ImGui::CalcTextSize(T("Browse")).x + ui.IconSize + ui.SpacingXL;
        ImGui::SetNextItemWidth(fieldW - browseW - ui.SpacingSM);
        ImGui::InputTextWithHint("##dir", T("Folder"), m_dirBuf, sizeof(m_dirBuf));
        ImGui::SameLine(0.0f, ui.SpacingSM);
        if (EditorIcons::Button("folder", T("Browse"))) {
            FileBrowser::Config c;
            c.Title = T("Where to put the project");
            c.Mode = FileBrowser::PickMode::PickFolder;
            c.StartDir = m_dirBuf;
            m_browser.Open(c);
            m_browseTarget = m_dirBuf;
            m_browseTargetSize = sizeof(m_dirBuf);
            m_browseImport = false;
        }
        // Итоговая папка — ПОКАЗЫВАЕМ. «Место» и «имя» человек в уме не
        // складывает, а ошибиться папкой на один уровень — обычное дело.
        Sage::UI::TextSecondary("%s", T("Will create:"));
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::Info));
        ImGui::TextWrapped("%s", (fs::path(m_dirBuf) / m_nameBuf).generic_string().c_str());
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
        ImGui::TextUnformatted(T("Template"));
        const size_t count = ProjectTemplates().size();
        if (count > 0) {
            const float gap = ui.SpacingSM;
            const int columns = std::min((int)count, 3);
            const float cardW = CardWidth(fieldW, columns, gap);
            int index = 0;
            for (const ProjectTemplate& tpl : ProjectTemplates()) {
                if (index % columns != 0) ImGui::SameLine(0.0f, gap);
                if (ProjectTemplateCard(tpl, m_templateId == tpl.Id, cardW)) m_templateId = tpl.Id;
                ++index;
            }
        }

        ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
        ImGui::TextUnformatted(T("Project Type"));
        ImGui::SetNextItemWidth(fieldW);
        if (ImGui::BeginCombo("##kind", ProjectKindLabel(m_newKind))) {
            // Шаблон как ТИП не предлагаем: тип «шаблон» проект получает, когда
            // его таковым делают, а не при создании.
            for (int i = 0; i < (int)ProjectKind::Template; ++i) {
                const ProjectKind kind = (ProjectKind)i;
                if (ImGui::Selectable(ProjectKindLabel(kind), kind == m_newKind))
                    m_newKind = kind;
            }
            ImGui::EndCombo();
        }

        ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
        ImGui::TextUnformatted(T("Description"));
        ImGui::SetNextItemWidth(fieldW);
        ImGui::InputTextWithHint("##desc", T("What this project is about (optional)"), m_descBuf,
                                 sizeof(m_descBuf));

        const std::string blocked = CreateBlockedReason();
        ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
        if (!blocked.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::Warn));
            ImGui::TextWrapped("%s", blocked.c_str());
            ImGui::PopStyleColor();
        }
        if (Sage::UI::Button(T("Cancel"), Sage::UI::ButtonStyle::Secondary)) close = true;
        ImGui::SameLine(0.0f, ui.SpacingSM);
        ImGui::BeginDisabled(!blocked.empty());
        if (Sage::UI::Button(T("Create Project"), Sage::UI::ButtonStyle::Primary)) {
            std::error_code ec;
            // Папку под проекты создаём сами: «Документы/SAGE Projects» при
            // первом запуске ещё нет, и требовать создать её вручную было бы
            // издевательством.
            fs::create_directories(m_dirBuf, ec);
            std::string err;
            if (host.CreateProject(m_dirBuf, m_nameBuf, m_templateId, err)) {
                // Тип и описание дописываем в дескриптор ПОСЛЕ создания:
                // создание проекта — общий контракт редактора, и расширять его
                // ради двух полей стартового окна незачем (см. WriteMetadata).
                const fs::path dir = fs::path(m_dirBuf) / m_nameBuf;
                std::string metaErr;
                if (!ProjectDatabase::WriteMetadata(dir, m_newKind, m_descBuf, metaErr))
                    LOG_WARN("Launcher") << "Не удалось записать тип проекта: " << metaErr;
                db.Refresh(sage::PathToUtf8(dir));
                m_selected = sage::PathToUtf8(dir);
                m_error.clear();
                m_descBuf[0] = '\0';
                SavePrefs();
                close = true;
            } else {
                m_error = err;
            }
        }
        ImGui::EndDisabled();
    } else if (m_dialog == Dialog::Rename || m_dialog == Dialog::Duplicate) {
        const bool duplicate = m_dialog == Dialog::Duplicate;
        Sage::UI::TextSecondary("%s", m_dialogTarget.c_str());
        ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
        ImGui::TextUnformatted(duplicate ? T("Name of the copy") : T("Project Name"));
        ImGui::SetNextItemWidth(fieldW);
        ImGui::InputText("##rename", m_nameBuf, sizeof(m_nameBuf));
        if (duplicate) {
            Sage::UI::TextSecondary("%s", T("Will create:"));
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::Info));
            ImGui::TextWrapped("%s", (fs::path(m_dialogTarget).parent_path() / m_nameBuf)
                                         .generic_string()
                                         .c_str());
            ImGui::PopStyleColor();
        } else {
            // Переименование меняет ИМЯ, а не папку — говорим об этом прямо,
            // иначе человек ждёт, что переедет и путь.
            Sage::UI::TextSecondary("%s", T("The folder keeps its path — only the name changes."));
        }

        ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
        if (Sage::UI::Button(T("Cancel"), Sage::UI::ButtonStyle::Secondary)) close = true;
        ImGui::SameLine(0.0f, ui.SpacingSM);
        ImGui::BeginDisabled(m_nameBuf[0] == '\0' || !target);
        if (Sage::UI::Button(duplicate ? T("Duplicate") : T("Rename"),
                             Sage::UI::ButtonStyle::Primary)) {
            std::string err;
            bool ok = false;
            if (duplicate) {
                std::string created;
                ok = db.Duplicate(m_dialogTarget, m_nameBuf, err, &created);
                if (ok) m_selected = created;
            } else {
                ok = db.Rename(m_dialogTarget, m_nameBuf, err);
            }
            if (ok) {
                m_error.clear();
                m_status = duplicate ? T("Project duplicated") : T("Project renamed");
                close = true;
            } else {
                m_error = err;
            }
        }
        ImGui::EndDisabled();
    } else if (m_dialog == Dialog::Delete) {
        // УДАЛЕНИЕ С ДИСКА. Единственное необратимое действие окна, поэтому оно
        // называет папку целиком и говорит, что вернуть её будет нечем.
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::Danger));
        ImGui::TextWrapped("%s", T("The project folder will be deleted from disk together with "
                                   "everything inside it."));
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::TextFaint));
        ImGui::TextWrapped("%s", m_dialogTarget.c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
        Sage::UI::TextSecondary("%s", T("There is no undo for this. To just hide the project "
                                        "from the list, use «Remove from Launcher»."));

        ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
        if (Sage::UI::Button(T("Cancel"), Sage::UI::ButtonStyle::Secondary)) close = true;
        ImGui::SameLine(0.0f, ui.SpacingSM);
        if (Sage::UI::Button(T("Delete from disk"), Sage::UI::ButtonStyle::Danger)) {
            std::string err;
            if (db.DeleteFromDisk(m_dialogTarget, err)) {
                if (m_selected == m_dialogTarget) m_selected.clear();
                m_error.clear();
                m_status = T("Project deleted");
                close = true;
            } else {
                m_error = err;
            }
        }
    }

    if (close) {
        ImGui::CloseCurrentPopup();
        m_dialog = Dialog::None;
        m_dialogTarget.clear();
    }
    ImGui::EndPopup();
}

} // namespace Sage::Launcher
