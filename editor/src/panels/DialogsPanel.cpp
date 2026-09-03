#include "DialogsPanel.h"

#include "../ProjectTemplates.h"
#include "../ProjectTemplateCover.h"
#include "EditorHost.h"
#include "Project.h"

#include <imgui.h>

#include "EditorIcons.h"
#include <filesystem>

#include <cstdio>
#include "../Localization.h"

namespace fs = std::filesystem;

DialogsPanel::DialogsPanel() {
    // Дефолтная папка диалогов — рядом с бинарником; сборка игр — в dist/.
    std::snprintf(m_projectDir, sizeof(m_projectDir), "%s", fs::current_path().string().c_str());
    std::snprintf(m_buildDir, sizeof(m_buildDir), "%s",
                  (fs::current_path() / "dist").string().c_str());
}

void DialogsPanel::Open(const char* name) {
    m_error.clear();
    // Build Game показывает путь прошлой успешной сборки — сбрасываем при повторном открытии.
    if (std::string(name) == "Build Game") m_buildResult.clear();
    ImGui::OpenPopup(name);
}

void DialogsPanel::Browse(const FileBrowser::Config& cfg, char* buffer, size_t size) {
    m_browseTarget = buffer;
    m_browseTargetSize = size;
    FileBrowser::Config c = cfg;
    // Начинаем с того, что уже введено: если человек правит путь, обзор обязан
    // открыться ТАМ ЖЕ, а не в домашней папке — иначе он каждый раз идёт весь
    // путь заново.
    if (buffer[0]) {
        std::error_code ec;
        fs::path typed(buffer);
        if (fs::is_directory(typed, ec)) c.StartDir = typed;
        else if (typed.has_parent_path() && fs::is_directory(typed.parent_path(), ec))
            c.StartDir = typed.parent_path();
    }
    m_browser.Open(c);
}

void DialogsPanel::BrowseButton(const char* id, const FileBrowser::Config& cfg, char* buffer,
                                size_t size) {
    ImGui::SameLine();
    ImGui::PushID(id);
    if (EditorIcons::Button("folder", T("Browse..."))) Browse(cfg, buffer, size);
    ImGui::PopID();
}

void DialogsPanel::Draw(EditorHost& host) {
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();

    // Результат обзора кладём в буфер того поля, ради которого его открыли.
    if (m_browser.Draw() && m_browseTarget) {
        std::snprintf(m_browseTarget, m_browseTargetSize, "%s",
                      m_browser.Result().string().c_str());
        m_browseTarget = nullptr;
        m_error.clear();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(T("New Project" "###New Project"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText(T("Name"), m_projectName, sizeof(m_projectName));
        ImGui::InputText(T("Location"), m_projectDir, sizeof(m_projectDir));
        {
            FileBrowser::Config c;
            c.Title = T("Where to create the project");
            c.Mode = FileBrowser::PickMode::PickFolder;
            BrowseButton("newproj", c, m_projectDir, sizeof(m_projectDir));
        }
        ImGui::TextDisabled("%s", T("Creates <Location>/<Name>/project.sageproj + scenes/ + assets/"));

        // Шаблон — С ЧЕГО начинается проект. Пока выбора не было, каждый новый
        // проект начинался с девяти демо-объектов, которые первым делом
        // удаляли.
        ImGui::SeparatorText(T("Start from"));
        // Карточками с обложкой — тем же виджетом, что и в стартовом окне.
        // Двух похожих списков шаблонов в редакторе быть не должно: они
        // разъедутся ровно так же, как разъезжались подписи и порядок.
        for (size_t i = 0; i < ProjectTemplates().size(); ++i) {
            const ProjectTemplate& tpl = ProjectTemplates()[i];
            if (i > 0) ImGui::SameLine();
            if (ProjectTemplateCard(tpl, m_templateId == tpl.Id, 190.0f)) m_templateId = tpl.Id;
        }

        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (ImGui::Button(T("Create"), ImVec2(120, 0))) {
            std::string err;
            if (host.CreateProject(m_projectDir, m_projectName, m_templateId, err)) {
                ImGui::CloseCurrentPopup();
            } else {
                m_error = err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(T("Open Project" "###Open Project"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText(T("Path"), m_openPath, sizeof(m_openPath));
        {
            FileBrowser::Config c;
            c.Title = T("Open project");
            c.Mode = FileBrowser::PickMode::OpenFile;
            c.Filters = {".sageproj"};
            c.FilterLabel = T("Projects (*.sageproj)");
            BrowseButton("openproj", c, m_openPath, sizeof(m_openPath));
        }
        ImGui::SameLine();
        {
            FileBrowser::Config c;
            c.Title = T("Open the project folder");
            c.Mode = FileBrowser::PickMode::PickFolder;
            BrowseButton("openprojdir", c, m_openPath, sizeof(m_openPath));
        }
        ImGui::TextDisabled("%s", T("Path to project.sageproj or to a project folder"));
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (ImGui::Button(T("Open"), ImVec2(120, 0))) {
            std::string err;
            if (host.OpenProject(m_openPath, err)) {
                ImGui::CloseCurrentPopup();
            } else {
                m_error = err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(T("Save Scene As" "###Save Scene As"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText(T("File name"), m_sceneName, sizeof(m_sceneName));
        fs::path target =
            host.CurrentProject().ScenesDir() / (std::string(m_sceneName) + ".sage");
        ImGui::TextDisabled("-> %s", target.string().c_str());
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (ImGui::Button(T("Save"), ImVec2(120, 0))) {
            host.CurrentScene().SetName(m_sceneName);
            if (host.SaveSceneToFile(target)) ImGui::CloseCurrentPopup();
            else m_error = "Save failed (see Console)";
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(T("Build Game" "###Build Game"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        Project& project = host.CurrentProject();
        ImGui::TextDisabled(T("Packages SagePlayer + project '%s' into a runnable game"),
                            project.Name().c_str());
        ImGui::InputText(T("Output dir"), m_buildDir, sizeof(m_buildDir));
        {
            FileBrowser::Config c;
            c.Title = T("Where to build the game");
            c.Mode = FileBrowser::PickMode::PickFolder;
            BrowseButton("builddir", c, m_buildDir, sizeof(m_buildDir));
        }
        ImGui::TextDisabled("-> %s/%s/%s", m_buildDir, project.Name().c_str(),
                            project.Name().c_str());
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (!m_buildResult.empty())
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), T("Built: %s"), m_buildResult.c_str());
        if (ImGui::Button(T("Build"), ImVec2(120, 0))) {
            std::string err;
            if (host.BuildGame(m_buildDir, err)) {
                m_error.clear();
                m_buildResult = (fs::path(m_buildDir) / project.Name()).string();
            } else {
                m_error = err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Close"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(T("Open Scene" "###Open Scene"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText(T("Path"), m_openPath, sizeof(m_openPath));
        {
            FileBrowser::Config c;
            c.Title = T("Open scene");
            c.Filters = {".sage"};
            c.FilterLabel = T("Scenes (*.sage)");
            c.StartDir = host.CurrentProject().ScenesDir();
            BrowseButton("openscene", c, m_openPath, sizeof(m_openPath));
        }
        ImGui::TextDisabled("%s", T("Path to a .sage file (or double-click in the Assets panel)"));
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (ImGui::Button(T("Open"), ImVec2(120, 0))) {
            if (host.LoadSceneFromFile(m_openPath)) ImGui::CloseCurrentPopup();
            else m_error = "Load failed (see Console)";
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
