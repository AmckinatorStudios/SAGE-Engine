#include "DialogsPanel.h"
#include "EditorHost.h"
#include "Project.h"

#include <imgui.h>

#include "EditorIcons.h"
#include <filesystem>

#include <cstdio>

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
    if (EditorIcons::Button("folder", "Обзор…")) Browse(cfg, buffer, size);
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
    if (ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Name", m_projectName, sizeof(m_projectName));
        ImGui::InputText("Location", m_projectDir, sizeof(m_projectDir));
        {
            FileBrowser::Config c;
            c.Title = "Куда создать проект";
            c.Mode = FileBrowser::PickMode::PickFolder;
            BrowseButton("newproj", c, m_projectDir, sizeof(m_projectDir));
        }
        ImGui::TextDisabled("Creates <Location>/<Name>/project.sageproj + scenes/ + assets/");
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (ImGui::Button("Create", ImVec2(120, 0))) {
            std::string err;
            if (host.CreateProject(m_projectDir, m_projectName, err)) {
                ImGui::CloseCurrentPopup();
            } else {
                m_error = err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Open Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Path", m_openPath, sizeof(m_openPath));
        {
            FileBrowser::Config c;
            c.Title = "Открыть проект";
            c.Mode = FileBrowser::PickMode::OpenFile;
            c.Filters = {".sageproj"};
            c.FilterLabel = "Проекты (*.sageproj)";
            BrowseButton("openproj", c, m_openPath, sizeof(m_openPath));
        }
        ImGui::SameLine();
        {
            FileBrowser::Config c;
            c.Title = "Открыть папку проекта";
            c.Mode = FileBrowser::PickMode::PickFolder;
            BrowseButton("openprojdir", c, m_openPath, sizeof(m_openPath));
        }
        ImGui::TextDisabled("Путь к project.sageproj или к папке проекта");
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (ImGui::Button("Open", ImVec2(120, 0))) {
            std::string err;
            if (host.OpenProject(m_openPath, err)) {
                ImGui::CloseCurrentPopup();
            } else {
                m_error = err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Save Scene As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("File name", m_sceneName, sizeof(m_sceneName));
        Project& project = host.CurrentProject();
        fs::path target = project.Loaded()
            ? project.ScenesDir() / (std::string(m_sceneName) + ".sage")
            : fs::path(std::string(m_sceneName) + ".sage");
        ImGui::TextDisabled("-> %s", target.string().c_str());
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            host.CurrentScene().SetName(m_sceneName);
            if (host.SaveSceneToFile(target)) ImGui::CloseCurrentPopup();
            else m_error = "Save failed (see Console)";
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Build Game", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        Project& project = host.CurrentProject();
        ImGui::TextDisabled("Packages SagePlayer + project '%s' into a runnable game",
                            project.Name().c_str());
        ImGui::InputText("Output dir", m_buildDir, sizeof(m_buildDir));
        {
            FileBrowser::Config c;
            c.Title = "Куда собрать игру";
            c.Mode = FileBrowser::PickMode::PickFolder;
            BrowseButton("builddir", c, m_buildDir, sizeof(m_buildDir));
        }
        ImGui::TextDisabled("-> %s/%s/%s", m_buildDir, project.Name().c_str(),
                            project.Name().c_str());
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (!m_buildResult.empty())
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "Built: %s", m_buildResult.c_str());
        if (ImGui::Button("Build", ImVec2(120, 0))) {
            std::string err;
            if (host.BuildGame(m_buildDir, err)) {
                m_error.clear();
                m_buildResult = (fs::path(m_buildDir) / project.Name()).string();
            } else {
                m_error = err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Open Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Path", m_openPath, sizeof(m_openPath));
        {
            FileBrowser::Config c;
            c.Title = "Открыть сцену";
            c.Filters = {".sage"};
            c.FilterLabel = "Сцены (*.sage)";
            if (host.CurrentProject().Loaded()) c.StartDir = host.CurrentProject().ScenesDir();
            BrowseButton("openscene", c, m_openPath, sizeof(m_openPath));
        }
        ImGui::TextDisabled("Путь к файлу .sage (или двойной клик в панели Assets)");
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (ImGui::Button("Open", ImVec2(120, 0))) {
            if (host.LoadSceneFromFile(m_openPath)) ImGui::CloseCurrentPopup();
            else m_error = "Load failed (see Console)";
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
