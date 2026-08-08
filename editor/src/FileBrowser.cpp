#include "FileBrowser.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "EditorIcons.h"
#include "sage/core/Paths.h"
#include "imgui.h"
#include "Localization.h"

namespace fs = std::filesystem;

namespace {

std::string HumanSize(uintmax_t bytes) {
    char buf[64];
    if (bytes < 1024) std::snprintf(buf, sizeof(buf), T("%llu B"), (unsigned long long)bytes);
    else if (bytes < 1024 * 1024) std::snprintf(buf, sizeof(buf), T("%.1f KiB"), bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024)
        std::snprintf(buf, sizeof(buf), T("%.1f MiB"), bytes / (1024.0 * 1024.0));
    else std::snprintf(buf, sizeof(buf), T("%.1f GiB"), bytes / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

std::string LowerOf(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Иконка по расширению — чтобы список читался глазами, а не построчно.
const char* IconFor(const fs::path& p, bool isDir) {
    if (isDir) return "folder";
    const std::string ext = LowerOf(p.extension().string());
    if (ext == ".sage") return "scene";
    if (ext == ".sageproj") return "project";
    if (ext == ".sagemat") return "material";
    if (ext == ".sageprefab") return "prefab";
    if (ext == ".lua") return "script";
    if (ext == ".obj" || ext == ".gltf" || ext == ".glb") return "model";
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" ||
        ext == ".hdr")
        return "texture";
    if (ext == ".vert" || ext == ".frag") return "shader";
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3") return "audio";
    return "file";
}

} // namespace

void FileBrowser::Open(const Config& config) {
    m_cfg = config;
    m_error.clear();
    m_selected = -1;
    m_search[0] = '\0';
    m_newFolder[0] = '\0';
    std::snprintf(m_name, sizeof(m_name), "%s", m_cfg.DefaultName.c_str());

    std::error_code ec;
    fs::path start = m_cfg.StartDir;
    if (start.empty() || !fs::is_directory(start, ec)) start = fs::current_path(ec);
    GoTo(start);

    // --- Быстрый доступ ------------------------------------------------------
    //
    // Список делится на ГРУППЫ, и это не украшение: «Документы» и «диск D:» —
    // разные по смыслу места, а вперемешку они превращаются в один длинный
    // столбец, по которому надо читать глазами.
    //
    // Папки пользователя спрашиваются у системы (см. sage::UserFolders): их
    // имена локализованы и переносимы, собрать их из «дом плюс Documents»
    // нельзя. Несуществующие не показываются вовсе — кнопка в никуда хуже
    // отсутствующей.
    m_places.clear();
    auto group = [&](const char* title) { m_places.push_back({title, {}, true}); };
    auto place = [&](const std::string& label, const fs::path& p) {
        m_places.push_back({label, p, false});
    };

    const std::vector<sage::UserFolder> userFolders = sage::UserFolders();
    if (!userFolders.empty()) {
        group(T("Folders"));
        for (const sage::UserFolder& f : userFolders) place(f.Label, f.Path);
    }

    // Места, относящиеся к работе: папка проекта и та, откуда запущен редактор.
    bool workGroup = false;
    auto workPlace = [&](const char* label, const fs::path& p) {
        if (p.empty() || !fs::is_directory(p, ec)) return;
        if (!workGroup) { group(T("Project")); workGroup = true; }
        place(label, p);
    };
    workPlace(T("Project assets"), m_cfg.StartDir);
    workPlace(T("Startup folder"), fs::current_path(ec));

    group(T("Drives"));
#ifdef _WIN32
    for (char drive = 'A'; drive <= 'Z'; ++drive) {
        const std::string root = std::string(1, drive) + ":\\";
        if (fs::exists(root, ec)) place(root, root);
    }
#else
    place(T("Root /"), "/");
    // Смонтированные носители: флешка и внешний диск — самые частые источники
    // чужих ассетов, и до них иначе идти вслепую через /media.
    for (const char* mountRoot : {"/media", "/mnt", "/run/media"}) {
        if (!fs::is_directory(mountRoot, ec)) continue;
        for (const fs::directory_entry& e : fs::directory_iterator(mountRoot, ec)) {
            if (!e.is_directory(ec)) continue;
            // /run/media/<пользователь>/<носитель> — на уровень глубже.
            bool leaf = true;
            for (const fs::directory_entry& sub : fs::directory_iterator(e.path(), ec)) {
                if (sub.is_directory(ec)) { place(sub.path().filename().string(), sub.path()); leaf = false; }
                break;
            }
            if (leaf) place(e.path().filename().string(), e.path());
        }
    }
#endif

    m_open = true;
    m_needsOpen = true;
}

void FileBrowser::GoTo(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    m_dir = fs::absolute(dir, ec).lexically_normal();
    m_selected = -1;
    Refresh();
}

bool FileBrowser::PassesFilter(const fs::path& p) const {
    if (m_cfg.Filters.empty()) return true;
    const std::string ext = LowerOf(p.extension().string());
    for (const std::string& f : m_cfg.Filters) {
        if (ext == LowerOf(f)) return true;
    }
    return false;
}

void FileBrowser::Refresh() {
    m_entries.clear();
    m_error.clear();
    std::error_code ec;
    fs::directory_iterator it(m_dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        m_error = T("The folder is not accessible: ") + ec.message();
        return;
    }
    for (const fs::directory_entry& e : it) {
        const bool isDir = e.is_directory(ec);
        const std::string name = e.path().filename().string();
        if (!m_showHidden && !name.empty() && name[0] == '.') continue;
        // В режиме выбора папки файлы не показываем вовсе: они там только шум.
        if (!isDir && m_cfg.Mode == PickMode::PickFolder) continue;
        if (!isDir && !PassesFilter(e.path())) continue;
        Entry entry;
        entry.Name = name;
        entry.IsDir = isDir;
        entry.Size = isDir ? 0 : (uintmax_t)e.file_size(ec);
        m_entries.push_back(std::move(entry));
    }
    // Папки первыми, дальше по алфавиту без учёта регистра — так список
    // предсказуем, а не зависит от порядка обхода файловой системы.
    std::sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) {
        if (a.IsDir != b.IsDir) return a.IsDir;
        return LowerOf(a.Name) < LowerOf(b.Name);
    });
}

void FileBrowser::DrawPlaces() {
    ImGui::BeginChild("##places", ImVec2(190, -ImGui::GetFrameHeightWithSpacing() * 2.2f), true);
    for (const Place& p : m_places) {
        if (p.IsGroup) {
            ImGui::Spacing();
            ImGui::TextDisabled("%s", p.Label.c_str());
            ImGui::Separator();
            continue;
        }
        // Текущее место подсвечено: в списке из полутора десятков папок иначе
        // не видно, где ты находишься.
        const bool here = !p.Path.empty() && p.Path == m_dir;
        if (here) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
        if (EditorIcons::Button("folder", p.Label.c_str())) GoTo(p.Path);
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.Path.string().c_str());
    }
    ImGui::EndChild();
}

void FileBrowser::DrawBreadcrumbs() {
    // Хлебные крошки кликабельны: подняться на три уровня — один клик, а не три
    // нажатия «вверх».
    fs::path acc;
    std::vector<fs::path> parts;
    for (const fs::path& part : m_dir) parts.push_back(part);
    for (size_t i = 0; i < parts.size(); ++i) {
        acc /= parts[i];
        std::string label = parts[i].string();
        if (label.empty() || label == "/") label = "/";
        ImGui::PushID((int)i);
        if (ImGui::SmallButton(label.c_str())) GoTo(acc);
        ImGui::PopID();
        if (i + 1 < parts.size()) {
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0.0f, 2.0f);
        }
    }
}

bool FileBrowser::Draw() {
    if (!m_open) return false;
    if (m_needsOpen) {
        ImGui::OpenPopup(m_cfg.Title.c_str());
        m_needsOpen = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(760, 480), ImGuiCond_Appearing);

    bool confirmed = false;
    bool stayOpen = true;
    if (ImGui::BeginPopupModal(m_cfg.Title.c_str(), &stayOpen)) {
        // --- Панель навигации ---
        if (EditorIcons::Button("up", T("Up"))) {
            if (m_dir.has_parent_path() && m_dir.parent_path() != m_dir) GoTo(m_dir.parent_path());
        }
        ImGui::SameLine();
        if (EditorIcons::Button("refresh", T("Refresh"))) Refresh();
        ImGui::SameLine();
        if (EditorIcons::Button("folder-plus", T("New Folder"))) ImGui::OpenPopup("##newfolder");
        ImGui::SameLine();
        ImGui::Checkbox(T("Hidden"), &m_showHidden);
        if (ImGui::IsItemDeactivatedAfterEdit()) Refresh();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        ImGui::InputTextWithHint("##search", T("Search..."), m_search, sizeof(m_search));

        if (ImGui::BeginPopup("##newfolder")) {
            ImGui::InputText(T("Name"), m_newFolder, sizeof(m_newFolder));
            if (ImGui::Button(T("Create")) && m_newFolder[0]) {
                std::error_code ec;
                if (fs::create_directory(m_dir / m_newFolder, ec)) {
                    Refresh();
                    m_newFolder[0] = '\0';
                    ImGui::CloseCurrentPopup();
                } else {
                    m_error = T("Could not create the folder: ") + ec.message();
                }
            }
            ImGui::EndPopup();
        }

        DrawBreadcrumbs();
        ImGui::Separator();

        DrawPlaces();
        ImGui::SameLine();

        // --- Список ---
        ImGui::BeginChild("##list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.2f), true);
        const std::string needle = LowerOf(m_search);
        for (int i = 0; i < (int)m_entries.size(); ++i) {
            const Entry& e = m_entries[i];
            if (!needle.empty() && LowerOf(e.Name).find(needle) == std::string::npos) continue;

            ImGui::PushID(i);
            EditorIcons::Inline(IconFor(e.Name, e.IsDir));
            ImGui::SameLine();
            const bool selected = (i == m_selected);
            if (ImGui::Selectable(e.Name.c_str(), selected,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                m_selected = i;
                if (!e.IsDir) std::snprintf(m_name, sizeof(m_name), "%s", e.Name.c_str());
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (e.IsDir) {
                        GoTo(m_dir / e.Name);
                        ImGui::PopID();
                        break;   // список пересобран — итерация по нему больше не валидна
                    } else if (m_cfg.Mode != PickMode::PickFolder) {
                        m_result = m_dir / e.Name;
                        confirmed = true;
                    }
                }
            }
            if (!e.IsDir) {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70.0f);
                ImGui::TextDisabled("%s", HumanSize(e.Size).c_str());
            }
            ImGui::PopID();
        }
        if (m_entries.empty()) ImGui::TextDisabled("%s", T("Empty"));
        ImGui::EndChild();

        // --- Имя и кнопки ---
        if (m_cfg.Mode != PickMode::PickFolder) {
            ImGui::SetNextItemWidth(-220);
            ImGui::InputText("##name", m_name, sizeof(m_name));
            ImGui::SameLine();
            if (!m_cfg.FilterLabel.empty()) ImGui::TextDisabled("%s", m_cfg.FilterLabel.c_str());
        } else {
            ImGui::TextDisabled(T("Will select: %s"), m_dir.string().c_str());
        }

        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());

        const char* okLabel = m_cfg.Mode == PickMode::SaveFile     ? T("Save")
                              : m_cfg.Mode == PickMode::PickFolder ? T("Choose folder")
                                                               : T("Open");
        if (ImGui::Button(okLabel, ImVec2(140, 0))) {
            if (m_cfg.Mode == PickMode::PickFolder) {
                m_result = m_dir;
                confirmed = true;
            } else if (m_name[0]) {
                fs::path candidate = m_dir / m_name;
                // В режиме сохранения дописываем расширение, если человек его не
                // ввёл: файл без расширения редактор потом не опознает, а
                // объяснять это диалогом — хуже, чем просто дописать.
                if (m_cfg.Mode == PickMode::SaveFile && !m_cfg.Filters.empty() &&
                    candidate.extension().empty()) {
                    candidate += m_cfg.Filters.front();
                }
                std::error_code ec;
                if (m_cfg.Mode == PickMode::OpenFile && !fs::exists(candidate, ec)) {
                    m_error = T("No such file: ") + candidate.string();
                } else {
                    m_result = candidate;
                    confirmed = true;
                }
            } else {
                m_error = T("Enter a file name");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancel"), ImVec2(140, 0))) {
            m_open = false;
            ImGui::CloseCurrentPopup();
        }

        if (confirmed) {
            m_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!stayOpen) m_open = false;
    return confirmed;
}
