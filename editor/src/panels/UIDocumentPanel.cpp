// ---------------------------------------------------------------------------
// Редактор документа интерфейса. Смысл и границы — в UIDocumentPanel.h.
//
// Правило, которому подчинён весь файл: редактор НИЧЕГО не вычисляет сам.
// Положение элемента он берёт у раскладки движка, оформление показывает по
// таблицам свойств компонентов, документ сохраняет и читает тем же кодом, что
// и игра. Всё, что здесь есть своего, — мышь, клавиши и расположение панелей.
// ---------------------------------------------------------------------------
#include "UIDocumentPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../Localization.h"
#include "../Project.h"
#include "sage/core/Application.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/ui/UIRenderer.h"
#include "sage/ui/showcase/UIShowcaseDocument.h"

using namespace sage::editor;
namespace ui = sage::ui;

namespace {

// Разрешение, в котором интерфейс увидит игрок. Из настроек ИГРЫ, а не из
// размера панели: под него считаются якоря, растяжения и проценты, и верстать в
// размер окна редактора значило бы верстать под экран, которого у игрока нет.
void GameFrameSize(EditorHost& host, int& outW, int& outH) {
    const sage::EngineConfig& cfg = host.Settings();
    outW = std::max(64, cfg.Width);
    outH = std::max(64, cfg.Height);
}

ImU32 Col(float r, float g, float b, float a) {
    return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), (int)(a * 255));
}

const char* CategoryName(ui::UIComponentCategory c) {
    return ui::UIComponentCategoryNames()[(int)c];
}

} // namespace

UIDocumentPanel::UIDocumentPanel() {
    m_res.Install(m_rt.Context());
    NewDocument();
}

UIDocumentPanel::~UIDocumentPanel() = default;

// ============================================================================
//  Документ
// ============================================================================

void UIDocumentPanel::NewDocument() {
    ui::UIBuildShowcase(m_rt.Doc(), m_rt.Theme());
    // Новый документ начинается с витрины возможностей, а не с пустого экрана:
    // пустой холст не отвечает ни на один вопрос о том, что система умеет, а
    // разобрать готовое проще, чем собрать с нуля.
    m_path.clear();
    m_dirty = false;
    m_selection.clear();
    m_primary = ui::kUIInvalidNode;
    m_undo.clear();
    m_redo.clear();
    m_fitOnce = true;
}

bool UIDocumentPanel::Open(const std::string& path) {
    const ui::UILoadReport report = ui::UILoadDocument(m_rt.Doc(), path, &m_rt.Theme());
    if (!report.Ok) return false;
    m_path = path;
    m_dirty = false;
    m_selection.clear();
    m_primary = ui::kUIInvalidNode;
    m_undo.clear();
    m_redo.clear();
    m_fitOnce = true;
    return true;
}

bool UIDocumentPanel::Save(const std::string& path) {
    if (!ui::UISaveDocument(m_rt.Doc(), path, &m_rt.Theme())) return false;
    m_path = path;
    m_dirty = false;
    return true;
}

void UIDocumentPanel::PushUndo() {
    // Снимок ДО правки. Глубина ограничена: документ интерфейса невелик, но
    // держать бесконечную историю в памяти незачем.
    m_undo.push_back(ui::UISaveDocumentToString(m_rt.Doc(), &m_rt.Theme()));
    if (m_undo.size() > 64) m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_dirty = true;
}

void UIDocumentPanel::Undo() {
    if (m_undo.empty()) return;
    m_redo.push_back(ui::UISaveDocumentToString(m_rt.Doc(), &m_rt.Theme()));
    ui::UILoadDocumentFromString(m_rt.Doc(), m_undo.back(), &m_rt.Theme());
    m_undo.pop_back();
    m_selection.clear();
    m_primary = ui::kUIInvalidNode;
    m_dirty = true;
}

void UIDocumentPanel::Redo() {
    if (m_redo.empty()) return;
    m_undo.push_back(ui::UISaveDocumentToString(m_rt.Doc(), &m_rt.Theme()));
    ui::UILoadDocumentFromString(m_rt.Doc(), m_redo.back(), &m_rt.Theme());
    m_redo.pop_back();
    m_selection.clear();
    m_primary = ui::kUIInvalidNode;
    m_dirty = true;
}

void UIDocumentPanel::Select(ui::UINodeId id, bool additive) {
    if (!additive) {
        m_selection.clear();
        if (id != ui::kUIInvalidNode) m_selection.push_back(id);
        m_primary = id;
        return;
    }
    auto it = std::find(m_selection.begin(), m_selection.end(), id);
    if (it != m_selection.end()) {
        m_selection.erase(it);
        m_primary = m_selection.empty() ? ui::kUIInvalidNode : m_selection.back();
    } else {
        m_selection.push_back(id);
        m_primary = id;
    }
}

bool UIDocumentPanel::IsSelected(ui::UINodeId id) const {
    return std::find(m_selection.begin(), m_selection.end(), id) != m_selection.end();
}

void UIDocumentPanel::DeleteSelected() {
    if (m_selection.empty()) return;
    PushUndo();
    for (ui::UINodeId id : m_selection) m_rt.Doc().Destroy(id);
    m_selection.clear();
    m_primary = ui::kUIInvalidNode;
}

void UIDocumentPanel::DuplicateSelected() {
    if (m_selection.empty()) return;
    PushUndo();
    std::vector<ui::UINodeId> copies;
    for (ui::UINodeId id : m_selection) {
        const ui::UINodeId copy = m_rt.Doc().Duplicate(id);
        if (copy != ui::kUIInvalidNode) copies.push_back(copy);
    }
    m_selection = copies;
    m_primary = copies.empty() ? ui::kUIInvalidNode : copies.back();
}

// ============================================================================
//  Пересчёт кадра
// ============================================================================

void UIDocumentPanel::Recompute(int frameW, int frameH) {
    m_rt.SetScreen({(float)frameW, (float)frameH});
    m_rt.Context().Debug = m_debug;
    // Тот же шаг, что делает игра каждый кадр. Никакого «редакторского»
    // расчёта: раскладку считает движок, редактор только смотрит результат.
    m_rt.Update(ImGui::GetIO().DeltaTime);
}

sage::ui::UIRect UIDocumentPanel::SelectionBounds() const {
    ui::UIRect box{};
    for (ui::UINodeId id : m_selection) {
        ui::UIRect r{};
        if (m_rt.Layout().RectOf(id, r)) box = ui::UIUnionRect(box, r);
    }
    return box;
}

void UIDocumentPanel::ApplyRect(ui::UINodeId id, glm::vec2 topLeft, glm::vec2 size,
                                bool sizeChanged) {
    ui::UINode* node = m_rt.Doc().Find(id);
    if (!node) return;
    ui::UITransform* t = node->Get<ui::UITransform>();
    const ui::UIResolvedNode* r = m_rt.Layout().Get(id);
    if (!t || !r) return;

    // Прямоугольник родителя — в ЛОГИЧЕСКИХ единицах: Offset и Size хранятся в
    // них, и обратный ход обязан идти через тот же масштаб, каким считали.
    const float scale = r->Scale > 0.0001f ? r->Scale : 1.0f;
    ui::UIRect parent = m_rt.Layout().Viewport();
    if (r->ParentIndex >= 0) {
        const ui::UIRect& pr = m_rt.Layout().Nodes()[(size_t)r->ParentIndex].Rect;
        parent = ui::UIRect{pr.x / scale, pr.y / scale, pr.w / scale, pr.h / scale};
    }
    const glm::vec2 logicalTopLeft = topLeft / scale;
    const glm::vec2 logicalSize = size / scale;

    if (sizeChanged) {
        // Размер записывается только по тем осям, которые задаются числом:
        // тянуть мышью растянутую или подогнанную под содержимое ось значило бы
        // менять число, которое всё равно ни на что не влияет.
        if (t->WidthMode == ui::UISizeMode::Fixed) t->Size.x = logicalSize.x;
        if (t->HeightMode == ui::UISizeMode::Fixed) t->Size.y = logicalSize.y;
    }
    t->Offset = ui::UIOffsetForTopLeft(*t, logicalTopLeft, logicalSize, parent);
    m_rt.Doc().MarkDirty(ui::UIDirty_Layout);
    m_dirty = true;
}

void UIDocumentPanel::MoveSelection(glm::vec2 deltaPixels) {
    for (ui::UINodeId id : m_selection) {
        ui::UIRect r{};
        if (!m_rt.Layout().RectOf(id, r)) continue;
        ApplyRect(id, glm::vec2(r.x, r.y) + deltaPixels, glm::vec2(r.w, r.h), false);
    }
}

// ============================================================================
//  Верхняя строка
// ============================================================================

void UIDocumentPanel::DrawCreateMenu(EditorHost& host) {
    (void)host;
    if (!ImGui::BeginMenu(T("Create"))) return;

    // Меню собирается ПО РЕЕСТРУ виджетов: свой виджет игры или плагина
    // появляется здесь сам, без единой правки в редакторе (§96).
    std::vector<std::string> categories;
    for (const ui::UIWidgetType& w : ui::UIWidgetRegistry::Instance().All()) {
        if (std::find(categories.begin(), categories.end(), w.Category) == categories.end())
            categories.push_back(w.Category);
    }
    for (const std::string& category : categories) {
        if (!ImGui::BeginMenu(T(category))) continue;
        for (const ui::UIWidgetType& w : ui::UIWidgetRegistry::Instance().All()) {
            if (w.Category != category) continue;
            if (ImGui::MenuItem(T(w.Title))) {
                PushUndo();
                // Новый узел становится ребёнком выделенного: интерфейс
                // собирается из вложенных прямоугольников, и это самый частый шаг.
                const ui::UINodeId created = m_rt.Doc().CreateWidget(w.Id, m_primary);
                if (created != ui::kUIInvalidNode) Select(created, false);
            }
            if (ImGui::IsItemHovered() && !w.Hint.empty())
                ImGui::SetTooltip("%s", T(w.Hint));
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

void UIDocumentPanel::DrawToolbar(EditorHost& host) {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu(T("Document"))) {
            if (ImGui::MenuItem(T("New"))) NewDocument();
            if (ImGui::MenuItem(T("Open..."))) {
                FileBrowser::Config cfg;
                cfg.Title = T("Open UI document");
                cfg.Mode = FileBrowser::PickMode::OpenFile;
                cfg.StartDir = host.CurrentProject().AssetsDir();
                cfg.Filters = {ui::kUIDocumentExt, ui::kUIPrefabExt};
                cfg.FilterLabel = T("UI documents (*.uidoc)");
                m_browser.Open(cfg);
                m_browseIsSave = false;
            }
            if (ImGui::MenuItem(T("Save"), "Ctrl+S", false, !m_path.empty())) Save(m_path);
            if (ImGui::MenuItem(T("Save as..."))) {
                FileBrowser::Config cfg;
                cfg.Title = T("Save UI document");
                cfg.Mode = FileBrowser::PickMode::SaveFile;
                cfg.StartDir = host.CurrentProject().AssetsDir();
                cfg.DefaultName = std::string("interface") + ui::kUIDocumentExt;
                cfg.Filters = {ui::kUIDocumentExt};
                m_browser.Open(cfg);
                m_browseIsSave = true;
            }
            ImGui::EndMenu();
        }
        DrawCreateMenu(host);
        if (ImGui::BeginMenu(T("View"))) {
            // Отладочные слои — то, что отвечает на «почему так» (§109).
            ImGui::CheckboxFlags(T("Show bounds"), &m_debug, ui::UIDebug_Bounds);
            ImGui::CheckboxFlags(T("Show anchors"), &m_debug, ui::UIDebug_Anchors);
            ImGui::CheckboxFlags(T("Show pivots"), &m_debug, ui::UIDebug_Pivots);
            ImGui::CheckboxFlags(T("Show masks"), &m_debug, ui::UIDebug_Masks);
            ImGui::CheckboxFlags(T("Show clip regions"), &m_debug, ui::UIDebug_Clip);
            ImGui::CheckboxFlags(T("Show hit areas"), &m_debug, ui::UIDebug_HitAreas);
            ImGui::CheckboxFlags(T("Show layers"), &m_debug, ui::UIDebug_Layers);
            ImGui::CheckboxFlags(T("Show render batches"), &m_debug, ui::UIDebug_Batches);
            ImGui::Separator();
            ImGui::Checkbox(T("Grid"), &m_showGrid);
            ImGui::Checkbox(T("Snap to grid"), &m_snapToGrid);
            ImGui::SetNextItemWidth(100.0f);
            ImGui::DragFloat(T("Grid cell"), &m_gridStep, 1.0f, 1.0f, 256.0f, "%.0f");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImGui::SetNextItemWidth(100.0f);
    float zoomPercent = m_zoom * 100.0f;
    if (ImGui::DragFloat("##zoom", &zoomPercent, 1.0f, 10.0f, 400.0f, "%.0f%%"))
        m_zoom = std::clamp(zoomPercent / 100.0f, 0.10f, 4.0f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Canvas zoom (wheel over the canvas)"));
    ImGui::SameLine();
    if (ImGui::SmallButton(T("1:1"))) { m_zoom = 1.0f; m_pan = ImVec2(0, 0); }
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Fit"))) { m_fitOnce = true; m_pan = ImVec2(0, 0); }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderFloat(T("Backdrop"), &m_backdrop, 0.0f, 1.0f, "%.2f");

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    DrawAlignTools();

    // Справа — стоимость кадра. Не украшение: разработчик, включивший размытие
    // на десяти панелях, обязан увидеть десять проходов, а не гадать (§113).
    const ui::UIProfile& profile = m_rt.Profile();
    char cost[160];
    std::snprintf(cost, sizeof(cost), "%s %d · %s %d · %s %d", T("nodes"), profile.Layout.Visible,
                  T("batches"), profile.Render.Batches, T("targets"),
                  profile.Render.RenderTargets);
    const float w = ImGui::CalcTextSize(cost).x + 24.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8.0f, ImGui::GetWindowWidth() - w));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", cost);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m_rt.Profile().Summary().c_str());
}

void UIDocumentPanel::DrawAlignTools() {
    const bool enabled = m_selection.size() >= 2;
    ImGui::BeginDisabled(!enabled);
    struct Action { const char* icon; const char* tip; int mode; };
    static const Action kActions[] = {
        {"align", SAGE_UI_TEXT("Align left"), 0},   {"align", SAGE_UI_TEXT("Align center"), 1},
        {"align", SAGE_UI_TEXT("Align right"), 2},  {"align", SAGE_UI_TEXT("Align top"), 3},
        {"align", SAGE_UI_TEXT("Align middle"), 4}, {"align", SAGE_UI_TEXT("Align bottom"), 5},
        {"align", SAGE_UI_TEXT("Distribute horizontally"), 6},
        {"align", SAGE_UI_TEXT("Distribute vertically"), 7},
    };
    for (const Action& a : kActions) {
        ImGui::PushID(a.mode);
        const bool pressed = ImGui::SmallButton(T(a.tip));
        ImGui::PopID();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(a.tip));
        ImGui::SameLine();
        if (!pressed) continue;

        PushUndo();
        const ui::UIRect box = SelectionBounds();
        std::vector<std::pair<ui::UINodeId, ui::UIRect>> items;
        for (ui::UINodeId id : m_selection) {
            ui::UIRect r{};
            if (m_rt.Layout().RectOf(id, r)) items.emplace_back(id, r);
        }
        if (a.mode <= 5) {
            for (auto& [id, r] : items) {
                glm::vec2 pos(r.x, r.y);
                switch (a.mode) {
                    case 0: pos.x = box.x; break;
                    case 1: pos.x = box.x + (box.w - r.w) * 0.5f; break;
                    case 2: pos.x = ui::UIRight(box) - r.w; break;
                    case 3: pos.y = box.y; break;
                    case 4: pos.y = box.y + (box.h - r.h) * 0.5f; break;
                    default: pos.y = ui::UIBottom(box) - r.h; break;
                }
                ApplyRect(id, pos, glm::vec2(r.w, r.h), false);
            }
        } else {
            // Распределение: равные ПРОМЕЖУТКИ между элементами, а не равный
            // шаг между их левыми краями — иначе элементы разной ширины стоят
            // визуально неровно.
            const bool horizontal = a.mode == 6;
            std::sort(items.begin(), items.end(), [&](const auto& x, const auto& y) {
                return horizontal ? x.second.x < y.second.x : x.second.y < y.second.y;
            });
            float used = 0.0f;
            for (const auto& [id, r] : items) used += horizontal ? r.w : r.h;
            const float span = horizontal ? box.w : box.h;
            const float gap = items.size() > 1 ? (span - used) / (float)(items.size() - 1) : 0.0f;
            float cursor = horizontal ? box.x : box.y;
            for (auto& [id, r] : items) {
                glm::vec2 pos(r.x, r.y);
                (horizontal ? pos.x : pos.y) = cursor;
                cursor += (horizontal ? r.w : r.h) + gap;
                ApplyRect(id, pos, glm::vec2(r.w, r.h), false);
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::NewLine();
}

// ============================================================================
//  Дерево
// ============================================================================

void UIDocumentPanel::DrawTreeNode(EditorHost& host, ui::UINodeId id, int depth) {
    ui::UINode* node = m_rt.Doc().Find(id);
    if (!node) return;

    ImGui::PushID((int)id);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;
    if (node->Children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (IsSelected(id)) flags |= ImGuiTreeNodeFlags_Selected;

    // Глаз и замок — слева от имени и всегда на месте: их ищут глазами, и
    // «плавающая» кнопка каждый раз заставляет искать заново.
    const float lineStart = ImGui::GetCursorPosX();
    if (ImGui::SmallButton(node->Visible ? "o" : "-")) {
        PushUndo();
        node->Visible = !node->Visible;
        m_rt.Doc().MarkDirty(ui::UIDirty_Visual);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Hide or show"));
    ImGui::SameLine();
    if (ImGui::SmallButton(node->Locked ? "L" : ".")) {
        node->Locked = !node->Locked;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Lock: not selectable on the canvas"));
    ImGui::SameLine();
    ImGui::SetCursorPosX(lineStart + 56.0f);

    bool open;
    if (m_renaming == id) {
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##rename", m_renameBuf, sizeof(m_renameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            PushUndo();
            node->Name = m_renameBuf;
            m_renaming = ui::kUIInvalidNode;
        }
        if (ImGui::IsItemDeactivated()) m_renaming = ui::kUIInvalidNode;
        open = !node->Children.empty();
    } else {
        open = ImGui::TreeNodeEx("##node", flags, "%s", node->Name.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            Select(id, ImGui::GetIO().KeyCtrl);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_renaming = id;
            std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", node->Name.c_str());
        }

        // Перетаскивание в дереве: и вложение, и перестановка. Один и тот же
        // источник, разные цели — иначе пришлось бы заводить два способа
        // тащить одно и то же.
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("SAGE_UI_NODE", &id, sizeof(id));
            ImGui::TextUnformatted(node->Name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_UI_NODE")) {
                const ui::UINodeId dragged = *(const ui::UINodeId*)p->Data;
                PushUndo();
                // Отказ вместо молчаливого «как-нибудь»: перенос внутрь
                // собственного поддерева — это цикл, а не странная вёрстка.
                if (!m_rt.Doc().Reparent(dragged, id))
                    host.SetStatusMessage(T("Cannot nest a node inside itself"));
            }
            ImGui::EndDragDropTarget();
        }

        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem(T("Rename"))) {
                m_renaming = id;
                std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", node->Name.c_str());
            }
            if (ImGui::MenuItem(T("Duplicate"))) { Select(id, false); DuplicateSelected(); }
            if (ImGui::MenuItem(T("Delete"))) { Select(id, false); DeleteSelected(); }
            ImGui::Separator();
            if (ImGui::MenuItem(T("Move up"))) {
                PushUndo();
                ui::UINode* parent = m_rt.Doc().Find(node->Parent);
                const std::vector<ui::UINodeId>& list =
                    parent ? parent->Children : m_rt.Doc().Roots();
                for (int i = 0; i < (int)list.size(); ++i)
                    if (list[(size_t)i] == id && i > 0) { m_rt.Doc().Move(id, i - 1); break; }
            }
            if (ImGui::MenuItem(T("Move down"))) {
                PushUndo();
                ui::UINode* parent = m_rt.Doc().Find(node->Parent);
                const std::vector<ui::UINodeId>& list =
                    parent ? parent->Children : m_rt.Doc().Roots();
                for (int i = 0; i < (int)list.size(); ++i)
                    if (list[(size_t)i] == id && i + 1 < (int)list.size()) {
                        m_rt.Doc().Move(id, i + 1);
                        break;
                    }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(T("Save as prefab..."))) {
                FileBrowser::Config cfg;
                cfg.Title = T("Save UI prefab");
                cfg.Mode = FileBrowser::PickMode::SaveFile;
                cfg.StartDir = host.CurrentProject().AssetsDir();
                cfg.DefaultName = node->Name + ui::kUIPrefabExt;
                cfg.Filters = {ui::kUIPrefabExt};
                m_browser.Open(cfg);
                m_browseIsSave = true;
                Select(id, false);
            }
            if (node->Get<ui::UIPrefabInstance>()) {
                if (ImGui::MenuItem(T("Capture prefab overrides"))) {
                    PushUndo();
                    ui::UICapturePrefabOverrides(m_rt.Doc(), id);
                }
                if (ImGui::MenuItem(T("Unpack prefab"))) {
                    PushUndo();
                    ui::UIUnpackPrefab(m_rt.Doc(), id);
                }
            }
            ImGui::EndPopup();
        }
    }

    // Слой справа: колонка «слой/порядок» — то, ради чего §70 просит отдельный
    // вид. Отдельного окна для неё не нужно, если она видна прямо здесь.
    if (node->Layer != 0 || node->Order != 0) {
        char tag[32];
        std::snprintf(tag, sizeof(tag), "%d.%d", node->Layer, node->Order);
        const float x = ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(tag).x - 4.0f;
        ImGui::SameLine(x);
        ImGui::TextDisabled("%s", tag);
    }

    if (open && !node->Children.empty()) {
        const std::vector<ui::UINodeId> children = node->Children;
        for (ui::UINodeId child : children) DrawTreeNode(host, child, depth + 1);
        if (m_renaming != id) ImGui::TreePop();
    } else if (open && m_renaming != id) {
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void UIDocumentPanel::DrawHierarchy(EditorHost& host, float width) {
    ImGui::BeginChild("##ui_tree", ImVec2(width, 0), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("%s", T("Hierarchy"));
    ImGui::Separator();

    const std::vector<ui::UINodeId> roots = m_rt.Doc().Roots();
    for (ui::UINodeId id : roots) DrawTreeNode(host, id, 0);

    // Бросок на пустое место дерева — вынести узел в корень. Иначе вложенный
    // узел невозможно достать наружу иначе как через контекстное меню.
    ImGui::Dummy(ImVec2(-1.0f, std::max(24.0f, ImGui::GetContentRegionAvail().y)));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("SAGE_UI_NODE")) {
            const ui::UINodeId dragged = *(const ui::UINodeId*)p->Data;
            PushUndo();
            m_rt.Doc().Reparent(dragged, ui::kUIInvalidNode);
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();
}

// ============================================================================
//  Холст
// ============================================================================

void UIDocumentPanel::DrawCanvas(EditorHost& host) {
    int gw = 0, gh = 0;
    GameFrameSize(host, gw, gh);
    Recompute(gw, gh);

    ImGui::BeginChild("##ui_canvas", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    if (avail.x < 32.0f || avail.y < 32.0f) { ImGui::EndChild(); return; }

    if (m_fitOnce) {
        m_zoom = std::clamp(std::min(avail.x / (float)gw, avail.y / (float)gh) * 0.92f, 0.10f,
                            4.0f);
        m_fitOnce = false;
    }
    const ImVec2 imgSize((float)gw * m_zoom, (float)gh * m_zoom);
    const ImVec2 imgPos(origin.x + (avail.x - imgSize.x) * 0.5f + m_pan.x,
                        origin.y + (avail.y - imgSize.y) * 0.5f + m_pan.y);

    // --- сам кадр: рисуется РАНТАЙМОМ, а не редактором --------------------
    if (!m_renderer) m_renderer = std::make_unique<UIRenderer>();
    if (!m_target) m_target = std::make_unique<Framebuffer>(gw, gh);
    m_target->Resize(gw, gh);
    m_target->Bind();
    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();
    device.SetClearColor(0.07f, 0.08f, 0.10f, 1.0f);
    device.Clear();
    {
        ui::UIClassicBackend backend(*m_renderer);
        // Куда возвращаться после промежуточного прохода: без этого остаток
        // кадра после первого же размытия уехал бы на экран мимо панели.
        backend.SetRootTarget(m_target.get());
        m_rt.Render(backend);
    }
    device.BindDefaultFramebuffer();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y),
                      IM_COL32(24, 26, 30, 255));
    ImGui::SetCursorScreenPos(imgPos);
    ImGui::Image((ImTextureID)(std::intptr_t)m_target->NativeColorTexture(), imgSize,
                 ImVec2(0, 1), ImVec2(1, 0));
    const bool hovered = ImGui::IsItemHovered();
    if (m_backdrop < 0.999f) {
        dl->AddRectFilled(imgPos, ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y),
                          IM_COL32(26, 28, 33, (int)((1.0f - m_backdrop) * 160.0f)));
    }
    dl->AddRect(imgPos, ImVec2(imgPos.x + imgSize.x, imgPos.y + imgSize.y),
                IM_COL32(110, 120, 140, 200));

    auto toScreen = [&](glm::vec2 p) {
        return ImVec2(imgPos.x + p.x * m_zoom, imgPos.y + p.y * m_zoom);
    };
    auto toFrame = [&](ImVec2 p) {
        return glm::vec2((p.x - imgPos.x) / m_zoom, (p.y - imgPos.y) / m_zoom);
    };

    if (m_showGrid && m_gridStep * m_zoom > 4.0f) {
        for (float x = 0.0f; x <= (float)gw; x += m_gridStep) {
            const ImVec2 a = toScreen({x, 0.0f});
            dl->AddLine(a, ImVec2(a.x, imgPos.y + imgSize.y), IM_COL32(255, 255, 255, 12));
        }
        for (float y = 0.0f; y <= (float)gh; y += m_gridStep) {
            const ImVec2 a = toScreen({0.0f, y});
            dl->AddLine(a, ImVec2(imgPos.x + imgSize.x, a.y), IM_COL32(255, 255, 255, 12));
        }
    }

    // --- рамки: ИЗ РАСКЛАДКИ, а не из своей копии формул --------------------
    for (const ui::UIResolvedNode& r : m_rt.Layout().Nodes()) {
        const ui::UINode* node = m_rt.Doc().Find(r.Id);
        if (!node) continue;
        const bool selected = IsSelected(r.Id);
        if (!selected && !(m_debug & ui::UIDebug_Bounds)) continue;
        const ImVec2 a = toScreen({r.Rect.x, r.Rect.y});
        const ImVec2 b = toScreen({ui::UIRight(r.Rect), ui::UIBottom(r.Rect)});
        dl->AddRect(a, b, selected ? Col(0.95f, 0.76f, 0.20f, 1.0f) : Col(0.4f, 0.7f, 1.0f, 0.35f),
                    0.0f, 0, selected ? 2.0f : 1.0f);
    }

    // Ручки изменения размера — только у одиночного выделения: у набора «за
    // какой угол тянем» не определено, и попытка додумать это даёт неожиданный
    // результат чаще, чем полезный.
    ui::UIRect primaryRect{};
    const bool hasPrimary = m_primary != ui::kUIInvalidNode &&
                            m_rt.Layout().RectOf(m_primary, primaryRect);
    struct Handle { ImVec2 pos; Drag mode; };
    std::vector<Handle> handles;
    if (hasPrimary && m_selection.size() == 1) {
        const float x0 = primaryRect.x, y0 = primaryRect.y;
        const float x1 = ui::UIRight(primaryRect), y1 = ui::UIBottom(primaryRect);
        const float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
        handles = {{toScreen({x0, y0}), Drag::NW}, {toScreen({mx, y0}), Drag::N},
                   {toScreen({x1, y0}), Drag::NE}, {toScreen({x1, my}), Drag::E},
                   {toScreen({x1, y1}), Drag::SE}, {toScreen({mx, y1}), Drag::S},
                   {toScreen({x0, y1}), Drag::SW}, {toScreen({x0, my}), Drag::W}};
        for (const Handle& h : handles) {
            dl->AddRectFilled(ImVec2(h.pos.x - 4, h.pos.y - 4), ImVec2(h.pos.x + 4, h.pos.y + 4),
                              Col(0.95f, 0.76f, 0.20f, 1.0f));
        }
    }

    // --- мышь ---------------------------------------------------------------
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        Drag grabbed = Drag::None;
        for (const Handle& h : handles) {
            if (std::fabs(mouse.x - h.pos.x) <= 5.0f && std::fabs(mouse.y - h.pos.y) <= 5.0f) {
                grabbed = h.mode;
                break;
            }
        }
        if (grabbed == Drag::None) {
            // Выбор кликом идёт через тот же поиск попадания, что и в игре, —
            // с одной поправкой: в редакторе выбирается ЛЮБОЙ узел, а не только
            // принимающий ввод, иначе панель без взаимодействия не выделить.
            const glm::vec2 point = toFrame(mouse);
            ui::UINodeId picked = ui::kUIInvalidNode;
            uint64_t bestKey = 0;
            for (const ui::UIResolvedNode& r : m_rt.Layout().Nodes()) {
                const ui::UINode* node = m_rt.Doc().Find(r.Id);
                if (!node || node->Locked || !r.Visible) continue;
                if (!ui::UIContains(r.Rect, point)) continue;
                if (r.Clipped && !ui::UIContains(r.Clip, point)) continue;
                if (picked == ui::kUIInvalidNode || r.SortKey >= bestKey) {
                    picked = r.Id;
                    bestKey = r.SortKey;
                }
            }
            Select(picked, ImGui::GetIO().KeyCtrl);
            if (picked != ui::kUIInvalidNode) grabbed = Drag::Move;
        }
        m_drag = grabbed;
        m_dragStartMouse = mouse;
        m_dragPushedUndo = false;
        m_dragStart.clear();
        for (ui::UINodeId id : m_selection) {
            ui::UIRect r{};
            if (m_rt.Layout().RectOf(id, r)) m_dragStart.emplace_back(id, r);
        }
        m_dragStartUnion = SelectionBounds();
    }

    if (m_drag != Drag::None && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        glm::vec2 delta((mouse.x - m_dragStartMouse.x) / m_zoom,
                        (mouse.y - m_dragStartMouse.y) / m_zoom);
        if (m_snapToGrid && m_gridStep > 0.0f) {
            // Притягивается ИТОГОВОЕ положение, а не смещение: иначе элемент,
            // стоящий не по сетке, так по ней и не встанет.
            const glm::vec2 target(m_dragStartUnion.x + delta.x, m_dragStartUnion.y + delta.y);
            const glm::vec2 snapped(std::round(target.x / m_gridStep) * m_gridStep,
                                    std::round(target.y / m_gridStep) * m_gridStep);
            delta += snapped - target;
        }
        if (delta.x != 0.0f || delta.y != 0.0f) {
            if (!m_dragPushedUndo) { PushUndo(); m_dragPushedUndo = true; }
            for (const auto& [id, start] : m_dragStart) {
                glm::vec2 pos(start.x, start.y);
                glm::vec2 size(start.w, start.h);
                switch (m_drag) {
                    case Drag::Move: pos += delta; break;
                    case Drag::N: pos.y += delta.y; size.y -= delta.y; break;
                    case Drag::S: size.y += delta.y; break;
                    case Drag::W: pos.x += delta.x; size.x -= delta.x; break;
                    case Drag::E: size.x += delta.x; break;
                    case Drag::NW: pos += delta; size -= delta; break;
                    case Drag::NE: pos.y += delta.y; size.x += delta.x; size.y -= delta.y; break;
                    case Drag::SW: pos.x += delta.x; size.x -= delta.x; size.y += delta.y; break;
                    case Drag::SE: size += delta; break;
                    default: break;
                }
                size.x = std::max(1.0f, size.x);
                size.y = std::max(1.0f, size.y);
                ApplyRect(id, pos, size, m_drag != Drag::Move);
            }
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_drag = Drag::None;

    // Колесо — масштаб показа, средняя кнопка — панорамирование.
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f && m_drag == Drag::None)
        m_zoom = std::clamp(m_zoom * (1.0f + ImGui::GetIO().MouseWheel * 0.1f), 0.1f, 4.0f);
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        m_pan.x += d.x;
        m_pan.y += d.y;
    }

    // Стрелки: сдвиг на пиксель, с Shift — на шаг сетки. Точная доводка мышью
    // невозможна в принципе.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !m_selection.empty()) {
        const float step = ImGui::GetIO().KeyShift ? m_gridStep : 1.0f;
        glm::vec2 nudge(0.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) nudge.x -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) nudge.x += step;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) nudge.y -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) nudge.y += step;
        if (nudge.x != 0.0f || nudge.y != 0.0f) { PushUndo(); MoveSelection(nudge); }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) DeleteSelected();
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) DuplicateSelected();
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) Undo();
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) Redo();
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S) && !m_path.empty())
            Save(m_path);
    }

    ImGui::EndChild();
}

// ============================================================================
//  Инспектор
// ============================================================================

bool UIDocumentPanel::DrawProperty(void* data, const ui::UIProperty& p, const char* idSuffix) {
    // Виджет выбирается ПО ТИПУ и по подсказке из таблицы. Никакого списка
    // «какое поле каким виджетом» в редакторе нет: он немедленно разошёлся бы с
    // компонентами (§64, §72).
    char label[192];
    std::snprintf(label, sizeof(label), "%s##%s%s", T(p.Label ? p.Label : p.Key), p.Key,
                  idSuffix);
    bool changed = false;

    switch (p.Type) {
        case ui::UIProperty::Kind::Bool:
            changed = ImGui::Checkbox(label, &ui::UIFieldAs<bool>(data, p));
            break;
        case ui::UIProperty::Kind::Int: {
            int& v = ui::UIFieldAs<int>(data, p);
            changed = p.HasRange() ? ImGui::SliderInt(label, &v, (int)p.Min, (int)p.Max)
                                   : ImGui::DragInt(label, &v);
            break;
        }
        case ui::UIProperty::Kind::Float: {
            float& v = ui::UIFieldAs<float>(data, p);
            if (p.Editor == ui::UIProperty::Widget::Slider && p.HasRange())
                changed = ImGui::SliderFloat(label, &v, p.Min, p.Max, "%.2f");
            else if (p.Editor == ui::UIProperty::Widget::Angle)
                changed = ImGui::SliderFloat(label, &v, p.Min, p.Max, "%.1f°");
            else
                changed = ImGui::DragFloat(label, &v, 0.5f, p.HasRange() ? p.Min : 0.0f,
                                           p.HasRange() ? p.Max : 0.0f, "%.2f");
            break;
        }
        case ui::UIProperty::Kind::String: {
            std::string& s = ui::UIFieldAs<std::string>(data, p);
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%s", s.c_str());
            if (p.Editor == ui::UIProperty::Widget::Multiline) {
                if (ImGui::InputTextMultiline(label, buf, sizeof(buf), ImVec2(-1.0f, 64.0f))) {
                    s = buf;
                    changed = true;
                }
            } else {
                if (ImGui::InputText(label, buf, sizeof(buf))) { s = buf; changed = true; }
                if (p.Editor == ui::UIProperty::Widget::Texture) {
                    // Приём броска из панели ассетов — единственный способ
                    // назначить картинку, не переписывая путь руками.
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                            s.assign((const char*)pl->Data, (size_t)pl->DataSize);
                            if (!s.empty() && s.back() == '\0') s.pop_back();
                            changed = true;
                        }
                        ImGui::EndDragDropTarget();
                    }
                }
            }
            break;
        }
        case ui::UIProperty::Kind::Color:
            changed = ImGui::ColorEdit4(label, &ui::UIFieldAs<glm::vec4>(data, p).x,
                                        ImGuiColorEditFlags_AlphaBar |
                                            ImGuiColorEditFlags_AlphaPreviewHalf);
            break;
        case ui::UIProperty::Kind::Vec2: {
            glm::vec2& v = ui::UIFieldAs<glm::vec2>(data, p);
            changed = ImGui::DragFloat2(label, &v.x, 0.5f, p.HasRange() ? p.Min : 0.0f,
                                        p.HasRange() ? p.Max : 0.0f, "%.2f");
            break;
        }
        case ui::UIProperty::Kind::Vec4: {
            glm::vec4& v = ui::UIFieldAs<glm::vec4>(data, p);
            changed = ImGui::DragFloat4(label, &v.x, 0.5f, p.HasRange() ? p.Min : 0.0f,
                                        p.HasRange() ? p.Max : 0.0f, "%.2f");
            break;
        }
        case ui::UIProperty::Kind::Enum: {
            int& v = ui::UIFieldAs<int>(data, p);
            if (!p.EnumNames || p.EnumCount <= 0) break;
            if (ImGui::BeginCombo(label, T(p.EnumNames[std::clamp(v, 0, p.EnumCount - 1)]))) {
                for (int i = 0; i < p.EnumCount; ++i) {
                    if (ImGui::Selectable(T(p.EnumNames[i]), v == i)) { v = i; changed = true; }
                }
                ImGui::EndCombo();
            }
            break;
        }
        case ui::UIProperty::Kind::Edges: {
            ui::UIEdges& e = ui::UIFieldAs<ui::UIEdges>(data, p);
            float v[4] = {e.L, e.T, e.R, e.B};
            if (ImGui::DragFloat4(label, v, 0.5f, p.HasRange() ? p.Min : 0.0f,
                                  p.HasRange() ? p.Max : 0.0f, "%.1f")) {
                e = ui::UIEdges(v[0], v[1], v[2], v[3]);
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Left, top, right, bottom"));
            break;
        }
        case ui::UIProperty::Kind::Corners: {
            ui::UICorners& c = ui::UIFieldAs<ui::UICorners>(data, p);
            float v[4] = {c.TL, c.TR, c.BR, c.BL};
            if (ImGui::DragFloat4(label, v, 0.5f, p.HasRange() ? p.Min : 0.0f,
                                  p.HasRange() ? p.Max : 0.0f, "%.1f")) {
                c = ui::UICorners(v[0], v[1], v[2], v[3]);
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", T("Top-left, top-right, bottom-right, bottom-left"));
            break;
        }
        case ui::UIProperty::Kind::Gradient: {
            ui::UIGradient& g = ui::UIFieldAs<ui::UIGradient>(data, p);
            if (ImGui::TreeNode(label)) {
                int kind = (int)g.Type;
                if (ImGui::Combo(T("Type##grad"), &kind, "None\0Linear\0Radial\0Angular\0")) {
                    g.Type = (ui::UIGradient::Kind)kind;
                    // Градиент из двух цветов появляется сразу: пустой список
                    // остановок выглядел бы как «включил и ничего не изменилось».
                    if (g.Stops.size() < 2)
                        g.Stops = {{0.0f, ui::UIColor(1.0f)}, {1.0f, ui::UIColor(0, 0, 0, 1)}};
                    changed = true;
                }
                changed |= ImGui::SliderFloat(T("Angle##grad"), &g.Angle, -180.0f, 180.0f, "%.0f°");
                for (size_t i = 0; i < g.Stops.size(); ++i) {
                    ImGui::PushID((int)i);
                    changed |= ImGui::SliderFloat("##pos", &g.Stops[i].Position, 0.0f, 1.0f,
                                                  "%.2f");
                    ImGui::SameLine();
                    changed |= ImGui::ColorEdit4("##col", &g.Stops[i].Color.x,
                                                 ImGuiColorEditFlags_NoInputs |
                                                     ImGuiColorEditFlags_AlphaBar);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("-") && g.Stops.size() > 2) {
                        g.Stops.erase(g.Stops.begin() + (long)i);
                        changed = true;
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
                if (ImGui::SmallButton(T("Add stop"))) {
                    g.Stops.push_back({0.5f, ui::UIColor(1.0f)});
                    changed = true;
                }
                if (changed) g.Sort();
                ImGui::TreePop();
            }
            break;
        }
        default: break;
    }
    if (p.Tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(p.Tooltip));
    return changed;
}

void UIDocumentPanel::DrawComponentEditor(ui::UINode& node, ui::UIComponent& comp) {
    const ui::UIComponentType& type = comp.Type();
    ImGui::PushID(type.Id.c_str());

    const bool open = ImGui::CollapsingHeader(T(type.Title), ImGuiTreeNodeFlags_DefaultOpen);
    if (!type.Essential) {
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 24.0f);
        if (ImGui::SmallButton("x")) {
            PushUndo();
            node.RemoveById(type.Id);
            ImGui::PopID();
            return;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Remove component"));
    }
    if (!open) { ImGui::PopID(); return; }
    if (!type.Hint.empty()) ImGui::TextDisabled("%s", T(type.Hint));

    // Свойства идут группами из таблицы: компонент из двадцати полей без
    // подгрупп нечитаем, а разбивать его на пять компонентов ради вида — врать
    // про модель данных (§71).
    std::string currentGroup;
    bool groupOpen = true;
    for (const ui::UIProperty& p : type.Props) {
        const std::string group = p.Group ? p.Group : "";
        if (group != currentGroup) {
            currentGroup = group;
            groupOpen = group.empty() ? true : ImGui::TreeNodeEx(T(group),
                                                                ImGuiTreeNodeFlags_SpanAvailWidth);
            if (!group.empty() && groupOpen) ImGui::TreePop();
        }
        if (!groupOpen) continue;
        if (DrawProperty(comp.Data(), p, type.Id.c_str())) {
            m_rt.Doc().MarkDirty(ui::UIDirty_All);
            m_dirty = true;
            // Правка вручную защищается от темы: иначе следующее применение
            // стиля молча вернёт всё назад (§59).
            if (ui::UIStyled* styled = node.Get<ui::UIStyled>())
                styled->SetOverride(type.Id + "." + p.Key, true);
        }
        if (ImGui::IsItemActivated()) PushUndo();
    }
    if (type.Id == "effects") DrawEffectsEditor(node);
    ImGui::PopID();
}

void UIDocumentPanel::DrawEffectsEditor(ui::UINode& node) {
    ui::UIEffects* fx = node.Get<ui::UIEffects>();
    if (!fx) return;

    for (size_t i = 0; i < fx->Items.size(); ++i) {
        ui::UIEffect& e = *fx->Items[i];
        ImGui::PushID((int)i);
        ImGui::Checkbox("##on", &e.Enabled);
        ImGui::SameLine();
        const bool open = ImGui::TreeNodeEx("##fx", ImGuiTreeNodeFlags_DefaultOpen, "%s",
                                            T(e.Type().Title));
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 76.0f);
        // Порядок в стеке значим (§37) — значит, переставлять его надо прямо
        // здесь, а не «как-нибудь потом».
        if (ImGui::SmallButton("^")) { PushUndo(); fx->MoveItem((int)i, (int)i - 1); }
        ImGui::SameLine();
        if (ImGui::SmallButton("v")) { PushUndo(); fx->MoveItem((int)i, (int)i + 1); }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            PushUndo();
            fx->Remove((int)i);
            if (open) ImGui::TreePop();
            ImGui::PopID();
            break;
        }
        if (open) {
            for (const ui::UIProperty& p : e.Type().Props) {
                if (DrawProperty(e.Data(), p, e.Type().Id.c_str())) {
                    m_rt.Doc().MarkDirty(ui::UIDirty_Effect | ui::UIDirty_Visual);
                    m_dirty = true;
                }
                if (ImGui::IsItemActivated()) PushUndo();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    if (ImGui::BeginCombo(T("Add effect"), T("Add effect"))) {
        // Список — ИЗ РЕЕСТРА: свой эффект появляется здесь одной регистрацией.
        for (const ui::UIEffectType* t : ui::UIEffectRegistry::Instance().All()) {
            if (ImGui::Selectable(T(t->Title))) {
                PushUndo();
                fx->Add(t->Id);
            }
            if (ImGui::IsItemHovered() && !t->Hint.empty()) ImGui::SetTooltip("%s", T(t->Hint));
        }
        ImGui::EndCombo();
    }
}

void UIDocumentPanel::DrawInspector(EditorHost& host, float width) {
    (void)host;
    ImGui::BeginChild("##ui_inspector", ImVec2(width, 0), ImGuiChildFlags_Borders);
    ui::UINode* node = m_rt.Doc().Find(m_primary);
    if (!node) {
        ImGui::TextDisabled("%s", T("Select a node"));
        ImGui::EndChild();
        return;
    }

    // --- свойства самого узла (наследуемые) ---------------------------------
    char nameBuf[128];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", node->Name.c_str());
    if (ImGui::InputText(T("Name"), nameBuf, sizeof(nameBuf))) { node->Name = nameBuf; m_dirty = true; }
    if (ImGui::IsItemActivated()) PushUndo();

    bool touched = false;
    touched |= ImGui::Checkbox(T("Enabled"), &node->Enabled);
    ImGui::SameLine();
    touched |= ImGui::Checkbox(T("Visible"), &node->Visible);
    ImGui::SameLine();
    touched |= ImGui::Checkbox(T("Locked"), &node->Locked);
    touched |= ImGui::SliderFloat(T("Opacity"), &node->Opacity, 0.0f, 1.0f, "%.2f");
    touched |= ImGui::DragInt(T("Layer"), &node->Layer);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Layer → order → tree position"));
    touched |= ImGui::DragInt(T("Order"), &node->Order);
    int blend = (int)node->Blend;
    if (ImGui::BeginCombo(T("Blend mode"), T(ui::UIBlendModeNames()[blend]))) {
        for (int i = 0; i < ui::UIBlendModeCount(); ++i)
            if (ImGui::Selectable(T(ui::UIBlendModeNames()[i]), blend == i)) {
                node->Blend = (ui::UIBlendMode)i;
                touched = true;
            }
        ImGui::EndCombo();
    }
    if (touched) { m_rt.Doc().MarkDirty(ui::UIDirty_All); m_dirty = true; }

    ImGui::Separator();

    // --- компоненты: только те, которые у узла есть (§72) -------------------
    for (ui::UIComponent* comp : node->DrawOrder()) DrawComponentEditor(*node, *comp);

    ImGui::Separator();
    if (ImGui::BeginCombo(T("Add component"), T("Add component"))) {
        ui::UIComponentCategory lastCategory = ui::UIComponentCategory::Transform;
        bool first = true;
        for (const ui::UIComponentType* t : ui::UIComponentRegistry::Instance().All()) {
            if (t->Id == "unknown") continue;              // служебный, руками не добавляют
            if (t->Unique && node->Find(t->Id)) continue;  // уже есть
            if (first || t->Category != lastCategory) {
                ImGui::TextDisabled("%s", T(CategoryName(t->Category)));
                lastCategory = t->Category;
                first = false;
            }
            if (ImGui::Selectable(T(t->Title))) {
                PushUndo();
                node->Add(t->Id);
            }
            if (ImGui::IsItemHovered() && !t->Hint.empty()) ImGui::SetTooltip("%s", T(t->Hint));
        }
        ImGui::EndCombo();
    }

    // --- «почему так» (§114) -------------------------------------------------
    ImGui::Separator();
    if (ImGui::CollapsingHeader(T("Why is it here?"))) {
        ImGui::TextWrapped("%s", ui::UIExplainPosition(m_rt.Doc(), m_rt.Layout(), node->Id).c_str());
        ImGui::Separator();
        ImGui::TextWrapped("%s",
                           ui::UIExplainVisibility(m_rt.Doc(), m_rt.Layout(), node->Id).c_str());
        ImGui::Separator();
        ImGui::TextWrapped("%s", ui::UIExplainOrder(m_rt.Doc(), m_rt.Layout(), node->Id).c_str());
        ImGui::Separator();
        ImGui::TextWrapped("%s", ui::UIExplainEffects(m_rt.Doc(), node->Id).c_str());
    }
    ImGui::EndChild();
}

// ============================================================================
//  Окно
// ============================================================================

void UIDocumentPanel::Draw(EditorHost& host, bool* open) {
    if (!open || !*open) return;
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }

    char title[256];
    std::snprintf(title, sizeof(title), "%s%s###ui_document", T("UI Document"),
                  m_dirty ? " *" : "");
    if (!ImGui::Begin(title, open, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }

    DrawToolbar(host);

    const float side = std::max(240.0f, ImGui::GetContentRegionAvail().x * 0.20f);
    DrawHierarchy(host, side);
    ImGui::SameLine();
    ImGui::BeginChild("##ui_center", ImVec2(-side - 8.0f, 0));
    DrawCanvas(host);
    ImGui::EndChild();
    ImGui::SameLine();
    DrawInspector(host, 0.0f);

    if (m_browser.IsOpen() && m_browser.Draw()) {
        const std::string path = m_browser.Result().string();
        if (m_browseIsSave) {
            const bool prefab = path.size() > 9 &&
                                path.compare(path.size() - 9, 9, ui::kUIPrefabExt) == 0;
            if (prefab && m_primary != ui::kUIInvalidNode) {
                PushUndo();
                ui::UICreatePrefabFromNode(m_rt.Doc(), m_primary, path);
                host.SetStatusMessage(T("UI prefab saved"));
            } else if (Save(path)) {
                host.SetStatusMessage(T("UI document saved"));
            }
        } else if (Open(path)) {
            host.SetStatusMessage(T("UI document loaded"));
        } else {
            host.SetStatusMessage(T("Could not read the UI document"));
        }
    }

    ImGui::End();
}
