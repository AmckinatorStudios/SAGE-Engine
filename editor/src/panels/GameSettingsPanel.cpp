#include "panels/GameSettingsPanel.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "EditorHost.h"
#include "Localization.h"
#include "Project.h"

#include "sage/assets/UserContent.h"

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

    changed |= DrawUserFolders();

    if (changed) Save();
    if (!m_status.empty()) {
        ImGui::Spacing();
        if (m_statusError) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", m_status.c_str());
        else ImGui::TextDisabled("%s", m_status.c_str());
    }
    ImGui::End();
}

// Пользовательские папки: открытые папки для файлов игрока (скины, музыка,
// моды) — рядом с собранной игрой или в папке её данных. Возвращает «правили».
bool GameSettingsPanel::DrawUserFolders() {
    namespace uc = sage::assets::usercontent;
    bool changed = false;
    ImGui::Spacing();
    ImGui::SeparatorText(T("User folders"));
    ImGui::TextWrapped("%s", T("Open folders where players put their own files (skins, music, mods). "
                                "Scripts read them with user_folder.list(name). Example files: "
                                "userfolders/<name>/ in the project."));
    int removeAt = -1;
    for (size_t i = 0; i < m_info.UserFolders.size(); ++i) {
        sage::assets::UserFolderSpec& f = m_info.UserFolders[i];
        ImGui::PushID((int)i);
        const std::string header = (f.Name.empty() ? std::string(T("(no name)")) : f.Name) + "###uf";
        if (ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextUnformatted(T("Folder name"));
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##uf_name", T("skins"), &f.Name);
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            if (!f.Name.empty() && !uc::ValidName(f.Name))
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", T("Latin letters, digits, - and _ only"));
            ImGui::TextUnformatted(T("Title"));
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##uf_title", T("Shown to the player"), &f.Title);
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            ImGui::TextUnformatted(T("What to put here"));
            ImGui::InputTextMultiline("##uf_desc", &f.Description, ImVec2(-1, ImGui::GetTextLineHeight() * 3));
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            // Расширения — одной строкой через запятую: «.png, .jpg».
            std::string ext;
            for (size_t k = 0; k < f.Extensions.size(); ++k) ext += (k ? ", " : "") + f.Extensions[k];
            ImGui::TextUnformatted(T("File types"));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputTextWithHint("##uf_ext", T(".png, .ogg — empty: any"), &ext)) {
                f.Extensions.clear();
                std::string cur;
                for (char c : ext + ",") {
                    if (c == ',' || c == ' ') {
                        if (!cur.empty()) f.Extensions.push_back(cur[0] == '.' ? cur : "." + cur);
                        cur.clear();
                    } else {
                        cur += c;
                    }
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            ImGui::TextUnformatted(T("Where it lives"));
            int loc = f.NextToGame ? 0 : 1;
            const char* locs[] = {T("Next to the game"), T("Player's game data folder")};
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##uf_loc", &loc, locs, 2)) { f.NextToGame = loc == 0; changed = true; }
            if (ImGui::Button(T("Open folder"))) {
                // Папка Play-режима (данные игры этого пользователя).
                uc::Configure(m_info.UserFolders, {}, m_dir);
                uc::OpenInSystem(f.Name);
            }
            ImGui::SameLine();
            if (ImGui::Button(T("Remove folder"))) removeAt = (int)i;
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (removeAt >= 0) {
        m_info.UserFolders.erase(m_info.UserFolders.begin() + removeAt);
        changed = true;
    }
    if (ImGui::Button(T("Add folder"))) {
        sage::assets::UserFolderSpec f;
        f.Name = "folder" + std::to_string(m_info.UserFolders.size() + 1);
        m_info.UserFolders.push_back(f);
        changed = true;
    }
    // Play-режим видит правку сразу, без переоткрытия проекта.
    if (changed) uc::Configure(m_info.UserFolders, {}, m_dir);
    return changed;
}
