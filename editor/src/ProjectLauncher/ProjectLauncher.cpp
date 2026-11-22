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
    // Папка для новых проектов: своя, если человек её выбирал, иначе
    // «Документы/SAGE Projects». НЕ текущая папка процесса — это установка
    // редактора, куда проект класть нельзя (снесёт следующей распаковкой).
    const std::string dir = prefs::GetString("launcher.projects_dir",
                                             sage::PathToUtf8(sage::DefaultProjectsDir()));
    std::snprintf(m_dirBuf, sizeof(m_dirBuf), "%s", dir.c_str());

    // ДИАЛОГ ИЗ ОКРУЖЕНИЯ — для проверки скриншотом (SAGE_LAUNCHER_SCREEN).
    // Диалог открывается мышью, а прогон без человека мышью не владеет.
    // «projects» — сам список (по умолчанию), «create» — окно создания.
    if (sage::EnvString("SAGE_LAUNCHER_SCREEN") == "create") StartCreate();
    m_prefsLoaded = true;
}

void ProjectLauncher::SavePrefs() const {
    sage::editor::prefs::SetString("launcher.projects_dir", m_dirBuf);
}

void ProjectLauncher::StartCreate() {
    m_dialog = Dialog::Create;
    m_dialogOpening = true;
    m_focusName = true;
    m_dialogTarget.clear();
}

void ProjectLauncher::StartOpenBrowser() {
    FileBrowser::Config c;
    c.Title = T("Open project");
    c.Filters = {".sageproj"};
    c.FilterLabel = T("SAGE projects (*.sageproj)");
    c.StartDir = m_dirBuf[0] ? fs::path(m_dirBuf) : sage::DefaultProjectsDir();
    m_browser.Open(c);
    m_browseOpen = true;
    m_browseTarget = nullptr;
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
    // Готовые обложки забираем ДО отрисовки строк: иначе картинка, приехавшая
    // в этом кадре, показалась бы только в следующем.
    m_thumbs.Pump();

    const Sage::UI::Style& ui = Sage::UI::Get();
    // Закрыть окно можно, только когда есть куда возвращаться.
    const bool canClose = host.CurrentProject().Loaded();
    bool closeRequested = false;

    // ОКНО НА ВЕСЬ ВЬЮПОРТ: это экран приложения, а не диалог поверх редактора.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::Color(Role::Bg));
    ImGui::Begin("###SAGE Launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(3);

    DrawTopBar(host, db, canClose, closeRequested);

    // Нижняя строка занимает место, только когда ей есть что сказать.
    const bool hasFooter = !m_error.empty() || !m_status.empty();
    const float footerH =
        hasFooter ? std::floor(ui.SpacingXS * 2.0f + ImGui::GetTextLineHeight()) : 0.0f;
    const float bodyH = std::max(ui.ControlHeight, ImGui::GetContentRegionAvail().y - footerH);

    // Список — по центру с ограниченной шириной: на широком экране строка в
    // тысячу пикселей читается хуже, чем колонка.
    const float availW = ImGui::GetContentRegionAvail().x;
    const float maxW = std::floor(980.0f * Sage::UI::Scale());
    const float colW = std::min(availW - ui.PaddingPanel * 2.0f, maxW);
    ImGui::BeginChild("##body", ImVec2(0.0f, bodyH));
    m_colW = colW;
    ImGui::SetCursorPosX(std::max(ui.PaddingPanel, (availW - colW) * 0.5f));
    ImGui::BeginGroup();
    DrawList(host, db);
    ImGui::EndGroup();
    ImGui::EndChild();

    // Одна строка на окно: отказ импорта и отказ открытия говорят в одно место.
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
        if (m_browseOpen) {
            m_browseOpen = false;
            // Import добавляет проект в базу, если его там не было, и отдаёт
            // корень; открываем сразу — для этого человек и выбирал файл.
            std::string err;
            std::string imported;
            if (db.Import(picked, err, &imported)) {
                m_selected = imported;
                OpenProject(host, db, imported);
            } else {
                m_error = err;
            }
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

void ProjectLauncher::DrawTopBar(EditorHost& host, ProjectDatabase& db, bool canClose,
                                 bool& closeRequested) {
    (void)host;
    (void)db;
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
    // списком отрисовки, чтобы не попадать в каталог переводов.
    const float markX = ui.PaddingPanel + ui.IconSizeLarge + EditorIcons::TextGap();
    const ImVec2 wordPos(p0.x + markX, markPos.y + (ImGui::GetFrameHeight() -
                                                    ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ui.FontTitle, wordPos,
                                        EditorTheme::Color32(Role::Text), "SAGE");

    // Справа: поиск, «Открыть», «Новый проект» и (если есть куда) возврат.
    // Ширины кнопок считаются по подписям: по-русски они длиннее.
    const float searchW = std::floor(280.0f * Sage::UI::Scale());
    const float openW = ImGui::CalcTextSize(T("Open Project")).x + ui.IconSize + ui.SpacingXL;
    const float newW = ImGui::CalcTextSize(T("New Project")).x + ui.IconSize + ui.SpacingXL;
    const float iconW = ImGui::GetFrameHeight() + ui.SpacingXS;
    const float rightX = ImGui::GetWindowWidth() - ui.PaddingPanel - searchW - openW - newW -
                         ui.SpacingSM * 2.0f - (canClose ? iconW : 0.0f);
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightX));
    if (m_focusSearch) {
        // Ctrl+K ставит курсор в поиск. Фокус запрашивается ДО поля.
        ImGui::SetKeyboardFocusHere();
        m_focusSearch = false;
    }
    Sage::UI::SearchField("##search", m_search, sizeof(m_search),
                          T("Search projects...   Ctrl+K"), searchW);
    ImGui::SameLine(0.0f, ui.SpacingSM);
    if (EditorIcons::Button("folder", T("Open Project"), T("Open an existing project (Ctrl+O)")))
        StartOpenBrowser();
    ImGui::SameLine(0.0f, ui.SpacingSM);
    ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(Role::Accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Color(Role::AccentHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::Color(Role::AccentActive));
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::TextOnAccent));
    if (EditorIcons::Button("plus", T("New Project"), T("Create a new project (Ctrl+Shift+N)")))
        StartCreate();
    ImGui::PopStyleColor(4);
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

// ---------------------------------------------------------------------------
//  Список проектов
// ---------------------------------------------------------------------------

void ProjectLauncher::DrawEmpty(bool searching) {
    const Sage::UI::Style& ui = Sage::UI::Get();
    if (searching) {
        Sage::UI::EmptyState(T("Nothing matches"), T("Try another search."));
        return;
    }
    Sage::UI::EmptyState(T("No projects yet"),
                         T("Create a new project or open an existing one — you can also just "
                           "drop a project folder into this window."));
    ImGui::Dummy(ImVec2(0.0f, ui.SpacingMD));
    const float newW = ImGui::CalcTextSize(T("New Project")).x + ui.IconSize + ui.SpacingXL;
    const float openW = ImGui::CalcTextSize(T("Open Project")).x + ui.IconSize + ui.SpacingXL;
    const float total = newW + openW + ui.SpacingSM;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0f, (ImGui::GetContentRegionAvail().x - total) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(Role::Accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Color(Role::AccentHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::Color(Role::AccentActive));
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::TextOnAccent));
    if (EditorIcons::Button("plus", T("New Project"))) StartCreate();
    ImGui::PopStyleColor(4);
    ImGui::SameLine(0.0f, ui.SpacingSM);
    if (EditorIcons::Button("folder", T("Open Project"))) StartOpenBrowser();
}

void ProjectLauncher::DrawList(EditorHost& host, ProjectDatabase& db) {
    const Sage::UI::Style& ui = Sage::UI::Get();

    ImGui::Dummy(ImVec2(0.0f, ui.SpacingLG));
    PushFontSize(ui.FontTitle);
    ImGui::TextUnformatted(T("Projects"));
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));

    // Порядок один — по последнему открытию; выбирать его не из чего.
    const std::vector<int> shown = db.Query(ProjectFilter::All, m_search, ProjectSort::Recent);

    // Первый проект выбран сам: Enter сразу открывает самое нужное.
    if (!shown.empty() && db.Find(m_selected) == nullptr)
        m_selected = db.All()[(size_t)shown.front()].Path;

    ImGui::BeginChild("##rows", ImVec2(std::min(m_colW, ImGui::GetContentRegionAvail().x), 0.0f));
    if (shown.empty()) {
        DrawEmpty(!db.Empty());
    } else {
        // ВИРТУАЛИЗАЦИЯ: рисуются только строки видимой полосы, иначе пятьсот
        // проектов — это пятьсот строк с обложками каждый кадр.
        const float gap = ui.SpacingXS;
        const float avail = ImGui::GetContentRegionAvail().x;
        const float stride = RowHeight() + gap;
        const int rows = (int)shown.size();

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(avail, std::max(0.0f, (float)rows * stride - gap)));

        const float viewTop = ImGui::GetWindowPos().y;
        const float viewBottom = viewTop + ImGui::GetWindowHeight();
        const int first = std::max(0, (int)std::floor((viewTop - origin.y) / stride));
        const int last = std::min(rows, (int)std::ceil((viewBottom - origin.y) / stride) + 1);

        for (int row = first; row < last; ++row) {
            const ProjectEntry& entry = db.All()[(size_t)shown[(size_t)row]];
            ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + (float)row * stride));
            const CardEvent ev = DrawProjectRow(entry, m_selected == entry.Path, avail, m_thumbs);
            if (ev.Selected) m_selected = entry.Path;
            if (ev.MenuRequested) {
                m_menuTarget = entry.Path;
                m_menuRequested = true;
            }
            if (ev.Activated) OpenProject(host, db, entry.Path);
        }
        // Курсор после строк возвращаем под весь список и подаём элемент: иначе
        // ImGui считает, что окно растягивают курсором.
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + (float)rows * stride));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }

    // Контекстное меню одно на список: над каким проектом — помнит m_menuTarget.
    if (m_menuRequested) {
        ImGui::OpenPopup("##projectmenu");
        m_menuRequested = false;
    }
    ProjectAction action = ProjectAction::None;
    // Время жизни MenuScope держится в границах if — иначе деструктор снялся бы
    // после EndChild() ниже (см. Sage::UI::MenuScope).
    if (Sage::UI::MenuScope projectMenu; ImGui::BeginPopup("##projectmenu")) {
        if (const ProjectEntry* entry = db.Find(m_menuTarget)) action = DrawProjectMenuItems(*entry);
        else ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (action != ProjectAction::None) RunAction(host, db, action, m_menuTarget);

    HandleShortcuts(host, db, shown);
    ImGui::EndChild();
}

void ProjectLauncher::HandleShortcuts(EditorHost& host, ProjectDatabase& db,
                                      const std::vector<int>& shown) {
    // Клавиши работают, только когда человек не набирает текст: иначе Delete в
    // поле поиска удалял бы проект.
    if (ImGui::IsAnyItemActive() || ImGui::GetIO().WantTextInput) return;
    // Под открытым диалогом окно списка тоже не должно реагировать на Enter.
    if (m_dialog != Dialog::None || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;

    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false)) m_focusSearch = true;
    if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N, false)) StartCreate();
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) StartOpenBrowser();
    if (shown.empty()) return;

    // Стрелки ходят по ОТОБРАННОМУ списку: «следующий» значит следующий на экране.
    int current = -1;
    for (size_t i = 0; i < shown.size(); ++i) {
        if (db.All()[(size_t)shown[i]].Path == m_selected) { current = (int)i; break; }
    }
    const int last = (int)shown.size() - 1;
    int next = current;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) next = current < 0 ? 0 : std::min(last, current + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) next = current <= 0 ? 0 : current - 1;
    if (next != current && next >= 0) m_selected = db.All()[(size_t)shown[(size_t)next]].Path;

    if (m_selected.empty()) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
        OpenProject(host, db, m_selected);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        // Delete — «убрать из списка», не удаление с диска: клавиша слишком
        // близко к промаху, чтобы стирать чужую работу.
        RunAction(host, db, ProjectAction::Forget, m_selected);
    }
}

} // namespace Sage::Launcher
