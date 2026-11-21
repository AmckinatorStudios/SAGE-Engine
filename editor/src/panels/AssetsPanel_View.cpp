// Вид панели Assets: шапка (путь, вид, масштаб, поиск, сортировка, импорт,
// создание), дерево папок и список элементов текущей папки, общий для сетки и
// таблицы. Настройки вида — привычка человека: живут в EditorPrefs.
#include "AssetsPanel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_set>

#include "imgui.h"

#include "../CreateAssetDialog.h"
#include "../DragSet.h"
#include "../EditorIcons.h"
#include "../FolderColors.h"
#include "../InspectorControls.h"
#include "../Localization.h"
#include "AssetsPanelDetail.h"
#include "EditorHost.h"
#include "EditorPrefs.h"
#include "EditorTheme.h"
#include "FsCache.h"
#include "Project.h"
#include "ui/UI.h"

namespace fs = std::filesystem;
namespace fscache = sage::editor::fscache;
namespace prefs = sage::editor::prefs;
namespace ictl = sage::editor::ictl;
using namespace sage::editor::assetspanel_detail;

namespace {
const char* const kSortKeys[] = {"name", "type", "size", "modified"};

double NowSeconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}
} // namespace

// ============================================================================
//  Настройки вида
// ============================================================================

void AssetsPanel::LoadPrefs() {
    m_view = prefs::GetString("assets.view", "grid") == "list" ? ViewMode::List : ViewMode::Grid;
    m_tileSize = std::clamp(prefs::GetFloat("assets.tile_size", kTileDefault), kTileMin, kTileMax);
    m_rowHeight = std::clamp(prefs::GetFloat("assets.row_height", kRowDefault), kRowMin, kRowMax);
    m_treeWidth = std::clamp(prefs::GetFloat("assets.tree_width", kTreeDefault), kTreeMin, kTreeMax);
    const std::string sort = prefs::GetString("assets.sort", "name");
    m_sortBy = SortBy::Name;
    for (int i = 0; i < 4; ++i)
        if (sort == kSortKeys[i]) m_sortBy = static_cast<SortBy>(i);
    m_sortDesc = prefs::GetBool("assets.sort_desc", false);
    m_prefsLoaded = true;
    m_tableSortSync = true;
}

void AssetsPanel::SetView(ViewMode v) {
    m_view = v;
    m_tableSortSync = true;
    prefs::SetString("assets.view", v == ViewMode::List ? "list" : "grid");
}
void AssetsPanel::SetTileSize(float px) {
    m_tileSize = std::clamp(std::round(px), kTileMin, kTileMax);
    prefs::SetFloat("assets.tile_size", m_tileSize);
}
void AssetsPanel::SetRowHeight(float px) {
    m_rowHeight = std::clamp(std::round(px), kRowMin, kRowMax);
    prefs::SetFloat("assets.row_height", m_rowHeight);
}
void AssetsPanel::SetTreeWidth(float px) {
    m_treeWidth = std::clamp(std::round(px), kTreeMin, kTreeMax);
    prefs::SetFloat("assets.tree_width", m_treeWidth);
}
void AssetsPanel::SetSort(SortBy by, bool descending) {
    m_sortBy = by;
    m_sortDesc = descending;
    m_tableSortSync = true;
    prefs::SetString("assets.sort", kSortKeys[static_cast<int>(by)]);
    prefs::SetBool("assets.sort_desc", descending);
}
void AssetsPanel::SetSearch(const std::string& text) {
    std::snprintf(m_search, sizeof(m_search), "%s", text.c_str());
}

// ============================================================================
//  Список элементов текущей папки
// ============================================================================

void AssetsPanel::RefreshItems(EditorHost& host) {
    const fs::path& cwd = host.AssetsCwd();
    const fscache::Listing& listing = fscache::List(cwd, 0.0);
    const std::string filter = ToLower(m_search);
    const std::string dirKey = cwd.string();
    const bool setChanged = dirKey != m_itemsDir || listing.Generation != m_itemsGen || filter != m_itemsFilter;
    // Размер и дата меняются, не трогая состав папки, — раз в секунду
    // пересобираем и без повода (сведения берутся из FsCache, не с диска).
    const double now = NowSeconds();
    if (!setChanged && m_sortBy == m_itemsSort && m_sortDesc == m_itemsDesc && now - m_itemsTime < 1.0) return;
    m_itemsDir = dirKey;
    m_itemsGen = listing.Generation;
    m_itemsFilter = filter;
    m_itemsSort = m_sortBy;
    m_itemsDesc = m_sortDesc;
    m_itemsTime = now;

    m_items.clear();
    auto add = [&](const fs::path& p, bool isDir) {
        Item it;
        it.Path = p;
        it.IsDir = isDir;
        it.Name = p.filename().string();
        if (!filter.empty() && ToLower(it.Name).find(filter) == std::string::npos) return;
        it.Kind = KindLabel(p, isDir);
        const fscache::FileInfo info = fscache::Info(p, 2.0);
        it.Size = info.Size;
        it.Stamp = info.Stamp;
        m_items.push_back(std::move(it));
    };
    for (const fs::path& d : listing.Dirs) add(d, true);
    for (const fs::path& f : listing.Files) add(f, false);

    // ПАПКИ ВСЕГДА ПЕРВЫМИ, при любой сортировке: «где мои модели» ищут по
    // папкам, и папка, уехавшая в середину по дате, теряется среди файлов.
    // Равные по ключу — по имени без регистра, чтобы порядок не прыгал.
    const SortBy by = m_sortBy;
    const bool desc = m_sortDesc;
    auto nameLess = [](const Item& a, const Item& b) {
        const std::string la = ToLower(a.Name), lb = ToLower(b.Name);
        return la != lb ? la < lb : a.Name < b.Name;
    };
    std::stable_sort(m_items.begin(), m_items.end(), [&](const Item& a, const Item& b) {
        if (a.IsDir != b.IsDir) return a.IsDir;
        int c = 0;
        switch (by) {
            case SortBy::Name: break;
            case SortBy::Type: c = a.Kind.compare(b.Kind); break;
            case SortBy::Size: c = a.Size < b.Size ? -1 : (a.Size > b.Size ? 1 : 0); break;
            case SortBy::Modified: c = a.Stamp < b.Stamp ? -1 : (a.Stamp > b.Stamp ? 1 : 0); break;
        }
        if (c == 0) return desc && by == SortBy::Name ? nameLess(b, a) : nameLess(a, b);
        return desc ? c > 0 : c < 0;
    });

    m_visibleOrder.clear();
    m_visibleOrder.reserve(m_items.size());
    for (const Item& it : m_items) m_visibleOrder.push_back(it.Path);

    if (!setChanged) return;
    // СОСТАВ ПАПКИ СМЕНИЛСЯ — выделение приводим к тому, что в ней есть. Файл,
    // оставшийся выделенным в прошлой папке, иначе удалялся бы клавишей Delete
    // здесь, невидимым. Скрытые поиском — остаются (поиск сотрут — снова видны),
    // но Delete их не берёт (DeletableSelection).
    std::unordered_set<std::string> present;
    for (const fs::path& d : listing.Dirs) present.insert(d.string());
    for (const fs::path& f : listing.Files) present.insert(f.string());
    m_multi.erase(std::remove_if(m_multi.begin(), m_multi.end(),
                                 [&](const fs::path& p) { return !present.count(p.string()); }),
                  m_multi.end());
    // Первичный выбор (его смотрит инспектор) снимается, только если файл
    // исчез с диска: переход в другую папку инспектор не обнуляет.
    if (!m_selected.empty() && !present.count(m_selected.string()) &&
        m_selected.parent_path() == cwd)
        m_selected.clear();
    if (!m_anchor.empty() && !present.count(m_anchor.string())) m_anchor.clear();
    if (!m_pendingPick.empty() && !present.count(m_pendingPick.string())) m_pendingPick.clear();

    // Обложки файлов, которых в папке нет, отпускаем вместе с буферами — иначе
    // за сеанс в видеопамяти копились бы обложки всего, что показывали.
    // Здесь, при смене состава, а не каждый кадр с вопросом к диску о каждой.
    for (auto it = m_thumbs.begin(); it != m_thumbs.end();) {
        if (present.count(it->first)) { ++it; continue; }
        m_preview.ReleaseTarget(it->first + "@" + std::to_string(it->second.Px));
        it = m_thumbs.erase(it);
    }
}

// ============================================================================
//  Шапка
// ============================================================================

// ПУТЬ — СТРОКОЙ, А НЕ РЯДОМ КНОПОК: «Assets › Models › Characters». Сегменты
// откликаются на наведение (подчёркивание) и щелчок, на них бросают файлы
// (перенос в эту папку). Не влезает — середина сворачивается в «…» со списком
// скрытых уровней, а корень и хвост остаются на виду: нужно то место, где
// стоишь, и дорога на шаг-два назад.
void AssetsPanel::DrawBreadcrumb(EditorHost& host, float width) {
    fs::path& cwd = host.AssetsCwd();
    const fs::path root = AssetsRoot(host);
    std::vector<fs::path> chain;
    const fs::path& rootCanon = fscache::Canonical(root);
    for (fs::path p = cwd; !p.empty(); p = p.parent_path()) {
        chain.push_back(p);
        if (fscache::Canonical(p) == rootCanon || !p.has_parent_path() || p.parent_path() == p) break;
    }
    std::reverse(chain.begin(), chain.end());

    auto labelOf = [&](size_t i) {
        if (i == 0) return std::string(T("Assets"));
        std::string s = chain[i].filename().string();
        return s.empty() ? chain[i].string() : s;
    };
    const float frameH = ImGui::GetFrameHeight();
    const float sepW = std::floor(ImGui::GetFontSize() * 0.75f) + 8.0f;
    const float dotsW = frameH;
    std::vector<float> w(chain.size());
    float total = 0.0f;
    for (size_t i = 0; i < chain.size(); ++i) {
        w[i] = ImGui::CalcTextSize(labelOf(i).c_str()).x;
        total += w[i] + (i ? sepW : 0.0f);
    }
    // Сколько хвоста помещается после «Assets › … ›».
    size_t firstTail = 1;
    if (total > width && chain.size() > 2) {
        float used = w[0] + sepW + dotsW + sepW;
        firstTail = chain.size() - 1;
        used += w[firstTail];
        while (firstTail > 1 && used + w[firstTail - 1] + sepW <= width) used += w[--firstTail] + sepW;
        if (firstTail <= 1) firstTail = 1;
    }
    const bool collapsed = firstTail > 1;

    const ImU32 dim = EditorTheme::Color32(EditorTheme::Role::TextDim);
    const ImU32 strong = EditorTheme::Color32(EditorTheme::Role::Text);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float startX = ImGui::GetCursorScreenPos().x;
    auto separator = [&]() {
        ImGui::SameLine(0, 0);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(sepW, frameH));
        const float s = std::floor(ImGui::GetFontSize() * 0.75f);
        const ImVec4 c = EditorTheme::Color(EditorTheme::Role::TextFaint);
        EditorIcons::Overlay(dl, std::floor(at.x + (sepW - s) * 0.5f), std::floor(at.y + (frameH - s) * 0.5f), s,
                             "chevron-right", glm::vec3(c.x, c.y, c.z));
        ImGui::SameLine(0, 0);
    };
    auto segment = [&](size_t i) {
        const bool current = i + 1 == chain.size();
        std::string label = labelOf(i);
        // Последний сегмент длиннее, чем осталось места, — обрезаем его, а не
        // выталкиваем шапку за край.
        const float left = width - (ImGui::GetCursorScreenPos().x - startX);
        if (current && w[i] > left) label = TruncateToWidth(label, std::max(24.0f, left));
        const float tw = ImGui::CalcTextSize(label.c_str()).x;
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##crumb", ImVec2(std::max(1.0f, tw), frameH));
        const bool hovered = ImGui::IsItemHovered() && !current;
        const ImVec2 textAt(at.x, at.y + std::floor((frameH - ImGui::GetTextLineHeight()) * 0.5f));
        dl->AddText(textAt, current || hovered ? strong : dim, label.c_str());
        if (hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            const float y = textAt.y + ImGui::GetTextLineHeight();
            dl->AddLine(ImVec2(textAt.x, y), ImVec2(textAt.x + tw, y), strong);
        }
        if (!current && ImGui::IsItemClicked(ImGuiMouseButton_Left)) cwd = chain[i];
        if (!current && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ASSET_PATH"))
                MoveGroupIntoFolder(host, dragset::Assets(p), chain[i]);
            ImGui::EndDragDropTarget();
        }
        if (current && label != labelOf(i) && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", labelOf(i).c_str());
        ImGui::PopID();
    };

    segment(0);
    if (collapsed) {
        separator();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##crumb_more", ImVec2(dotsW, frameH));
        const bool hovered = ImGui::IsItemHovered();
        const float s = std::floor(ImGui::GetFontSize() * 0.9f);
        const ImVec4 c = EditorTheme::Color(hovered ? EditorTheme::Role::Text : EditorTheme::Role::TextDim);
        EditorIcons::Overlay(dl, std::floor(at.x + (dotsW - s) * 0.5f), std::floor(at.y + (frameH - s) * 0.5f), s,
                             "dots", glm::vec3(c.x, c.y, c.z));
        if (hovered) ImGui::SetTooltip("%s", T("Hidden path levels"));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) ImGui::OpenPopup("###assets_crumbs_hidden");
        if (Sage::UI::MenuScope crumbsMenu; ImGui::BeginPopup("###assets_crumbs_hidden")) {
            for (size_t i = 1; i < firstTail; ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (EditorIcons::MenuItem("folder", labelOf(i).c_str())) cwd = chain[i];
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
    }
    for (size_t i = firstTail; i < chain.size(); ++i) {
        separator();
        segment(i);
    }
}

// Пункты «Создать» — одни для правой кнопки по пустому месту и для кнопки
// в шапке. ОДИН ПУНКТ НА ВИД, настройки — в окне (CreateAssetDialog.h).
void AssetsPanel::DrawCreateMenu(EditorHost& host) {
    namespace ca = sage::editor::createasset;
    auto startCreate = [this, &host](ca::What kind, const char* defaultName) {
        ca::Request req;
        req.Kind = kind;
        req.Name = defaultName;
        // Сцена — всегда в assets/Scenes; остальное — в открытой папке.
        req.Dir = kind == ca::What::Scene && host.CurrentProject().Loaded() ? host.CurrentProject().ScenesDir()
                                                                            : host.AssetsCwd();
        req.OpenAfter = true;   // граф — сразу в его редактор
        req.Done = [this, &host, kind](const fs::path& made) {
            if (kind == ca::What::Scene) host.AssetsCwd() = made.parent_path();
            fscache::Invalidate(made.parent_path());
            m_itemsTime = -1.0;
            m_selected = made;
            m_multi = {made};
            m_anchor = made;
        };
        ca::Open(req);
    };
    if (EditorIcons::MenuItem("folder-plus", T("Folder..."))) startCreate(ca::What::Folder, "NewFolder");
    if (EditorIcons::MenuItem("scene", T("Scene..."))) startCreate(ca::What::Scene, "NewScene");
    // Логика — один пункт на все виды (LogicKinds.h).
    if (EditorIcons::MenuItem("script", T("Script or graph..."))) startCreate(ca::What::Script, "NewScript");
    if (EditorIcons::MenuItem("material", T("Material..."))) startCreate(ca::What::Material, "NewMaterial");
    if (EditorIcons::MenuItem("anim", T("Animation graph..."))) startCreate(ca::What::AnimGraph, "NewAnimGraph");
    if (EditorIcons::MenuItem("shader", T("Shader..."))) startCreate(ca::What::Shader, "NewShader");
    if (EditorIcons::MenuItem("file", T("Text file..."))) startCreate(ca::What::TextFile, "notes");
    ImGui::Separator();
    if (EditorIcons::MenuItem("refresh", T("Convert the whole folder to engine formats"))) ConvertFolderHere(host);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Models and images in this folder (including nested) -> .sagemesh/.sagetex.\n"
                                  "Already converted files are skipped, sources stay in place."));
    }
}

// ШАПКА — ДВЕ СТРОКИ: «где я и как смотрю» (путь; вид и масштаб справа) и
// «что делаю здесь» (поиск; сортировка, импорт, создание). Всё одной высоты,
// значков — только там, где они понятнее слова.
void AssetsPanel::DrawHeader(EditorHost& host) {
    fs::path& cwd = host.AssetsCwd();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float frameH = ImGui::GetFrameHeight();

    // --- Строка 1: вверх, путь | сетка/таблица, масштаб ---
    const bool atRoot = fscache::Canonical(cwd) == fscache::Canonical(AssetsRoot(host));
    ImGui::BeginDisabled(atRoot || !cwd.has_parent_path());
    if (EditorIcons::IconOnlyButton("up", T("Up"))) cwd = cwd.parent_path();
    ImGui::EndDisabled();
    ImGui::SameLine(0, st.ItemSpacing.x);

    const float zoomW = std::floor(ImGui::GetFontSize() * 5.5f);
    const float viewW = frameH * 2.0f;
    const float rightW = viewW + st.ItemSpacing.x + zoomW;
    const float crumbW = std::max(40.0f, ImGui::GetContentRegionAvail().x - rightW - st.ItemSpacing.x * 2.0f);
    const float rowX = ImGui::GetCursorPosX();
    DrawBreadcrumb(host, crumbW);
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rowX + crumbW + st.ItemSpacing.x * 2.0f));

    const ictl::GroupItem views[] = {
        {"grid", T("Grid"), T("Cards with previews"), m_view == ViewMode::Grid},
        {"list", T("List"), T("Table: name, type, size, date"), m_view == ViewMode::List},
    };
    const int picked = ictl::ButtonGroup("##assets_view", views, 2, ictl::ButtonLook::IconOnly);
    if (picked >= 0) SetView(picked == 0 ? ViewMode::Grid : ViewMode::List);
    ImGui::SameLine(0, st.ItemSpacing.x);

    // Масштаб: в сетке — размер карточек, в таблице — высота строк. Пишется
    // в настройки по отпусканию, а не на каждом пикселе перетаскивания.
    const bool grid = m_view == ViewMode::Grid;
    float zoom = grid ? m_tileSize : m_rowHeight;
    ImGui::SetNextItemWidth(zoomW);
    if (ImGui::SliderFloat("##assets_zoom", &zoom, grid ? kTileMin : kRowMin, grid ? kTileMax : kRowMax, "",
                           ImGuiSliderFlags_AlwaysClamp)) {
        (grid ? m_tileSize : m_rowHeight) = std::round(zoom);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) grid ? SetTileSize(m_tileSize) : SetRowHeight(m_rowHeight);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", grid ? T("Card size (Ctrl + wheel)") : T("Row height (Ctrl + wheel)"));

    // --- Строка 2: поиск | сортировка, импорт, создать ---
    static const char* const kSortNames[] = {"Name", "Type", "Size", "Modified"};
    const std::string sortLabel = std::string(T(kSortNames[static_cast<int>(m_sortBy)]));
    const float sortW = ImGui::CalcTextSize(sortLabel.c_str()).x + frameH + st.FramePadding.x * 3.0f;
    const float importW = ImGui::CalcTextSize(T("Import...")).x + frameH * 0.68f + st.FramePadding.x * 3.0f;
    const float searchW = std::max(80.0f, ImGui::GetContentRegionAvail().x - sortW - importW - frameH -
                                              st.ItemSpacing.x * 3.0f);
    Sage::UI::SearchField("assets_search", m_search, sizeof(m_search), T("Search..."), searchW);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(sortW);
    if (ictl::BeginDropdown("##assets_sort", sortLabel.c_str())) {
        for (int i = 0; i < 4; ++i)
            if (EditorIcons::MenuItemSelected("list", T(kSortNames[i]), static_cast<int>(m_sortBy) == i))
                SetSort(static_cast<SortBy>(i), m_sortDesc);
        ImGui::Separator();
        if (EditorIcons::MenuItemSelected("list", T("Ascending"), !m_sortDesc)) SetSort(m_sortBy, false);
        if (EditorIcons::MenuItemSelected("list", T("Descending"), m_sortDesc)) SetSort(m_sortBy, true);
        ictl::EndDropdown();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Sort order (folders always first)"));
    ImGui::SameLine();
    DrawImportButton(host);
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("plus", T("Create"))) ImGui::OpenPopup("###assets_create_btn");
    if (Sage::UI::MenuScope createBtnMenu; ImGui::BeginPopup("###assets_create_btn")) {
        DrawCreateMenu(host);
        ImGui::EndPopup();
    }
}

// ============================================================================
//  Дерево папок
// ============================================================================

void AssetsPanel::DrawFolderNode(EditorHost& host, const fs::path& dir, int depth) {
    // Подпапки и «та ли это папка» — с памятью (FsCache.h): дерево рисуется
    // каждый кадр, и листать диск на каждую строку — заметный кусок FPS.
    const std::vector<fs::path>& subdirs = fscache::List(dir, 0.5).Dirs;
    fs::path& cwd = host.AssetsCwd();
    const fs::path& canon = fscache::Canonical(dir);
    const bool current = fscache::Canonical(cwd) == canon;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DrawLinesNone;
    if (subdirs.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (current) flags |= ImGuiTreeNodeFlags_Selected;
    // Корень раскрыт сразу: свёрнутое дерево из одной строки требует лишнего
    // щелчка каждый раз.
    if (depth == 0) ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    // ДЕРЕВО ИДЁТ ЗА ПАПКОЙ. В папку вошли из сетки, пути или «Показать в
    // Assets» — дерево раскрывает дорогу к ней. Иначе текущая папка пряталась
    // в свёрнутой ветке, и где ты находишься, дерево не показывало.
    if (m_revealCwd && !current && std::find(m_cwdChain.begin(), m_cwdChain.end(), canon) != m_cwdChain.end())
        ImGui::SetNextItemOpen(true);

    ImGui::PushID(dir.string().c_str());
    const ImVec2 rowPos = ImGui::GetCursorScreenPos();
    const float indent = ImGui::GetTreeNodeToLabelSpacing();
    m_treeLines.Row(rowPos.y, depth, rowPos.x + ImGui::GetStyle().FramePadding.x, rowPos.x + indent, !subdirs.empty());
    const bool open = ImGui::TreeNodeEx("##folder", flags, "%s", "");
    if (current && m_revealCwd && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);
    // Значок и цвет — ТЕ ЖЕ, что в сетке, и из того же источника (FolderIcon
    // знает про содержимое целиком, а не только про подпапки).
    {
        glm::vec3 tint(0.85f, 0.68f, 0.32f);
        sage::editor::foldercolors::Get(dir, tint);
        const ImVec4 c(tint.x, tint.y, tint.z, 1.0f);
        const std::string label = depth == 0 ? std::string(T("Assets")) : dir.filename().string();
        EditorIcons::DrawLabeled(ImGui::GetWindowDrawList(), ImVec2(rowPos.x + indent, rowPos.y),
                                 ImGui::GetTextLineHeight(), FolderIcon(dir), ImGui::GetColorU32(c), label.c_str(),
                                 ImGui::GetColorU32(ImGuiCol_Text));
    }
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) cwd = dir;

    // Цветная метка ставится и отсюда: подменю общее (FolderColors.h).
    if (Sage::UI::MenuScope folderMenu; ImGui::BeginPopupContextItem("##folder_menu")) {
        ImGui::TextDisabled("%s", dir.filename().string().c_str());
        ImGui::Separator();
        sage::editor::foldercolors::DrawPaletteMenu(dir);
        if (EditorIcons::MenuItem("folder", T("Show in Assets"))) cwd = dir;
        ImGui::EndPopup();
    }
    // Бросок файла на папку дерева — перенос в неё; объекта — префаб в ней.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ASSET_PATH"))
            MoveGroupIntoFolder(host, dragset::Assets(p), dir);
        AcceptObjectAsPrefab(host, dir);
        ImGui::EndDragDropTarget();
    }
    if (open && !subdirs.empty()) {
        for (const fs::path& sub : subdirs) DrawFolderNode(host, sub, depth + 1);
        m_treeLines.Draw(rowPos, indent, depth + 1);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void AssetsPanel::DrawFolderTree(EditorHost& host) {
    // Папку сменили (откуда угодно) — дерево раскроет к ней путь один раз.
    const fs::path& cwd = host.AssetsCwd();
    if (cwd != m_lastCwd) {
        m_lastCwd = cwd;
        m_revealCwd = true;
        m_cwdChain.clear();
        for (fs::path p = fscache::Canonical(cwd); !p.empty(); p = p.parent_path()) {
            m_cwdChain.push_back(p);
            if (!p.has_parent_path() || p.parent_path() == p) break;
        }
    }
    ImGui::BeginChild("##assets_tree", ImVec2(m_treeWidth, 0), ImGuiChildFlags_Borders);
    m_treeLines.Clear();
    DrawFolderNode(host, AssetsRoot(host), 0);   // корень — assets/ (см. AssetsRoot)
    ImGui::EndChild();
    m_revealCwd = false;

    // Разделитель: ширину правят перетаскиванием, на глаз, глядя на имена.
    // Запоминается по отпусканию — до следующего запуска.
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::InvisibleButton("##assets_split", ImVec2(6.0f, ImGui::GetContentRegionAvail().y));
    if (ImGui::IsItemActive()) m_treeWidth = std::clamp(m_treeWidth + ImGui::GetIO().MouseDelta.x, kTreeMin, kTreeMax);
    if (ImGui::IsItemDeactivated()) SetTreeWidth(m_treeWidth);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) SetTreeWidth(kTreeDefault);
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2((a.x + b.x) * 0.5f - 1.0f, a.y),
                                                  ImVec2((a.x + b.x) * 0.5f + 1.0f, b.y),
                                                  EditorTheme::Color32(EditorTheme::Role::Accent));
    }
    ImGui::SameLine(0.0f, 0.0f);
}
