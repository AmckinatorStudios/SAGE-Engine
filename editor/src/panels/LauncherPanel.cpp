#include "LauncherPanel.h"

#include <cstdio>
#include <filesystem>

#include "imgui.h"

#include "EditorHost.h"
#include "Project.h"
#include "RecentProjects.h"

namespace fs = std::filesystem;

void LauncherPanel::Draw(EditorHost& host, RecentProjects& recent) {
    if (m_newDir[0] == '\0') {
        std::snprintf(m_newDir, sizeof(m_newDir), "%s", fs::current_path().string().c_str());
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    ImGui::Begin("Welcome to SAGE", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking);

    ImGui::TextDisabled("Open a project to keep scenes, assets and materials together.");
    ImGui::Separator();

    // --- Недавние проекты ---
    ImGui::Text("Recent projects");
    if (recent.List().empty()) {
        ImGui::TextDisabled("(none yet)");
    }
    // Копия списка: открытие/удаление меняет recent прямо во время обхода.
    std::vector<std::string> items = recent.List();
    for (size_t i = 0; i < items.size(); ++i) {
        const std::string& path = items[i];
        ImGui::PushID((int)i);
        bool exists = fs::exists(fs::path(path) / "project.sageproj");
        if (!exists) ImGui::BeginDisabled();
        std::string label = fs::path(path).filename().string();
        if (ImGui::Button(label.c_str(), ImVec2(180, 0))) {
            std::string err;
            if (host.OpenProject(path, err)) {
                ImGui::PopID();
                ImGui::End();
                return;
            }
            m_error = err;
        }
        if (!exists) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s", path.c_str(), exists ? "" : "  (missing)");
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) recent.Remove(path);
        ImGui::PopID();
    }

    ImGui::Separator();

    // --- Создать новый ---
    ImGui::Text("Create new project");
    ImGui::InputText("Name", m_newName, sizeof(m_newName));
    ImGui::InputText("Location", m_newDir, sizeof(m_newDir));
    if (ImGui::Button("Create Project", ImVec2(180, 0))) {
        std::string err;
        if (host.CreateProject(m_newDir, m_newName, err)) {
            ImGui::End();
            return;
        }
        m_error = err;
    }

    ImGui::Separator();

    // --- Открыть по пути ---
    ImGui::Text("Open existing");
    ImGui::InputText("Path", m_openPath, sizeof(m_openPath));
    if (ImGui::Button("Open Project", ImVec2(180, 0))) {
        std::string err;
        if (host.OpenProject(m_openPath, err)) {
            ImGui::End();
            return;
        }
        m_error = err;
    }

    if (!m_error.empty()) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
    }

    ImGui::Separator();
    if (ImGui::SmallButton("Skip — work without a project")) m_dismissed = true;

    ImGui::End();
}
