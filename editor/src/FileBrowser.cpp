#include "FileBrowser.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "AssetSlot.h"
#include "EditorIcons.h"
#include "EditorPrefs.h"
#include "PathList.h"
#include "sage/core/Paths.h"
#include "PathScope.h"
#include "Thumbnails.h"
#include "ui/UI.h"
#include "sage/core/Paths.h"
#include "imgui.h"
#include "AssetCovers.h"
#include "Localization.h"

namespace fs = std::filesystem;
namespace pathlist = sage::editor::pathlist;
namespace covers = sage::editor::covers;

namespace {

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

} // namespace

// Один ответ на щелчок для обоих видов. Раньше такого разбора не было вовсе —
// вид был один; с появлением второго «двойной клик открывает папку» обязано
// остаться ОДНОЙ строкой кода, иначе сетка и список разойдутся в поведении при
// первой же правке.
FileBrowser::Hit FileBrowser::DrawEntryCommon(int index, const Entry& entry, bool doubleClicked) {
    m_selected = index;
    // В режиме «файл или папка» имя папки тоже попадает в поле: щелчок по ней
    // означает «вот это и внести», а двойной по-прежнему заходит внутрь.
    if (!entry.IsDir || m_cfg.Mode == PickMode::OpenAny)
        std::snprintf(m_name, sizeof(m_name), "%s", entry.Name.c_str());
    if (!doubleClicked) return Hit::Selected;
    if (entry.IsDir) return Hit::EnterDir;
    if (m_cfg.Mode == PickMode::PickFolder) return Hit::Selected;
    m_result = m_dir / entry.Name;
    return Hit::Confirm;
}

// Обложка файла в заданной площадке. Одна на оба вида: строка рисует её 18
// пикселями, плитка — восемьюдесятью, и разойтись в том, что считается
// обложкой, они не могут.
FileBrowser::Cover FileBrowser::DrawCover(const fs::path& full, bool isDir, float x0, float y0,
                                          float x1, float y1, float rounding) {
    if (isDir) return Cover::None;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(x0, y0), b(x1, y1);

    // Картинки идут через свой кэш обложек: уменьшенными, с мипмапами и в
    // фоновом потоке. Почему не через ResourceManager — в Thumbnails.h.
    if (thumbs::IsImage(full)) {
        const thumbs::Thumb t = thumbs::Get(full, thumbs::Size::Tile);
        if (t.Id) {
            const covers::FitRect r = covers::Fit(a, b, t.W, t.H);
            covers::DrawChecker(dl, r.A, r.B);
            const float rr = std::min(rounding, std::min(r.B.x - r.A.x, r.B.y - r.A.y) * 0.5f);
            dl->AddImageRounded((ImTextureID)(std::intptr_t)t.Id, r.A, r.B, ImVec2(0, 1),
                                ImVec2(1, 0), IM_COL32_WHITE, rr);
            return Cover::Drawn;
        }
        if (t.Failed) return Cover::Failed;
        covers::DrawSpinner(dl, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f),
                    std::min(b.x - a.x, b.y - a.y) * 0.22f,
                    ImGui::GetColorU32(ImGuiCol_TextDisabled));
        return Cover::Loading;
    }

    // Модель, материал, префаб — их обложку надо РИСОВАТЬ, и делает это общий
    // кэш редактора: не больше одной съёмки за кадр, результат помнится.
    const uint64_t cover = assetslot::Cover(m_preview, full, (int)kCoverH);
    if (!cover) return Cover::None;
    dl->AddImageRounded((ImTextureID)(std::intptr_t)cover, a, b, ImVec2(0, 1), ImVec2(1, 0),
                        IM_COL32_WHITE, rounding);
    return Cover::Drawn;
}

// Превью под курсором.
//
// ЗАЧЕМ. Плитка 80x80 отвечает на вопрос «который из этих файлов», и на этом её
// возможности кончаются. «Та ли это картинка», «что на ней написано», «какого
// она размера», «почему она весит сорок мегабайт» — по плитке не видно ничего,
// и до сих пор ответ добывался открыванием файла. Превью показывает ту же
// картинку в шесть-семь раз крупнее и подписывает то, что по ней не прочитать:
// размеры в пикселях, вес, тип.
void FileBrowser::DrawHoverPreview(const fs::path& full, const Entry& entry) {
    // Содержимое подсказки — общее с панелью ассетов (AssetCovers.h): одна и
    // та же картинка не должна показываться в двух местах по-разному.
    covers::DrawHoverPreview(full, entry.IsDir, entry.Name, entry.Size, m_preview);
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
    if (!m_listsLoaded) {
        m_favorites = pathlist::Split(sage::editor::prefs::GetString("filebrowser.favorites", ""));
        m_recent = pathlist::Split(sage::editor::prefs::GetString("filebrowser.recent", ""));
        m_listsLoaded = true;
    }
    m_cfg = config;
    m_error.clear();
    m_selected = -1;
    m_search[0] = '\0';
    m_newFolder[0] = '\0';
    std::snprintf(m_name, sizeof(m_name), "%s", m_cfg.DefaultName.c_str());

    std::error_code ec;
    // Граница приводится к каноничному виду ОДИН раз: дальше с ней сравнивают
    // каждый переход, и делать это над «..» и символическими ссылками значит
    // сравнивать разные записи одного и того же пути.
    if (!m_cfg.Root.empty()) {
        m_cfg.Root = fs::weakly_canonical(fs::absolute(m_cfg.Root, ec), ec);
        // Границы нет на диске (проект снесли, папку переименовали) — тогда её
        // нет и вовсе: запереть диалог в несуществующей папке значит показать
        // пустоту без выхода.
        if (!fs::is_directory(m_cfg.Root, ec)) m_cfg.Root.clear();
    }

    fs::path start = m_cfg.StartDir;
    if (start.empty() || !fs::is_directory(start, ec)) start = fs::current_path(ec);
    // Начальная папка вне границы — начинаем с самой границы, а не «где-то там»:
    // иначе первый же кадр показал бы место, куда потом нельзя вернуться.
    if (!m_cfg.Root.empty() && !WithinRoot(start)) start = m_cfg.Root;
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

    // ЗА ГРАНИЦЕЙ БЫСТРОМУ ДОСТУПУ НЕ МЕСТО. «Документы», диски и флешки — это
    // кнопки НАРУЖУ, и в диалоге выбора ассета проекта каждая из них означала
    // бы «уйти туда, откуда выбрать нельзя». Вместо них — сама граница: одним
    // щелчком вернуться в корень ассетов.
    if (!m_cfg.Root.empty()) {
        group(T("Project"));
        place(T("Project assets"), m_cfg.Root);
        m_open = true;
        m_needsOpen = true;
        return;
    }

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

bool FileBrowser::WithinRoot(const fs::path& p) const {
    // Само правило — в PathScope.h: его же спрашивают и другие места, и там же
    // оно проверяется тестом.
    return sage::editor::pathscope::Within(m_cfg.Root, p);
}

void FileBrowser::GoTo(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    // ВЫХОД ЗА ГРАНИЦУ ПРОСТО НЕ ПРОИСХОДИТ. Не окно с отказом: человек не
    // просил объяснений, он нажал «вверх» — и там, куда он метил, выбирать
    // нечего. Кнопки наружу при этом не показываются вовсе (см. Open и
    // DrawBreadcrumbs), так что нажать на такую неоткуда.
    if (!WithinRoot(dir)) return;
    m_dir = fs::absolute(dir, ec).lexically_normal();
    m_selected = -1;
    Refresh();
    // История — каждая папка, в которую зашли: «где я только что был» —
    // ровно тот вопрос, с которым открывают диалог второй раз.
    pathlist::Remember(m_recent, sage::PathToUtf8(m_dir), 12);
    SaveLists();
}

void FileBrowser::SaveLists() const {
    sage::editor::prefs::SetString("filebrowser.favorites", pathlist::Join(m_favorites));
    sage::editor::prefs::SetString("filebrowser.recent", pathlist::Join(m_recent));
}

// Группа путей в быстром доступе. Только существующие и только в границе
// диалога: кнопка в папку, которой нет, или туда, откуда выбирать нельзя, хуже
// отсутствующей.
void FileBrowser::DrawPathGroup(const char* title, const std::vector<std::string>& paths,
                                bool favorites) {
    std::error_code ec;
    bool header = false;
    std::string removeFav;
    for (const std::string& utf8 : paths) {
        const fs::path p = sage::PathFromUtf8(utf8);
        if (!fs::is_directory(p, ec) || !WithinRoot(p)) continue;
        if (!header) {
            ImGui::Spacing();
            ImGui::TextDisabled("%s", title);
            ImGui::Separator();
            header = true;
        }
        ImGui::PushID(utf8.c_str());
        std::string label = sage::PathToUtf8(p.filename());
        if (label.empty()) label = utf8;
        const bool here = p == m_dir;
        if (here) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
        if (EditorIcons::Button(favorites ? "folder-full" : "clock", label.c_str())) GoTo(p);
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", utf8.c_str());
        if (favorites) {
            if (Sage::UI::MenuScope favMenu; ImGui::BeginPopupContextItem("##fav_menu")) {
                if (EditorIcons::MenuItem("trash", T("Remove from favorites"))) removeFav = utf8;
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();
    }
    if (!removeFav.empty()) {
        pathlist::Toggle(m_favorites, removeFav);
        SaveLists();
    }
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
    // Избранное — первым: это места, которые человек выбрал сам.
    DrawPathGroup(T("Favorites"), m_favorites, true);
    DrawPathGroup(T("Recent"), m_recent, false);
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
    // ЗА ГРАНИЦЕЙ КРОШЕК НЕТ. Показывать «/home/user/Проекты/Игра/assets»
    // целиком значит рисовать пять кнопок, из которых работает одна: остальные
    // ведут наружу, куда диалог не пустит. Крошки начинаются с самой границы —
    // она и есть верх мира для этого диалога.
    size_t rootParts = 0;
    for (auto it = m_cfg.Root.begin(); it != m_cfg.Root.end(); ++it) ++rootParts;

    fs::path acc;
    std::vector<fs::path> parts;
    size_t seen = 0;
    for (const fs::path& part : m_dir) {
        // Всё, что ВЫШЕ границы, копим в acc молча: по этим кускам собирается
        // рабочий путь для кнопок, но кнопок у них нет.
        if (rootParts > 0 && seen + 1 < rootParts) acc /= part;
        else parts.push_back(part);
        ++seen;
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        acc /= parts[i];
        std::string label = parts[i].string();
        if (label.empty() || label == "/") label = "/";
        ImGui::PushID((int)i);
        if (ImGui::Button(label.c_str())) GoTo(acc);
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
        const ImVec2 ip0 = ImGui::GetCursorScreenPos();
        const Cover cover =
            DrawCover(full, e.IsDir, ip0.x, ip0.y, ip0.x + icon, ip0.y + icon, 3.0f);
        if (cover == Cover::None || cover == Cover::Failed) {
            EditorIcons::Inline(cover == Cover::Failed ? "warn" : IconFor(e.Name, e.IsDir));
        } else {
            // Обложка занимает место сама, но зазор до имени обязан быть тем
            // же, что и у значка (EditorIcons::Inline его уже включает).
            ImGui::Dummy(ImVec2(icon + EditorIcons::TextGap(), icon));
        }
        ImGui::SameLine(0.0f, 0.0f);
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
        // Превью и в строках тоже: вид выбирают по привычке, а не по тому, нужно
        // ли сейчас разглядеть картинку, и лишать строки превью значит заставлять
        // человека переключать вид ради одного взгляда.
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) DrawHoverPreview(full, e);
        if (!e.IsDir) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70.0f);
            ImGui::TextDisabled("%s", covers::HumanSize(e.Size).c_str());
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

        ImU32 bg = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.035f));
        if (i == m_selected) bg = ImGui::GetColorU32(ImGuiCol_Header);
        else if (hovered)    bg = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
        dl->AddRectFilled(p0, p1, bg, 6.0f);

        const ImVec2 c0(p0.x + 4.0f, p0.y + 4.0f);
        const ImVec2 c1(p1.x - 4.0f, p0.y + kCoverH);
        dl->AddRectFilled(c0, c1, ImGui::GetColorU32(ImVec4(0, 0, 0, 0.28f)), 5.0f);

        const Cover cover = DrawCover(full, e.IsDir, c0.x, c0.y, c1.x, c1.y, 5.0f);
        if (cover == Cover::None || cover == Cover::Failed) {
            // Обложки нет (папка, звук, текст, ещё не снятая модель) — значок
            // типа во всю площадку. Пустая площадка читалась бы как «файл битый».
            // А вот если картинка ИМЕННО ЧТО не открылась — значок «внимание»:
            // это разные вещи, и одинаковым значком они были бы неразличимы.
            const float glyph = 34.0f;
            const bool failed = cover == Cover::Failed;
            EditorIcons::Overlay(c0.x + (c1.x - c0.x - glyph) * 0.5f,
                                 c0.y + (c1.y - c0.y - glyph) * 0.5f, glyph,
                                 failed ? "warn" : (e.IsDir ? "folder" : IconFor(e.Name, e.IsDir)),
                                 failed ? glm::vec3(0.95f, 0.62f, 0.38f)
                                        : glm::vec3(0.72f, 0.74f, 0.78f));
        }

        // Имя обрезается многоточием: под плиткой одна строка, и длинное имя
        // иначе уехало бы на соседнюю плитку.
        const std::string label = TruncateToWidth(e.Name, kTileW - 10.0f);
        const ImVec2 size = ImGui::CalcTextSize(label.c_str());
        dl->AddText(ImVec2(p0.x + (kTileW - size.x) * 0.5f, p0.y + kCoverH + 4.0f),
                    ImGui::GetColorU32(ImGuiCol_Text), label.c_str());

        // Превью — ПОСЛЕ плитки: оно уходит в своё окно, и открывать его раньше,
        // чем дорисована сама плитка, значит перемешивать два списка отрисовки.
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) DrawHoverPreview(full, e);

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
        // В избранное — нынешнюю папку одним щелчком; повторный — убрать.
        {
            const std::string here = sage::PathToUtf8(m_dir);
            const bool fav = pathlist::Contains(m_favorites, here);
            if (EditorIcons::Button(fav ? "folder-full" : "folder", fav ? T("In favorites") : T("To favorites"),
                                    fav ? T("Remove this folder from favorites")
                                        : T("Add this folder to favorites: it will be one click away "
                                            "in every file dialog"),
                                    fav)) {
                pathlist::Toggle(m_favorites, here);
                SaveLists();
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        ImGui::InputTextWithHint("##search", T("Search..."), m_search, sizeof(m_search));

        // Отступы темы для меню: всплывающее окно наследует стиль, действующий в
        // момент открытия (см. Sage::UI::MenuScope). Время жизни MenuScope
        // держится в границах if — иначе деструктор снялся бы после
        // EndChild() списка ниже, и ImGui решил бы, что стиль не сняли вовсе.
        if (Sage::UI::MenuScope newFolderMenu; ImGui::BeginPopup("##newfolder")) {
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
        if (m_cfg.Mode == PickMode::OpenAny) {
            ImGui::TextDisabled("%s", T("A file, a folder or a .zip — nothing chosen means this folder"));
        }
        // ГРАНИЦУ ВИДНО. Человек, не нашедший в диалоге своей папки «Загрузки»,
        // обязан понять, почему её там нет, — иначе это выглядит как потерянный
        // список мест.
        if (!m_cfg.Root.empty()) {
            ImGui::TextDisabled("%s", T("Inside the project only. Outside files: Assets > Import"));
        }

        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());

        const char* okLabel = m_cfg.Mode == PickMode::SaveFile     ? T("Save")
                              : m_cfg.Mode == PickMode::PickFolder ? T("Choose folder")
                              : m_cfg.Mode == PickMode::OpenAny    ? T("Bring in")
                                                               : T("Open");
        if (ImGui::Button(okLabel, ImVec2(140, 0))) {
            if (m_cfg.Mode == PickMode::PickFolder) {
                m_result = m_dir;
                confirmed = true;
            } else if (m_cfg.Mode == PickMode::OpenAny && !m_name[0]) {
                // Ничего не выбрано — значит «внести эту папку целиком»: в неё
                // человек и зашёл, разглядывая содержимое.
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
                if (m_cfg.Mode == PickMode::OpenAny && !fs::exists(candidate, ec)) {
                    m_error = T("No such file or folder: ") + candidate.string();
                } else if (m_cfg.Mode == PickMode::OpenFile && !fs::exists(candidate, ec)) {
                    m_error = T("No such file: ") + candidate.string();
                } else if (!WithinRoot(candidate)) {
                    // ИМЯ МОЖНО НАБРАТЬ РУКАМИ, и «..\..\Загрузки\текстура.png»
                    // в поле имени обошло бы любые запреты навигации: «папка
                    // плюс имя» для абсолютного пути даёт сам этот путь. Здесь
                    // единственное место, где выбор становится ответом, — и
                    // граница обязана стоять именно тут.
                    m_error = T("Only from inside the project: put the file into the project first "
                                "(Assets > Import)");
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
