#include "panels/GameSettingsPanel.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "EditorHost.h"
#include "Localization.h"
#include "Project.h"

void GameSettingsPanel::Load(const std::filesystem::path& dir) {
    m_dir = dir;
    m_info = sage::assets::GameInfo{};
    if (!sage::assets::LoadGameInfo(dir, m_info)) m_info.Name.clear();
    m_status.clear();
}

void GameSettingsPanel::Save() {
    std::string error;
    if (sage::assets::SaveGameInfo(m_dir, m_info, &error)) {
        m_status = T("Saved to project.sageproj");
        m_statusError = false;
    } else {
        m_status = std::string(T("Not saved: ")) + error;
        m_statusError = true;
    }
}

void GameSettingsPanel::Draw(EditorHost& host, bool* open) {
    if (!ImGui::Begin((std::string(T("Game Settings")) + "###GameSettings").c_str(), open)) {
        ImGui::End();
        return;
    }
    Project& project = host.CurrentProject();
    if (!project.Loaded()) {
        ImGui::TextDisabled("%s", T("Open a project first."));
        ImGui::End();
        return;
    }
    if (m_dir != project.Dir()) Load(project.Dir());

    ImGui::TextWrapped("%s", T("How the built game presents itself: window title, file name, icon and file properties."));
    ImGui::Separator();

    // Поле пишет файл, когда правку закончили (Enter или уход из поля), а не на
    // каждую букву: файл проекта не переписывается сотни раз за слово.
    bool changed = false;
    auto field = [&](const char* label, const char* id, std::string& value, const char* hint) {
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint(id, hint, &value);
        changed |= ImGui::IsItemDeactivatedAfterEdit();
    };
    field(T("Game name"), "##gs_name", m_info.Name, T("Shown in the window title; the .exe gets this name"));
    field(T("Version"), "##gs_version", m_info.Version, "1.0.0");
    field(T("Author"), "##gs_author", m_info.Author, "");
    field(T("Company"), "##gs_company", m_info.Company, T("Publisher; empty — the author"));
    field(T("Copyright"), "##gs_copyright", m_info.Copyright, "");
    ImGui::TextUnformatted(T("Description"));
    ImGui::InputTextMultiline("##gs_description", &m_info.Description, ImVec2(-1, ImGui::GetTextLineHeight() * 4));
    changed |= ImGui::IsItemDeactivatedAfterEdit();

    ImGui::Spacing();
    ImGui::TextUnformatted(T("Icon"));
    ImGui::TextDisabled("%s", T("PNG, square, 256×256 or larger"));
    const assetslot::Result r = assetslot::Draw(host, "##gs_icon", assetslot::Kind::Texture, m_info.Icon,
                                                &m_preview, T("No icon"));
    if (r.Changed) { m_info.Icon = r.Path; changed = true; }
    if (r.Cleared) { m_info.Icon.clear(); changed = true; }
    if (r.BrowseRequested) {
        FileBrowser::Config c;
        c.Title = T("Game icon");
        c.Filters = {".png"};
        c.FilterLabel = T("Images");
        // Диалог заперт в проекте: иконка — ассет проекта (см. assetslot::ProjectRoot).
        c.StartDir = c.Root = assetslot::ProjectRoot(host);
        m_browser.Open(c);
    }
    if (m_browser.Draw()) {
        m_info.Icon = project.AssetRef(m_browser.Result());
        changed = true;
    }

    if (changed) Save();
    if (!m_status.empty()) {
        ImGui::Spacing();
        if (m_statusError) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", m_status.c_str());
        else ImGui::TextDisabled("%s", m_status.c_str());
    }
    ImGui::End();
}
