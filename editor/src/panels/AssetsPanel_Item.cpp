// Всё, что делают с элементом папки мышью и меню, — ОДНО НА ОБА ВИДА (сетка и
// таблица). Раньше это жило внутри отрисовки карточки, и второй вид означал бы
// вторую копию выбора, меню, перетаскивания и удаления — то есть два поведения,
// которые однажды разойдутся. Здесь же — переименование и удаление вместе с
// выделением: после операции оно обязано указывать на новые пути.
#include "AssetsPanel.h"

#include <algorithm>
#include <system_error>

#include "imgui.h"

#include "../AssetCovers.h"
#include "../AssetOpen.h"
#include "../AssetSlot.h"
#include "../DragSet.h"
#include "../EditorIcons.h"
#include "../FolderColors.h"
#include "../Localization.h"
#include "../RectSelect.h"
#include "AssetsPanelDetail.h"
#include "EditorHost.h"
#include "FsCache.h"
#include "Project.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/assets/import/Convert.h"
#include "sage/core/Log.h"
#include "ui/UI.h"

namespace fs = std::filesystem;
namespace covers = sage::editor::covers;
using namespace sage::editor::assetspanel_detail;

void AssetsPanel::ItemBehavior(EditorHost& host, const Item& item, bool nameCut) {
    const fs::path& path = item.Path;
    const bool isDir = item.IsDir;
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    // Где элемент на экране: самопроверка щёлкает по нему по-настоящему.
    m_tileCenters.push_back(ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f));

    // ПРЕВЬЮ ПОД КУРСОРОМ — с задержкой (ForTooltip): без неё подсказка
    // выскакивала бы у каждого файла, над которым просто провели мышью. Имя в
    // подсказке целиком — отдельной подсказки «полное имя» больше нет: она
    // ЗАМЕНЯЛА превью (SetTooltip перекрывает прежнюю подсказку кадра), и у
    // файлов с длинным именем превью не показывалось вовсе.
    (void)nameCut;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && !ImGui::IsPopupOpen("##tile_ctx")) {
        const covers::HoverInfo info{item.Kind, FormatStamp(item.Stamp)};
        covers::DrawHoverPreview(path, isDir, item.Name, item.Size, &m_preview, host.CurrentProject().Dir(), &info);
    }

    // Источник перетаскивания — в слот инспектора, на папку, в сцену.
    // Взялись за выделенный файл — едет весь набор, в порядке на экране.
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        std::vector<std::string> group;
        for (const fs::path& g : dragset::Ordered(path, m_multi, m_visibleOrder)) group.push_back(g.string());
        dragset::BeginAssets(path.string(), group);
        assetslot::BeginDrag(path, &m_preview);
        if (group.size() > 1) ImGui::TextDisabled("%s", dragset::Caption(item.Name, group.size()).c_str());
        ImGui::EndDragDropSource();
    }

    // Папка принимает файлы: бросок ПЕРЕМЕЩАЕТ (вместе с .meta — ссылки сцен
    // держатся за GUID и переезд переживают). Объект из иерархии — префаб.
    if (isDir && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_ASSET_PATH"))
            MoveGroupIntoFolder(host, dragset::Assets(p), path);
        AcceptObjectAsPrefab(host, path);
        ImGui::EndDragDropTarget();
    }

    // Рамка выделения: элемент засчитывается, если его прямоугольник задет.
    if (m_rectActive && sage::editor::rectselect::Hits(m_rect, mn, mx)) m_rectHits.push_back(path);

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !m_rectActive) ClickItem(path);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenItem(host, path, isDir);

    // MenuScope — в границах if: деструктор снимается до End() окна.
    if (Sage::UI::MenuScope itemMenu; ImGui::BeginPopupContextItem("##tile_ctx")) {
        DrawItemMenu(host, item);
        ImGui::EndPopup();
    }
}

// ТРИ РАЗНЫХ ЩЕЛЧКА: Shift — диапазон от якоря по порядку НА ЭКРАНЕ (Ctrl+Shift
// — добавить диапазон), Ctrl — добавить или убрать один, обычный — один файл
// и новый якорь. Обычный щелчок по файлу из набора решается на отпускании:
// потянули — едет весь набор (DragSet.h).
void AssetsPanel::ClickItem(const fs::path& path) {
    const ImGuiIO& io = ImGui::GetIO();
    const auto anchorAt = std::find(m_visibleOrder.begin(), m_visibleOrder.end(), m_anchor);
    const auto hereAt = std::find(m_visibleOrder.begin(), m_visibleOrder.end(), path);
    if (io.KeyShift && anchorAt != m_visibleOrder.end() && hereAt != m_visibleOrder.end()) {
        auto from = anchorAt, to = hereAt;
        if (from > to) std::swap(from, to);
        if (!io.KeyCtrl) m_multi.clear();
        for (auto it = from; it <= to; ++it)
            if (std::find(m_multi.begin(), m_multi.end(), *it) == m_multi.end()) m_multi.push_back(*it);
        m_selected = path;
    } else if (io.KeyCtrl) {
        auto it = std::find(m_multi.begin(), m_multi.end(), path);
        if (it != m_multi.end()) {
            m_multi.erase(it);
            m_selected = m_multi.empty() ? fs::path{} : m_multi.back();
        } else {
            m_multi.push_back(path);
            m_selected = path;
        }
        m_anchor = path;
    } else if (dragset::DeferSinglePick(std::find(m_multi.begin(), m_multi.end(), path) != m_multi.end(),
                                        m_multi.size(), false)) {
        m_pendingPick = path;
    } else {
        m_selected = path;
        m_multi = {path};
        m_anchor = path;
    }
}

void AssetsPanel::DrawItemMenu(EditorHost& host, const Item& item) {
    const fs::path& path = item.Path;
    const bool isDir = item.IsDir;
    // ПКМ по файлу ВНЕ набора выбирает его одного: меню — про то, на что
    // нажали. ПКМ по файлу ИЗ набора набор сохраняет (удалить двадцать разом).
    if (std::find(m_multi.begin(), m_multi.end(), path) == m_multi.end()) {
        m_multi = {path};
        m_anchor = path;
    }
    m_selected = path;
    m_pendingPick.clear();

    // «ОТКРЫТЬ» — первым и только у того, что открывается (AssetOpen.h).
    {
        using sage::editor::assetopen::OpenAs;
        const char* openLabel = nullptr;
        const char* openIcon = "open";
        switch (sage::editor::assetopen::Of(path, isDir)) {
            case OpenAs::Folder: openLabel = T("Open folder"); openIcon = "folder"; break;
            case OpenAs::Scene: openLabel = T("Open scene"); openIcon = "scene"; break;
            case OpenAs::Prefab: openLabel = T("Add to the scene"); openIcon = "prefab"; break;
            case OpenAs::Clip: openLabel = T("Open in the animation tool"); openIcon = "anim"; break;
            case OpenAs::AiGraph: openLabel = T("Open in AI Graph"); openIcon = "wire"; break;
            case OpenAs::AnimGraph: openLabel = T("Open in Animation Graph"); openIcon = "anim"; break;
            case OpenAs::VisualScript: openLabel = T("Open in Script Graph"); openIcon = "wire"; break;
            case OpenAs::Interface: openLabel = T("Open Interface"); openIcon = "layout"; break;
            case OpenAs::Text: openLabel = T("Open in the code editor"); openIcon = "code"; break;
            case OpenAs::None: break;
        }
        if (openLabel) {
            if (EditorIcons::MenuItem(openIcon, openLabel)) OpenItem(host, path, isDir);
            ImGui::Separator();
        }
    }
    // Переименование — всегда про ОДИН файл: у двадцати файлов общего имени нет.
    if (isDir) sage::editor::foldercolors::DrawPaletteMenu(path);
    if (EditorIcons::MenuItem("pencil", T("Rename"))) {
        m_renameTarget = path;
        m_error.clear();
    }
    const std::vector<fs::path> deletable = DeletableSelection();
    if (EditorIcons::MenuItem("trash", deletable.size() > 1 ? T("Delete selected") : T("Delete"), "Del"))
        m_deleteTargets = deletable;

    // СТАРТОВАЯ СЦЕНА — в меню самого файла: решение автора, а не алфавита.
    if (!isDir && path.extension() == ".sage" && host.CurrentProject().Loaded()) {
        ImGui::Separator();
        const std::string name = path.stem().string();
        if (host.CurrentProject().StartScene() == name) {
            ImGui::TextDisabled("%s", T("This is the start scene"));
        } else if (EditorIcons::MenuItem("play", T("Make start scene"))) {
            std::string err;
            if (host.CurrentProject().SetStartScene(name, err)) host.SetStatusMessage(T("Start scene: ") + name);
            else host.SetStatusMessage(T("Failed to set the start scene: ") + err);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("The built game starts from this scene"));
    }

    // Конвертация в свой формат — в меню КОНКРЕТНОГО файла.
    const std::string p = path.string();
    const bool model = !isDir && sage::assets::IsConvertibleModel(p);
    const bool texture = !isDir && sage::assets::IsConvertibleTexture(p);
    if (texture) {
        ImGui::Separator();
        if (EditorIcons::MenuItem("grid", T("Edit Sprites"))) host.OpenSpriteEditor(host.CurrentProject().AssetRef(path));
    }
    if (model || texture) {
        ImGui::Separator();
        const char* label = model ? T("Convert to .sagemesh") : T("Convert to .sagetex");
        if (EditorIcons::MenuItem("refresh", label)) ConvertOne(host, path);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("The engine's own format: loads without parsing and weighs less.\n"
                                      "The source file stays where it is."));
        }
    }
}

std::vector<fs::path> AssetsPanel::DeletableSelection() const {
    std::vector<fs::path> out;
    for (const fs::path& p : m_multi)
        if (std::find(m_visibleOrder.begin(), m_visibleOrder.end(), p) != m_visibleOrder.end()) out.push_back(p);
    return out;
}

namespace {
// Путь в выделении: прежний -> новый (пустой новый — убрать).
void Repoint(std::vector<fs::path>& v, const fs::path& from, const fs::path& to) {
    for (auto it = v.begin(); it != v.end();) {
        if (*it == from) {
            if (to.empty()) { it = v.erase(it); continue; }
            *it = to;
        }
        ++it;
    }
}
void Repoint(fs::path& p, const fs::path& from, const fs::path& to) {
    if (p == from) p = to;
}
} // namespace

bool AssetsPanel::ApplyRename(EditorHost& host, const fs::path& from, const std::string& newName, std::string& err) {
    fs::path target;
    if (!RenameAsset(from, newName, target, err)) {
        LOG_ERROR("Editor") << "Asset rename failed: " << err;
        return false;
    }
    if (target == from) return true;   // имя не изменилось — и делать нечего
    sage::editor::foldercolors::Rename(from, target);
    Repoint(m_multi, from, target);
    Repoint(m_selected, from, target);
    Repoint(m_anchor, from, target);
    Repoint(m_pendingPick, from, target);
    // Обложка старого имени больше не нужна (буфер — тоже).
    if (auto it = m_thumbs.find(from.string()); it != m_thumbs.end()) {
        m_preview.ReleaseTarget(from.string() + "@" + std::to_string(it->second.Px));
        m_thumbs.erase(it);
    }
    sage::editor::fscache::Invalidate(from.parent_path());
    m_itemsTime = -1.0;   // пересобрать список в этом же кадре
    sage::AssetDatabase::Instance().ScanProject(host.CurrentProject().Dir().string());
    return true;
}

void AssetsPanel::ApplyDelete(EditorHost& host, const std::vector<fs::path>& victims) {
    auto& database = sage::AssetDatabase::Instance();
    std::vector<fs::path> deleted;
    for (const fs::path& victim : victims) {
        const auto ref = host.CurrentProject().AssetRef(victim);
        const auto isVictim = [&](const std::string& value) {
            return value == ref || value.rfind(ref + "/", 0) == 0 ||
                   value == ref + ".sagesheet" || value == ref + ".sage9";
        };
        bool used = false;
        for (const auto& record : database.All()) {
            if (!isVictim(record.Path)) continue;
            for (const auto& dependent : database.Dependents(record.Guid))
                if (!isVictim(database.PathOf(dependent))) used = true;
        }
        if (used) {
            host.SetStatusMessage("Cannot delete asset while it has dependents: " + ref);
            continue;
        }
        std::string error;
        if (!DeleteAsset(victim, error)) {
            host.SetStatusMessage("Asset delete failed: " + error);
            continue;
        }
        deleted.push_back(victim);
        Repoint(m_multi, victim, {});
        Repoint(m_selected, victim, {});
        Repoint(m_anchor, victim, {});
        Repoint(m_pendingPick, victim, {});
        if (auto it = m_thumbs.find(victim.string()); it != m_thumbs.end()) {
            m_preview.ReleaseTarget(victim.string() + "@" + std::to_string(it->second.Px));
            m_thumbs.erase(it);
        }
        sage::editor::fscache::Invalidate(victim.parent_path());
    }
    m_itemsTime = -1.0;
    if (!deleted.empty()) host.AssetsDeleted(deleted);
}
