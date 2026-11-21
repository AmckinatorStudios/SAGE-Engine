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
#include "../ProjectTemplates.h"
#include "../ui/UI.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/core/Version.h"
#include "ProjectLauncherDetail.h"

namespace fs = std::filesystem;

namespace Sage::Launcher {

// Диалоги стартового окна.
// ---------------------------------------------------------------------------
//  Диалоги: создание, переименование, дубликат, удаление
// ---------------------------------------------------------------------------

void ProjectLauncher::DrawDialogs(EditorHost& host, ProjectDatabase& db) {
    if (m_dialog == Dialog::None) return;   // ни одного Begin не открыто — выход честный
    const Sage::UI::Style& ui = Sage::UI::Get();

    // Идентификатор окна ОДИН на все четыре диалога (часть после «###»):
    // открытым может быть только один, а подпись у каждого своя.
    const char* title = T("New Project");
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
        if (m_focusName) {
            ImGui::SetKeyboardFocusHere();
            m_focusName = false;
        }
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
            m_browseOpen = false;
        }
        // Итоговая папка — ПОКАЗЫВАЕМ: «место» и «имя» человек в уме не
        // складывает, а ошибиться папкой на один уровень — обычное дело.
        ImGui::Dummy(ImVec2(0.0f, ui.SpacingSM));
        Sage::UI::TextSecondary("%s", T("Destination:"));
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(Role::Info));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + fieldW);
        ImGui::TextWrapped("%s", (fs::path(m_dirBuf) / m_nameBuf).generic_string().c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

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
        if (Sage::UI::Button(T("Create"), Sage::UI::ButtonStyle::Primary)) {
            std::error_code ec;
            // Папку под проекты создаём сами: «Документы/SAGE Projects» при
            // первом запуске ещё нет.
            fs::create_directories(m_dirBuf, ec);
            std::string err;
            if (host.CreateProject(m_dirBuf, m_nameBuf, kNewProjectTemplate, err)) {
                const fs::path dir = fs::path(m_dirBuf) / m_nameBuf;
                db.Refresh(sage::PathToUtf8(dir));
                m_selected = sage::PathToUtf8(dir);
                m_error.clear();
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
