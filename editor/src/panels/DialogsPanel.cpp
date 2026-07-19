#include "DialogsPanel.h"
#include "EditorHost.h"
#include "Project.h"

#include <imgui.h>
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

void DialogsPanel::Draw(EditorHost& host) {
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Name", m_projectName, sizeof(m_projectName));
        ImGui::InputText("Location", m_projectDir, sizeof(m_projectDir));
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
        ImGui::TextDisabled("Path to project.sageproj or the project folder");
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
        ImGui::TextDisabled("Path to a .sage scene file (tip: double-click one in Assets)");
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
