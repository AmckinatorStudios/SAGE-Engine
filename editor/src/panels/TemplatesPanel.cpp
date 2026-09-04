#include "TemplatesPanel.h"

#include <cfloat>
#include <cstring>

#include <imgui.h>

#include "../EditorHost.h"
#include "../EditorTheme.h"
#include "../Localization.h"
#include "../Project.h"
#include "../ProjectTemplates.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"

namespace tpl = sage::editor::templates;
using sage::editor::T;

namespace {

// Размер загрузки словами. «12 МБ» человек соотносит со своим интернетом,
// «12582912» — нет.
std::string HumanSize(uint64_t bytes) {
    char buf[64];
    if (bytes >= 1024ull * 1024ull)
        std::snprintf(buf, sizeof(buf), "%.1f %s", bytes / (1024.0 * 1024.0), T("MB"));
    else if (bytes >= 1024ull)
        std::snprintf(buf, sizeof(buf), "%.0f %s", bytes / 1024.0, T("KB"));
    else
        std::snprintf(buf, sizeof(buf), "%llu %s", (unsigned long long)bytes, T("bytes"));
    return buf;
}

} // namespace

void TemplatesPanel::Refresh() {
    m_installed = tpl::Installed();
    RefreshProjectTemplates();   // список в окне создания проекта — тот же самый
    m_loaded = true;
}

void TemplatesPanel::Say(EditorHost& host, const std::string& message, bool bad) {
    m_status = message;
    m_statusBad = bad;
    host.SetStatusMessage(message);
    if (bad) LOG_ERROR("Editor") << message;
    else LOG_INFO("Editor") << message;
}

void TemplatesPanel::Draw(EditorHost& host, bool& open) {
    if (!open) {
        m_loaded = false;   // при следующем открытии перечитаем диск
        return;
    }
    if (!m_loaded) {
        Refresh();
        const std::string url = tpl::CatalogUrl();
        std::snprintf(m_catalogUrl, sizeof(m_catalogUrl), "%s", url.c_str());
        m_urlEdited = false;
    }

    ImGui::SetNextWindowSize(ImVec2(720, 560), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(560, 380), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin(T("Project templates" "###Project templates"), &open)) {
        ImGui::End();
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::TextDim));
    ImGui::TextWrapped("%s", T("Ready-made projects are content, not part of the editor: they are "
                               "installed separately and can be removed. Built-in templates (empty, "
                               "demo, menu) are built by code and are always available."));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    if (!m_status.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              EditorTheme::Color(m_statusBad ? EditorTheme::Role::Danger
                                                             : EditorTheme::Role::Ok));
        ImGui::TextWrapped("%s", m_status.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    if (ImGui::BeginTabBar("##templates")) {
        if (ImGui::BeginTabItem(T("Installed"))) {
            DrawInstalled(host);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("Download"))) {
            DrawCatalog(host);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(T("From disk"))) {
            DrawSources(host);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // Диалог выбора файла/папки — общий на все три источника: он один и тот же,
    // отличается только тем, что делать с выбранным.
    if (m_browser.Draw()) {
        const std::filesystem::path picked = m_browser.Result();
        std::string err;
        switch (m_pending) {
            case Pending::InstallFile:
                if (tpl::InstallFromFile(picked, err)) {
                    Refresh();
                    Say(host, std::string(T("Template installed:")) + " " +
                                  sage::PathToUtf8(picked.filename()), false);
                } else {
                    Say(host, err, true);
                }
                break;
            case Pending::InstallFolder: {
                tpl::Manifest m;
                m.Id = sage::PathToUtf8(picked.filename());
                m.Name = m.Id;
                if (tpl::InstallFromFolder(picked, m, err)) {
                    Refresh();
                    Say(host, std::string(T("Template installed:")) + " " + m.Name, false);
                } else {
                    Say(host, err, true);
                }
                break;
            }
            case Pending::SaveAs: {
                tpl::Manifest m;
                m.Id = m_saveId[0] ? m_saveId : host.CurrentProject().Name();
                m.Name = m_saveName[0] ? m_saveName : m.Id;
                if (tpl::Pack(host.CurrentProject().Dir(), m, picked, err))
                    Say(host, std::string(T("Template written:")) + " " + sage::PathToUtf8(picked),
                        false);
                else
                    Say(host, err, true);
                break;
            }
            case Pending::None:
                break;
        }
        m_pending = Pending::None;
    }

    ImGui::End();
}

void TemplatesPanel::DrawInstalled(EditorHost& host) {
    ImGui::Spacing();
    if (m_installed.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::TextDim));
        ImGui::TextWrapped("%s", T("No ready-made projects installed yet. Get one on the "
                                   "«Download» tab, or install a .sagetemplate file from disk."));
        ImGui::PopStyleColor();
        return;
    }

    for (const tpl::Manifest& m : m_installed) {
        ImGui::PushID(m.Id.c_str());
        ImGui::Separator();
        ImGui::TextUnformatted(m.Name.c_str());
        if (!m.Version.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", m.Version.c_str());
        }
        if (!m.Summary.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::TextDim));
            ImGui::TextWrapped("%s", m.Summary.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::TextDisabled("%s", sage::PathToUtf8(tpl::Root() / m.Id).c_str());
        if (EditorTheme::ColoredButton(T("Remove"), EditorTheme::Role::Danger)) {
            std::string err;
            if (tpl::Uninstall(m.Id, err)) {
                Say(host, std::string(T("Template removed:")) + " " + m.Name, false);
                Refresh();
                ImGui::PopID();
                break;   // список изменился под ногами — обход прерываем
            }
            Say(host, err, true);
        }
        ImGui::PopID();
    }
}

void TemplatesPanel::DrawCatalog(EditorHost& host) {
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("Catalog:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-140.0f);
    if (ImGui::InputText("##catalogurl", m_catalogUrl, sizeof(m_catalogUrl))) m_urlEdited = true;
    ImGui::SameLine();
    if (ImGui::Button(T("Refresh"))) {
        if (m_urlEdited) {
            tpl::SetCatalogUrl(m_catalogUrl);
            m_urlEdited = false;
        }
        std::string err;
        m_catalogLoaded = tpl::FetchCatalog(m_catalog, err);
        if (!m_catalogLoaded) Say(host, err, true);
        else Say(host, std::string(T("Catalog loaded, templates:")) + " " +
                           std::to_string(m_catalog.size()), false);
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Default"))) {
        std::snprintf(m_catalogUrl, sizeof(m_catalogUrl), "%s", tpl::DefaultCatalogUrl().c_str());
        m_urlEdited = true;
    }

    if (!tpl::NetworkAvailable()) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Warn));
        ImGui::TextWrapped("%s", T("curl not found — downloading is unavailable. Templates can "
                                   "still be installed from a .sagetemplate file on the «From "
                                   "disk» tab."));
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    if (!m_catalogLoaded) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::TextDim));
        ImGui::TextWrapped("%s", T("Press «Refresh» to fetch the list. Nothing is downloaded "
                                   "until you ask for it."));
        ImGui::PopStyleColor();
        return;
    }

    for (const tpl::Manifest& m : m_catalog) {
        ImGui::PushID(m.Id.c_str());
        ImGui::Separator();
        ImGui::TextUnformatted(m.Name.c_str());
        if (!m.Version.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", m.Version.c_str());
        }
        if (m.Bytes) {
            ImGui::SameLine();
            ImGui::TextDisabled("· %s", HumanSize(m.Bytes).c_str());
        }
        if (!m.Summary.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::TextDim));
            ImGui::TextWrapped("%s", m.Summary.c_str());
            ImGui::PopStyleColor();
        }
        if (m.Installed) {
            ImGui::TextDisabled("%s", T("Already installed"));
            ImGui::SameLine();
            if (ImGui::Button(T("Reinstall"))) {
                std::string err;
                if (tpl::Download(m, err)) {
                    Refresh();
                    Say(host, std::string(T("Template installed:")) + " " + m.Name, false);
                } else {
                    Say(host, err, true);
                }
            }
        } else if (ImGui::Button(T("Download and install"))) {
            std::string err;
            if (tpl::Download(m, err)) {
                Refresh();
                Say(host, std::string(T("Template installed:")) + " " + m.Name, false);
            } else {
                Say(host, err, true);
            }
        }
        ImGui::PopID();
    }
}

void TemplatesPanel::DrawSources(EditorHost& host) {
    ImGui::Spacing();
    ImGui::SeparatorText(T("Install"));
    if (ImGui::Button(T("Install .sagetemplate file..."))) {
        FileBrowser::Config cfg;
        cfg.Title = T("Choose a template file");
        cfg.Mode = FileBrowser::PickMode::OpenFile;
        cfg.Filters = {".sagetemplate"};
        cfg.FilterLabel = T("SAGE templates (*.sagetemplate)");
        m_browser.Open(cfg);
        m_pending = Pending::InstallFile;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Install from a project folder..."))) {
        FileBrowser::Config cfg;
        cfg.Title = T("Choose a project folder");
        cfg.Mode = FileBrowser::PickMode::PickFolder;
        m_browser.Open(cfg);
        m_pending = Pending::InstallFolder;
    }

    ImGui::Spacing();
    ImGui::SeparatorText(T("Share the current project"));
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::TextDim));
    ImGui::TextWrapped("%s", T("Packs the open project into one .sagetemplate file — the same "
                               "kind of file the editor installs. Editor sidecars (.meta) are "
                               "left out: they describe a local import and are rebuilt anyway."));
    ImGui::PopStyleColor();

    if (m_saveId[0] == '\0')
        std::snprintf(m_saveId, sizeof(m_saveId), "%s", host.CurrentProject().Name().c_str());
    if (m_saveName[0] == '\0')
        std::snprintf(m_saveName, sizeof(m_saveName), "%s", host.CurrentProject().Name().c_str());

    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText(T("Id (folder name)"), m_saveId, sizeof(m_saveId));
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText(T("Name in the list"), m_saveName, sizeof(m_saveName));

    if (ImGui::Button(T("Save project as template..."))) {
        FileBrowser::Config cfg;
        cfg.Title = T("Where to write the template");
        cfg.Mode = FileBrowser::PickMode::SaveFile;
        cfg.DefaultName = std::string(m_saveId) + ".sagetemplate";
        cfg.Filters = {".sagetemplate"};
        cfg.FilterLabel = T("SAGE templates (*.sagetemplate)");
        m_browser.Open(cfg);
        m_pending = Pending::SaveAs;
    }

    ImGui::Spacing();
    ImGui::SeparatorText(T("Where templates live"));
    ImGui::TextDisabled("%s", sage::PathToUtf8(tpl::Root()).c_str());
}
