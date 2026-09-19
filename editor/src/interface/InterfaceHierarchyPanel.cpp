#include "InterfaceHierarchyPanel.h"

#include <algorithm>
#include <cstring>

#include "imgui.h"
#include "imgui_internal.h"   // ImGui::GetCurrentWindow для полосы броска

#include "InterfaceClipboard.h"

#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "InterfaceWidgets.h"
#include "../Localization.h"
#include "../PanelWindows.h"
#include "../ui/UI.h"

#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"

namespace uiclip = sage::editor::uiclipboard;
namespace ui = sage::ui;

namespace {

// Полезная нагрузка перетаскивания: номер сущности. Не путь и не имя — в дереве
// таскают КОНКРЕТНУЮ строку, а имена в интерфейсе повторяются сплошь и рядом.
constexpr const char* kDragPayload = "SAGE_UI_ELEMENT";

bool IsUi(const entt::registry& reg, entt::entity e) { return reg.all_of<ui::Element>(e); }

// Корни: элементы, у которых нет родителя-ЭЛЕМЕНТА. Именно они якорятся к
// экрану, остальные — к своему родителю.
std::vector<entt::entity> Roots(Scene& scene) {
    std::vector<entt::entity> out;
    entt::registry& reg = scene.Registry();
    for (entt::entity e : reg.view<ui::Element>()) {
        const entt::entity parent = scene.ParentOf(e);
        if (parent != entt::null && IsUi(reg, parent)) continue;
        out.push_back(e);
    }
    // Порядок корней — тот же, что на экране: больше Order — выше в списке
    // (он рисуется поверх). Иначе дерево и экран отвечают на вопрос «что
    // сверху» по-разному.
    std::stable_sort(out.begin(), out.end(), [&reg](entt::entity a, entt::entity b) {
        return reg.get<ui::Element>(a).Order > reg.get<ui::Element>(b).Order;
    });
    return out;
}

std::vector<entt::entity> UiChildren(Scene& scene, entt::entity e) {
    std::vector<entt::entity> out;
    entt::registry& reg = scene.Registry();
    if (const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e)) {
        for (entt::entity c : h->Children)
            if (IsUi(reg, c)) out.push_back(c);
    }
    std::stable_sort(out.begin(), out.end(), [&reg](entt::entity a, entt::entity b) {
        return reg.get<ui::Element>(a).Order > reg.get<ui::Element>(b).Order;
    });
    return out;
}

// Выделенные СУЩНОСТИ. Выделение хранит номера (они переживают перезагрузку
// сцены, а указатели — нет), а буфер обмена и операции над поддеревьями
// работают сущностями.
std::vector<entt::entity> SelectedEntities(EditorHost& host, Scene& scene) {
    std::vector<entt::entity> out;
    for (int id : host.Selection().All()) {
        GameObject obj = scene.Get(id);
        if (obj.Valid() && IsUi(scene.Registry(), obj.Entity())) out.push_back(obj.Entity());
    }
    return out;
}

int IdOf(const entt::registry& reg, entt::entity e) {
    const IdComponent* id = reg.try_get<IdComponent>(e);
    return id ? id->Id : -1;
}

std::string LowerOf(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

// Ставит элементу Order так, чтобы он оказался ПЕРЕД указанным соседом.
// Соседям при этом раздаются подряд идущие номера: держать их разреженными
// «на будущее» — значит однажды упереться в то, что вставить между двумя
// соседними числами некуда.
void PlaceBefore(Scene& scene, entt::entity moved, entt::entity before, entt::entity parent) {
    entt::registry& reg = scene.Registry();
    std::vector<entt::entity> siblings =
        parent == entt::null ? Roots(scene) : UiChildren(scene, parent);
    siblings.erase(std::remove(siblings.begin(), siblings.end(), moved), siblings.end());

    std::vector<entt::entity> ordered;
    ordered.reserve(siblings.size() + 1);
    for (entt::entity s : siblings) {
        if (s == before) ordered.push_back(moved);
        ordered.push_back(s);
    }
    if (before == entt::null || std::find(ordered.begin(), ordered.end(), moved) == ordered.end())
        ordered.push_back(moved);

    // Сверху списка — больший Order: в дереве выше значит на экране поверх.
    int order = (int)ordered.size();
    for (entt::entity s : ordered) reg.get<ui::Element>(s).Order = order--;
}

} // namespace

// ============================================================================
//  Панель
// ============================================================================

void InterfaceHierarchyPanel::Draw(EditorHost& host, bool& open) {
    if (!open) return;
    if (m_focusFrames > 0) {
        ImGui::SetNextWindowFocus();
        --m_focusFrames;
    }
    ImGui::SetNextWindowSize(ImVec2(320.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(T("Elements" "###InterfaceHierarchy"), &open,
                      panelwindows::WindowFlags("InterfaceHierarchy"))) {
        ImGui::End();
        return;
    }

    DrawToolbar(host);
    ImGui::Separator();

    Scene& scene = host.CurrentScene();
    m_rows.clear();

    ImGui::BeginChild("##ui_tree_scroll");
    const std::vector<entt::entity> roots = Roots(scene);
    for (size_t i = 0; i < roots.size(); ++i) {
        DropGap(host, scene, roots[i], entt::null);
        DrawNode(host, scene, roots[i], 0);
    }
    if (!roots.empty()) DropGap(host, scene, entt::null, entt::null);

    if (roots.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", T("No interface elements yet."));
        ImGui::TextDisabled("%s", T("Add one with Create above."));
    }

    // Щелчок по пустому месту снимает выделение — тот же жест, что во вьюпорте.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) {
        host.Selection().Clear();
    }
    // Бросок на пустое место — открепить в корень.
    if (ImGui::BeginDragDropTargetCustom(ImGui::GetCurrentWindow()->Rect(),
                                         ImGui::GetID("##ui_tree_root_drop"))) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kDragPayload)) {
            const entt::entity dragged = *(const entt::entity*)p->Data;
            host.PushUndoSnapshot();
            scene.SetParent(dragged, entt::null);
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();

    HandleShortcuts(host);
    ImGui::End();
}

void InterfaceHierarchyPanel::DrawToolbar(EditorHost& host) {
    // Создание — единственная кнопка, которой здесь место постоянно: всё
    // остальное относится к УЖЕ ВЫБРАННОМУ и потому живёт в меню по правой
    // кнопке, где оно и ожидается.
    if (EditorIcons::Button("plus", T("Create"), T("Add an element"))) ImGui::OpenPopup("##ui_create");
    if (Sage::UI::MenuScope createMenu; ImGui::BeginPopup("##ui_create")) {
        // СПИСОК ТИПОВ, РАЗБИТЫЙ ПО КАТЕГОРИЯМ, а не тринадцать строк подряд.
        //
        // Тринадцать имён в столбик читают целиком: «Grid» и «Vertical List»
        // рядом ничем не отличаются, пока не вспомнишь, что делает каждый.
        // Разделы отвечают на вопрос, с которым сюда и приходят: мне нужна
        // основа, орган управления, контейнер или целый экран.
        //
        // Категория и значок живут В САМОЙ ЗАГОТОВКЕ (sage::ui::Preset), а не
        // здесь: список типов — это и есть меню создания, и разбиение на
        // разделы, написанное отдельно от него, однажды потеряет новый тип.
        std::string category;
        for (const ui::Preset& preset : ui::Presets()) {
            if (preset.Category != category) {
                category = preset.Category;
                ImGui::SeparatorText(T(category.c_str()));
            }
            if (EditorIcons::MenuItem(preset.Icon, preset.Name.c_str())) {
                host.PushUndoSnapshot();
                GameObject created = host.CreateUIEntity(preset.Name);
                if (created.Valid()) host.Selection().SetPrimary(created.Id());
            }
            // Пояснение — подсказкой, а не второй строкой в пункте: строка
            // удвоила бы высоту меню, а читают её один раз, при знакомстве.
            if (preset.Hint && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", T(preset.Hint));
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    // Поиск — постоянно на виду, а не за кнопкой: в интерфейсе из сотни
    // элементов он нужен чаще, чем создание.
    ImGui::SetNextItemWidth(-1.0f);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", m_filter.c_str());
    if (ImGui::InputTextWithHint("##ui_search", T("Search elements..."), buf, sizeof(buf)))
        m_filter = buf;
}

bool InterfaceHierarchyPanel::MatchesFilter(const Scene& scene, entt::entity e) const {
    if (m_filter.empty()) return true;
    const entt::registry& reg = scene.Registry();
    if (const NameComponent* n = reg.try_get<NameComponent>(e)) {
        if (LowerOf(n->Name).find(LowerOf(m_filter)) != std::string::npos) return true;
    }
    // Родитель показывается, если что-то нашлось У НЕГО ВНУТРИ: показать одну
    // строку без её родителей значит показать, ЧТО нашлось, и спрятать, ГДЕ это
    // лежит.
    if (const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e)) {
        for (entt::entity c : h->Children)
            if (reg.all_of<ui::Element>(c) && MatchesFilter(scene, c)) return true;
    }
    return false;
}

void InterfaceHierarchyPanel::DropGap(EditorHost& host, Scene& scene, entt::entity before,
                                      entt::entity parent) {
    // Полоса высотой в несколько пикселей МЕЖДУ строками. Без неё порядок
    // среди соседей правился только числом Order, а «поставить выше соседа» —
    // самая частая правка порядка вообще.
    const float h = 4.0f;
    ImGui::PushID((int)(uint32_t)before + 1);
    ImGui::InvisibleButton("##gap", ImVec2(ImGui::GetContentRegionAvail().x, h));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kDragPayload)) {
            const entt::entity dragged = *(const entt::entity*)p->Data;
            host.PushUndoSnapshot();
            scene.SetParent(dragged, parent);
            PlaceBefore(scene, dragged, before, parent);
        }
        // Линия под курсором: без неё непонятно, куда именно ляжет элемент.
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine({a.x, (a.y + b.y) * 0.5f}, {b.x, (a.y + b.y) * 0.5f},
                                            ImGui::GetColorU32(ImGuiCol_DragDropTarget), 2.0f);
        ImGui::EndDragDropTarget();
    }
    ImGui::PopID();
}

bool InterfaceHierarchyPanel::DrawNode(EditorHost& host, Scene& scene, entt::entity e, int depth) {
    entt::registry& reg = scene.Registry();
    ui::Element* box = reg.try_get<ui::Element>(e);
    if (!box) return false;
    if (!MatchesFilter(scene, e)) return false;

    const int id = IdOf(reg, e);
    if (id < 0) return false;
    m_rows.push_back(e);

    ImGui::PushID(id);

    // ГЛАЗОК И ЗАМОК — в строке, а не в свойствах: спрятать мешающую панель и
    // запереть разложенный фон это два самых частых действия вёрстки, и ходить
    // ради них в другое окно незачем.
    if (EditorIcons::IconOnlyButton("eye", box->Visible ? T("Hide") : T("Show"), box->Visible)) {
        host.PushUndoSnapshot();
        box->Visible = !box->Visible;
    }
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("lock", box->Locked ? T("Unlock") : T("Lock"), box->Locked)) {
        host.PushUndoSnapshot();
        box->Locked = !box->Locked;
    }
    ImGui::SameLine();

    const std::vector<entt::entity> children = UiChildren(scene, e);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (host.Selection().Contains(id)) flags |= ImGuiTreeNodeFlags_Selected;

    const bool dim = !box->Visible || box->Locked;
    if (dim) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

    bool open;
    if (m_renaming == e) {
        // ПЕРЕИМЕНОВАНИЕ ПО МЕСТУ. Имя правилось в инспекторе, то есть в другом
        // окне: назвать десять элементов означало десять раз сходить туда и
        // обратно.
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::IsWindowAppearing() || ImGui::IsItemDeactivated()) ImGui::SetKeyboardFocusHere();
        const bool done = ImGui::InputText("##rename", m_renameBuf, sizeof(m_renameBuf),
                                           ImGuiInputTextFlags_EnterReturnsTrue |
                                               ImGuiInputTextFlags_AutoSelectAll);
        if (done || ImGui::IsItemDeactivated()) {
            if (done && m_renameBuf[0] != '\0') {
                host.PushUndoSnapshot();
                reg.get_or_emplace<NameComponent>(e).Name = m_renameBuf;
            }
            m_renaming = entt::null;
        }
        open = !children.empty();
    } else {
        const NameComponent* name = reg.try_get<NameComponent>(e);
        open = ImGui::TreeNodeEx("##node", flags, "%s", name ? name->Name.c_str() : "Element");
    }
    if (dim) ImGui::PopStyleColor();

    if (m_renaming != e) {
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.KeyShift && m_lastClicked != entt::null) SelectRangeTo(host, e);
            else if (io.KeyCtrl) { host.Selection().Toggle(id); m_lastClicked = e; }
            else { host.Selection().SetPrimary(id); m_lastClicked = e; }
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_renaming = e;
            const NameComponent* name = reg.try_get<NameComponent>(e);
            std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", name ? name->Name.c_str() : "");
        }

        // Перетаскивание: сама строка — источник, она же цель (бросок НА строку
        // делает элемент её ребёнком).
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kDragPayload, &e, sizeof(e));
            const NameComponent* name = reg.try_get<NameComponent>(e);
            ImGui::TextUnformatted(name ? name->Name.c_str() : "Element");
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kDragPayload)) {
                const entt::entity dragged = *(const entt::entity*)p->Data;
                host.PushUndoSnapshot();
                // Сцена сама не даст сделать элемент ребёнком собственного
                // потомка — цикл в иерархии это зависание, а не кривая картинка.
                scene.SetParent(dragged, e);
            }
            ImGui::EndDragDropTarget();
        }

        DrawContextMenu(host, scene, e);
    }

    if (open && !children.empty()) {
        for (size_t i = 0; i < children.size(); ++i) {
            DropGap(host, scene, children[i], e);
            DrawNode(host, scene, children[i], depth + 1);
        }
        DropGap(host, scene, entt::null, e);
        ImGui::TreePop();
    }
    ImGui::PopID();
    return true;
}

void InterfaceHierarchyPanel::SelectRangeTo(EditorHost& host, entt::entity e) {
    // По ПОКАЗАННОМУ порядку строк, а не по номерам сущностей: человек видит
    // список и ждёт, что выделится то, что между двумя строками НА ЭКРАНЕ.
    const auto from = std::find(m_rows.begin(), m_rows.end(), m_lastClicked);
    const auto to = std::find(m_rows.begin(), m_rows.end(), e);
    if (from == m_rows.end() || to == m_rows.end()) {
        host.Selection().SetPrimary(IdOf(host.CurrentScene().Registry(), e));
        m_lastClicked = e;
        return;
    }
    auto a = from, b = to;
    if (a > b) std::swap(a, b);
    std::vector<int> ids;
    const entt::registry& reg = host.CurrentScene().Registry();
    for (auto it = a; it <= b; ++it) {
        const int id = IdOf(reg, *it);
        if (id >= 0) ids.push_back(id);
    }
    host.Selection().Set(ids);
}

void InterfaceHierarchyPanel::DrawContextMenu(EditorHost& host, Scene& scene, entt::entity e) {
    if (Sage::UI::MenuScope menu; ImGui::BeginPopupContextItem("##ui_node_menu")) {
        const int id = IdOf(scene.Registry(), e);
        // Щелчок правой по НЕ выделенной строке сначала выделяет её: меню
        // относится к тому, по чему щёлкнули, а не к тому, что было выбрано
        // раньше, — иначе «Удалить» уносит не то.
        if (!host.Selection().Contains(id)) host.Selection().SetPrimary(id);

        if (EditorIcons::MenuItem("pencil", T("Rename"), "F2")) {
            m_renaming = e;
            const NameComponent* name = scene.Registry().try_get<NameComponent>(e);
            std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", name ? name->Name.c_str() : "");
        }
        if (EditorIcons::MenuItem("copy", T("Duplicate"), "Ctrl+D")) host.DuplicateSelected();
        ImGui::Separator();
        if (EditorIcons::MenuItem("copy", T("Copy"), "Ctrl+C"))
            uiclip::Copy(scene, SelectedEntities(host, scene));
        if (EditorIcons::MenuItem("paste", T("Paste"), "Ctrl+V")) {
            host.PushUndoSnapshot();
            uiclip::Paste(scene, e);
        }
        ImGui::Separator();

        ui::Element& box = scene.Registry().get<ui::Element>(e);
        if (EditorIcons::MenuItemSelected(box.Visible ? "eye" : "eye-off", T("Visible"),
                                          box.Visible)) {
            host.PushUndoSnapshot();
            box.Visible = !box.Visible;
        }
        if (EditorIcons::MenuItemSelected(box.Locked ? "lock" : "unlock", T("Locked"),
                                          box.Locked)) {
            host.PushUndoSnapshot();
            box.Locked = !box.Locked;
        }
        if (EditorIcons::MenuItem("up", T("Detach to root"), nullptr,
                                  scene.ParentOf(e) != entt::null)) {
            host.PushUndoSnapshot();
            scene.SetParent(e, entt::null);
        }
        ImGui::Separator();
        if (EditorIcons::MenuItem("trash", T("Delete"), "Del")) host.DeleteSelected();
        ImGui::EndPopup();
    }
}

void InterfaceHierarchyPanel::HandleShortcuts(EditorHost& host) {
    // Только пока окно в фокусе и никто не печатает: Ctrl+C в поле поиска
    // обязан копировать текст, а не элемент.
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    if (ImGui::GetIO().WantTextInput) return;

    Scene& scene = host.CurrentScene();
    const ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl;

    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) host.DeleteSelected();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) host.DuplicateSelected();
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
        uiclip::Copy(scene, SelectedEntities(host, scene));
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        host.PushUndoSnapshot();
        // Вставка идёт В ВЫДЕЛЕННЫЙ элемент (он становится родителем), а без
        // выделения — в корень. Это то, чего ждут: скопировал кнопку, выбрал
        // панель, вставил — кнопка внутри панели.
        const entt::entity parent = host.SelectedObject().Valid()
                                        ? host.SelectedObject().Entity()
                                        : entt::null;
        uiclip::Paste(scene, parent);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && host.SelectedObject().Valid()) {
        m_renaming = host.SelectedObject().Entity();
        const NameComponent* name = scene.Registry().try_get<NameComponent>(m_renaming);
        std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", name ? name->Name.c_str() : "");
    }
}
