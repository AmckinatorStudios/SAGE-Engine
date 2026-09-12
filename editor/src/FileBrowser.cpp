#include "FileBrowser.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "AssetSlot.h"
#include "EditorIcons.h"
#include "EditorPrefs.h"
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

// Размер плитки в сетке. 96 — тот же размер обложки, что в панели ассетов: одна
// и та же картинка не должна выглядеть в двух местах по-разному, да и кэш
// обложек у них общий (assetslot::Cover), так что совпадение размеров — это ещё
// и одна съёмка вместо двух.
constexpr float kTileW = 104.0f;
constexpr float kCoverH = 80.0f;
constexpr float kTileH = kCoverH + 26.0f;
constexpr float kTileGap = 8.0f;

// Обрезает имя многоточием справа, чтобы уместиться в ширину плитки.
std::string TruncateToWidth(const std::string& s, float maxWidth) {
    if (ImGui::CalcTextSize(s.c_str()).x <= maxWidth) return s;
    const float dots = ImGui::CalcTextSize("...").x;
    std::string out;
    for (size_t n = 1; n <= s.size(); ++n) {
        const std::string candidate = s.substr(0, n);
        if (ImGui::CalcTextSize(candidate.c_str()).x + dots > maxWidth) break;
        out = candidate;
    }
    if (out.empty()) out = s.substr(0, 1);
    return out + "...";
}

// Шахматка под картинкой с прозрачностью: без неё дырки в альфе неотличимы от
// фона, и текстура выглядит просто рваной.
void DrawChecker(ImDrawList* dl, const ImVec2& a, const ImVec2& b) {
    const float step = 8.0f;
    dl->PushClipRect(a, b, true);
    for (float y = a.y; y < b.y; y += step) {
        for (float x = a.x; x < b.x; x += step) {
            const bool odd = ((int)((x - a.x) / step) + (int)((y - a.y) / step)) % 2;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + step, y + step),
                              odd ? IM_COL32(68, 68, 74, 255) : IM_COL32(50, 50, 56, 255));
        }
    }
    dl->PopClipRect();
}

} // namespace

// Один ответ на щелчок для обоих видов. Раньше такого разбора не было вовсе —
// вид был один; с появлением второго «двойной клик открывает папку» обязано
// остаться ОДНОЙ строкой кода, иначе сетка и список разойдутся в поведении при
// первой же правке.
FileBrowser::Hit FileBrowser::DrawEntryCommon(int index, const Entry& entry, bool doubleClicked) {
    m_selected = index;
    if (!entry.IsDir) std::snprintf(m_name, sizeof(m_name), "%s", entry.Name.c_str());
    if (!doubleClicked) return Hit::Selected;
    if (entry.IsDir) return Hit::EnterDir;
    if (m_cfg.Mode == PickMode::PickFolder) return Hit::Selected;
    m_result = m_dir / entry.Name;
    return Hit::Confirm;
}

void FileBrowser::Open(const Config& config) {
    // Выбранный вид читается ОДИН раз за сеанс: файл настроек лежит на диске, а
    // диалог за сеанс открывают десятки раз. Именно здесь, а не в Draw(): вид
    // обязан быть известен до первого кадра окна, иначе диалог мигал бы строками
    // и только потом становился сеткой.
    if (!m_viewLoaded) {
        m_grid = sage::editor::prefs::GetBool("filebrowser.grid", false);
        m_viewLoaded = true;
    }
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

// --- СТРОКИ -----------------------------------------------------------------
//
// Вид для «я знаю, как называется файл»: одна строка на файл, видно имя целиком
// и размер. Обложка маленькая, вместо прежнего голого значка типа: даже 18
// пикселей отвечают на вопрос «это та картинка или соседняя», на который значок
// «texture» не отвечает никогда.
bool FileBrowser::DrawList() {
    bool confirmed = false;
    const std::string needle = LowerOf(m_search);
    const float icon = ImGui::GetTextLineHeight() + 4.0f;
    for (int i = 0; i < (int)m_entries.size(); ++i) {
        const Entry& e = m_entries[i];
        if (!needle.empty() && LowerOf(e.Name).find(needle) == std::string::npos) continue;

        ImGui::PushID(i);
        const fs::path full = m_dir / e.Name;
        const uint64_t cover = e.IsDir ? 0 : assetslot::Cover(m_preview, full, (int)kCoverH);
        if (cover) {
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const ImVec2 p1(p0.x + icon, p0.y + icon);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            DrawChecker(dl, p0, p1);
            dl->AddImageRounded((ImTextureID)(std::intptr_t)cover, p0, p1, ImVec2(0, 1),
                                ImVec2(1, 0), IM_COL32_WHITE, 3.0f);
            ImGui::Dummy(ImVec2(icon, icon));
        } else {
            EditorIcons::Inline(IconFor(e.Name, e.IsDir));
        }
        ImGui::SameLine();
        if (ImGui::Selectable(e.Name.c_str(), i == m_selected,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            const Hit hit = DrawEntryCommon(i, e, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
            if (hit == Hit::EnterDir) {
                GoTo(full);
                ImGui::PopID();
                break;   // список пересобран — итерация по нему больше не валидна
            }
            if (hit == Hit::Confirm) confirmed = true;
        }
        if (!e.IsDir) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70.0f);
            ImGui::TextDisabled("%s", HumanSize(e.Size).c_str());
        }
        ImGui::PopID();
    }
    return confirmed;
}

// --- СЕТКА ------------------------------------------------------------------
//
// Вид для «мне нужна вон та картинка». Имена у скачанных наборов не значат
// ничего (sky_04.png, T_Rock_02_D.png), и строка с таким именем и размером —
// это загадка, которую человек решал открыванием файлов по одному. Обложка
// отвечает на неё сразу.
bool FileBrowser::DrawGrid() {
    bool confirmed = false;
    const std::string needle = LowerOf(m_search);
    const float avail = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, (int)((avail + kTileGap) / (kTileW + kTileGap)));
    int col = 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (int i = 0; i < (int)m_entries.size(); ++i) {
        const Entry& e = m_entries[i];
        if (!needle.empty() && LowerOf(e.Name).find(needle) == std::string::npos) continue;

        if (col > 0) ImGui::SameLine(0.0f, kTileGap);
        ImGui::PushID(i);
        const fs::path full = m_dir / e.Name;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1(p0.x + kTileW, p0.y + kTileH);

        // Вся плитка — ОДИН элемент: подпись внутри его границ, и строки сетки
        // не налезают друг на друга, а попасть по плитке можно мимо картинки.
        ImGui::InvisibleButton("##tile", ImVec2(kTileW, kTileH));
        const bool hovered = ImGui::IsItemHovered();
        const bool doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        if (hovered) ImGui::SetTooltip("%s", e.Name.c_str());

        ImU32 bg = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.035f));
        if (i == m_selected) bg = ImGui::GetColorU32(ImGuiCol_Header);
        else if (hovered)    bg = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
        dl->AddRectFilled(p0, p1, bg, 6.0f);

        const ImVec2 c0(p0.x + 4.0f, p0.y + 4.0f);
        const ImVec2 c1(p1.x - 4.0f, p0.y + kCoverH);
        dl->AddRectFilled(c0, c1, ImGui::GetColorU32(ImVec4(0, 0, 0, 0.28f)), 5.0f);

        const uint64_t cover = e.IsDir ? 0 : assetslot::Cover(m_preview, full, (int)kCoverH);
        if (cover) {
            DrawChecker(dl, c0, c1);
            dl->AddImageRounded((ImTextureID)(std::intptr_t)cover, c0, c1, ImVec2(0, 1),
                                ImVec2(1, 0), IM_COL32_WHITE, 5.0f);
        } else {
            // Обложки нет (папка, звук, текст, ещё не снятая модель) — значок
            // типа во всю площадку. Пустая площадка читалась бы как «файл битый».
            const float glyph = 34.0f;
            EditorIcons::Overlay(c0.x + (c1.x - c0.x - glyph) * 0.5f,
                                 c0.y + (c1.y - c0.y - glyph) * 0.5f, glyph,
                                 e.IsDir ? "folder" : IconFor(e.Name, e.IsDir),
                                 glm::vec3(0.72f, 0.74f, 0.78f));
        }

        // Имя обрезается многоточием: под плиткой одна строка, и длинное имя
        // иначе уехало бы на соседнюю плитку.
        const std::string label = TruncateToWidth(e.Name, kTileW - 10.0f);
        const ImVec2 size = ImGui::CalcTextSize(label.c_str());
        dl->AddText(ImVec2(p0.x + (kTileW - size.x) * 0.5f, p0.y + kCoverH + 4.0f),
                    ImGui::GetColorU32(ImGuiCol_Text), label.c_str());

        if (clicked || doubleClicked) {
            const Hit hit = DrawEntryCommon(i, e, doubleClicked);
            if (hit == Hit::EnterDir) {
                GoTo(full);
                ImGui::PopID();
                break;   // список пересобран
            }
            if (hit == Hit::Confirm) confirmed = true;
        }
        ImGui::PopID();
        col = (col + 1) % columns;
    }
    return confirmed;
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
        // Вид — ОДНА кнопка-переключатель, а не две радиокнопки: состояний два,
        // и значок на ней показывает тот вид, в который она переключит.
        if (EditorIcons::Button(m_grid ? "list" : "grid",
                                m_grid ? T("Rows: name and size") : T("Grid: covers"))) {
            m_grid = !m_grid;
            sage::editor::prefs::SetBool("filebrowser.grid", m_grid);
        }
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

        // --- Список или сетка ---
        ImGui::BeginChild("##list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.2f), true);
        confirmed = m_grid ? DrawGrid() : DrawList();
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
