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
#include "ProjectLauncherDetail.h"

namespace fs = std::filesystem;

namespace Sage::Launcher {

// Панели стартового окна: подробности проекта, шаблоны, настройки, диалоги.
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
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui.IconSize + EditorIcons::TextGap());
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
    // ПОДПИСЬ РИСУЕТСЯ ВМЕСТЕ СО ЗНАЧКОМ, а кнопка подаётся пустой.
    //
    // Здесь стояли пять пробелов перед словом и значок, положенный поверх них
    // «примерно в это место» с поправкой в полпробела. Ширина пробела зависит от
    // шрифта и перевода, поправка не зависела ни от чего — и пара то налезала
    // сама на себя, то расползалась. Теперь ширину пары считает та же функция,
    // что её рисует, и центрируется она целиком.
    const char* openLabel = T("Open Project");
    if (Sage::UI::Button("##open_project", Sage::UI::ButtonStyle::Primary,
                         ImVec2(width, ImGui::GetFrameHeight() * 1.6f))) {
        OpenProject(host, db, entry->Path);
    }
    const ImVec2 btnMin = ImGui::GetItemRectMin();
    const ImVec2 btnMax = ImGui::GetItemRectMax();
    const ImVec4 onAccent = EditorTheme::Color(Role::TextOnAccent);
    {
        const float line = ImGui::GetTextLineHeight();
        const float blockW = EditorIcons::LabeledWidth(line, openLabel);
        EditorIcons::DrawLabeled(ImGui::GetWindowDrawList(),
                                 ImVec2((btnMin.x + btnMax.x) * 0.5f - blockW * 0.5f,
                                        (btnMin.y + btnMax.y) * 0.5f - line * 0.5f),
                                 line, "play", ImGui::GetColorU32(onAccent), openLabel,
                                 ImGui::GetColorU32(onAccent));
    }
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
        // Кнопка ПУСТАЯ, а «значок + подпись» рисуются парой у левого края.
        //
        // Раньше подпись начиналась с пяти пробелов, а значок клали поверх них.
        // Кнопка во всю ширину центрирует свой текст, поэтому расстояние от
        // значка до буквы зависело от ДЛИНЫ СЛОВА: у «Удалить» одно, у
        // «Дублировать» другое, и колонка не читалась как колонка.
        const bool pressed = Sage::UI::Button("##quick", Sage::UI::ButtonStyle::Ghost,
                                              ImVec2(width, 0.0f));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec4 col = EditorTheme::Color(q.Danger ? Role::Danger : Role::TextDim);
        const ImVec4 textCol = EditorTheme::Color(q.Danger ? Role::Danger : Role::Text);
        EditorIcons::DrawLabeled(ImGui::GetWindowDrawList(),
                                 ImVec2(rowMin.x + ui.SpacingMD,
                                        rowMin.y + (ImGui::GetFrameHeight() - ui.IconSize) * 0.5f),
                                 ui.IconSize, q.Icon, ImGui::GetColorU32(col), T(q.Label),
                                 ImGui::GetColorU32(textCol));
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
                             T("Templates come with the editor."));
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
