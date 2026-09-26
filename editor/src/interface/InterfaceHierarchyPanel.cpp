#include "InterfaceHierarchyPanel.h"

#include <algorithm>
#include <cstring>

#include "imgui.h"
#include "imgui_internal.h"   // ImGui::GetCurrentWindow для полосы броска

#include "InterfaceClipboard.h"

#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorTheme.h"
#include "../HotkeyScope.h"
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

// Сущность интерфейса, который сейчас верстают (entt::null — «без интерфейса»
// либо интерфейса нет вовсе).
entt::entity CurrentInterface(EditorHost& host) {
    const int id = host.CurrentInterfaceId();
    if (id <= 0) return entt::null;
    GameObject obj = host.CurrentScene().Get(id);
    return obj.Valid() ? obj.Entity() : entt::null;
}

// Корни ТЕКУЩЕГО интерфейса, а не все корни сцены.
//
// Это и есть главная правка панели. Раньше здесь собирались ВСЕ элементы
// сцены, у которых нет родителя-элемента: два интерфейса — меню и HUD —
// оказывались в одном дереве вперемешку, и верстать один, не задевая другой,
// можно было только пряча чужие объекты глазом в иерархии.
std::vector<entt::entity> Roots(EditorHost& host, Scene& scene) {
    // Правило «чей это элемент» живёт в движке (sage/ui/UISceneSystem.h) — тем
    // же пользуется отрисовка. Своя копия здесь означала бы дерево, которое
    // показывает не то, что рисует холст.
    std::vector<entt::entity> out = ui::InterfaceRoots(scene, CurrentInterface(host));
    entt::registry& reg = scene.Registry();
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
void PlaceBefore(EditorHost& host, Scene& scene, entt::entity moved, entt::entity before,
                 entt::entity parent) {
    entt::registry& reg = scene.Registry();
    std::vector<entt::entity> siblings =
        parent == entt::null ? Roots(host, scene) : UiChildren(scene, parent);
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
    if (!ImGui::Begin(EditorIcons::WindowTitle("list", T("Elements"), "InterfaceHierarchy").c_str(), &open,
                      panelwindows::WindowFlags("InterfaceHierarchy"))) {
        ImGui::End();
        return;
    }

    DrawInterfacePicker(host);
    DrawToolbar(host);
    ImGui::Separator();

    Scene& scene = host.CurrentScene();
    m_rows.clear();
    m_lines.Clear();   // строки прошлого кадра — это координаты, которых уже нет

    ImGui::BeginChild("##ui_tree_scroll");
    const std::vector<entt::entity> roots = Roots(host, scene);
    for (size_t i = 0; i < roots.size(); ++i) {
        DropGap(host, scene, roots[i], entt::null);
        DrawNode(host, scene, roots[i], 0);
    }
    if (!roots.empty()) DropGap(host, scene, entt::null, entt::null);

    if (roots.empty()) {
        ImGui::Spacing();
        if (host.CurrentInterfaceId() < 0) {
            // Интерфейсов в сцене нет вовсе. Предлагаем завести — а не молчим:
            // «элементов нет» при отсутствующем интерфейсе отвечает не на тот
            // вопрос, с которым сюда пришли.
            ImGui::TextDisabled("%s", T("No interface in the scene yet."));
            if (EditorIcons::Button("plus", T("Create interface"),
                                    T("An interface object: its elements live inside it")))
                CreateInterface(host);
        } else {
            ImGui::TextDisabled("%s", T("No interface elements yet."));
            ImGui::TextDisabled("%s", T("Add one with Create above."));
        }
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
            // «В корень» — это корень ИНТЕРФЕЙСА, а не сцены (см. ReparentElement):
            // раньше элемент уезжал из интерфейса и пропадал из дерева.
            if (ui::CanReparent(scene, dragged, entt::null)) {
                host.PushUndoSnapshot();
                ui::ReparentElement(scene, dragged, entt::null);
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();

    HandleShortcuts(host);
    ImGui::End();
}

// ЧТО ИМЕННО ВЕРСТАЕМ — ПЕРВОЙ СТРОКОЙ ПАНЕЛИ.
//
// Интерфейсов в сцене может быть сколько угодно, и работают всегда с одним.
// Раньше выбора не было вовсе: дерево показывало все сразу, и «переключиться
// на другое меню» означало спрятать чужие объекты. Теперь это список: выбрал —
// и дерево, холст и инспектор показывают именно его.
void InterfaceHierarchyPanel::DrawInterfacePicker(EditorHost& host) {
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();

    struct Row {
        int Id = 0;
        std::string Name;
    };
    std::vector<Row> rows;
    for (entt::entity e : ui::SortedInterfaces(scene)) {
        const IdComponent* id = reg.try_get<IdComponent>(e);
        const NameComponent* name = reg.try_get<NameComponent>(e);
        if (!id) continue;
        rows.push_back({id->Id, name ? name->Name : std::string("Interface")});
    }
    // Элементы без интерфейса — отдельной строкой, а не подмешанные к чужому
    // экрану: их собирает код или скрипт, и они существуют сами по себе.
    if (!ui::InterfaceRoots(scene, entt::null).empty()) rows.push_back({0, T("No interface")});

    std::string current = T("Nothing to edit");
    for (const Row& r : rows)
        if (r.Id == host.CurrentInterfaceId()) current = r.Name;

    ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginCombo("##interface", current.c_str())) {
        for (const Row& r : rows) {
            const bool selected = r.Id == host.CurrentInterfaceId();
            if (ImGui::Selectable(r.Name.c_str(), selected)) {
                host.SetCurrentInterface(r.Id);
                // Выделение сбрасывается: выбранный элемент принадлежал
                // ПРОШЛОМУ интерфейсу, и инспектор показывал бы чужое.
                host.Selection().Clear();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        // ИМЕННО EndCombo, а не EndPopup. Разница не косметическая: EndCombo
        // ещё и уменьшает счётчик глубины списков ImGui, по которому тот
        // ПЕРЕИСПОЛЬЗУЕТ окна («##Combo_00», «##Combo_01» …). С EndPopup
        // счётчик рос каждый кадр, пока список открыт, и следующий же список
        // редактора закрывался с проверкой «EndCombo() в чужом окне» — ImGui
        // прерывал отрисовку окна, и интерфейс редактора разваливался целиком.
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    // КНОПКА ТОЛЬКО ЗНАЧКОМ. У кнопки со значком и подписью подпись рисуется
    // как есть — «##new_interface» и печаталось рядом со значком, а ширина
    // кнопки считалась по этой надписи: кнопка вылезала за правый край панели,
    // где её обрезал ImGui, и нажатие не доходило («кнопка не работает»).
    if (EditorIcons::IconOnlyButton("plus", T("New interface in this scene")))
        CreateInterface(host);
}

void InterfaceHierarchyPanel::CreateInterface(EditorHost& host) {
    host.PushUndoSnapshot();
    Scene& scene = host.CurrentScene();
    // БЕЗ МЕША: интерфейс ничего не рисует сам, он граница. Объект с
    // компонентом «Меш» обещал бы модель, цвет и тени, которых у него нет.
    GameObject obj = scene.CreateEmptyObject("Interface");
    scene.Registry().emplace<sage::ui::InterfaceComponent>(obj.Entity());
    host.SetCurrentInterface(obj.Id());
    host.Selection().SetPrimary(obj.Id());
}

void InterfaceHierarchyPanel::DrawToolbar(EditorHost& host) {
    // Создание — единственная кнопка, которой здесь место постоянно: всё
    // остальное относится к УЖЕ ВЫБРАННОМУ и потому живёт в меню по правой
    // кнопке, где оно и ожидается.
    // ЭЛЕМЕНТЫ И КОНТЕЙНЕРЫ — РАЗНЫМИ КНОПКАМИ. Элемент — то, что видно
    // (кнопка, текст, картинка); контейнер — невидимая раскладка детей (ряд,
    // столбец, сетка, прокрутка). В одном списке они читались как «ещё один
    // элемент», и «столбец» создавали, чтобы получить панель с фоном.
    //
    // Категория и значок живут В САМОЙ ЗАГОТОВКЕ (sage::ui::Preset), а не
    // здесь: список типов — это и есть меню создания.
    auto typeMenu = [&](bool containers) {
        std::string category;
        for (const ui::Preset& preset : ui::Presets()) {
            if (preset.Container != containers) continue;
            if (!containers && preset.Category != category) {
                category = preset.Category;
                ImGui::SeparatorText(T(category.c_str()));
            }
            // Имя типа переводится, а создаётся элемент по английскому ключу.
            if (EditorIcons::MenuItem(preset.Icon, T(preset.Name.c_str()))) {
                host.PushUndoSnapshot();
                GameObject created = host.CreateUIEntity(preset.Name);
                if (created.Valid()) host.Selection().SetPrimary(created.Id());
            }
            if (preset.Hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(preset.Hint));
        }
    };
    if (EditorIcons::Button("plus", T("Element"), T("Add a visible element: text, button, picture...")))
        ImGui::OpenPopup("##ui_create");
    if (Sage::UI::MenuScope createMenu; ImGui::BeginPopup("##ui_create")) {
        typeMenu(false);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (EditorIcons::Button("layout", T("Container"),
                            T("Add an invisible layout: row, column, grid, scroll view...")))
        ImGui::OpenPopup("##ui_create_container");
    if (Sage::UI::MenuScope containerMenu; ImGui::BeginPopup("##ui_create_container")) {
        typeMenu(true);
        ImGui::EndPopup();
    }

    // Поиск — постоянно на виду и СВОЕЙ СТРОКОЙ во всю ширину. В одной строке
    // с двумя кнопками ему оставалась щель в несколько символов, и подсказка
    // «Поиск элементов…» обрезалась на полуслове.
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
            // Перенос, от которого сцена отказалась (в собственного потомка),
            // не должен и переставлять порядок: элемент попал бы в список
            // чужих соседей, оставаясь на старом месте.
            if (dragged != before && ui::CanReparent(scene, dragged, parent)) {
                host.PushUndoSnapshot();
                ui::ReparentElement(scene, dragged, parent);
                PlaceBefore(host, scene, dragged, before, parent);
            }
        }
        // Линия под курсором: без неё непонятно, куда именно ляжет элемент.
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine({a.x, (a.y + b.y) * 0.5f}, {b.x, (a.y + b.y) * 0.5f},
                                            ImGui::GetColorU32(ImGuiCol_DragDropTarget), 2.0f);
        ImGui::EndDragDropTarget();
    }
    ImGui::PopID();
}

// ГЛАЗОК И ЗАМОК У ПРАВОГО КРАЯ СТРОКИ.
//
// Спрятать мешающую панель и запереть разложенный фон — два самых частых
// действия вёрстки, и ходить ради них в другое окно незачем. Но стоять они
// обязаны СПРАВА: перед именем они оттесняли бы значок типа, по которому
// строку и находят, а главное — занимали бы место стрелки раскрытия, от
// которой ImGui считает вложенность (из-за этого дерево и разъезжалось).
//
// Рисуются НАКЛАДКОЙ, а не кнопками: кнопка подала бы свой элемент, и
// «последним элементом» для ImGui стала бы она — выделение строки,
// перетаскивание и меню по правой кнопке начали бы отвечать про квадратик
// размером с букву вместо строки.
void InterfaceHierarchyPanel::DrawRowToggles(EditorHost& host, ui::Element& box,
                                             const ImVec2& rowPos) {
    const float size = ImGui::GetTextLineHeight();
    const float gap = 4.0f;
    const float right = ImGui::GetWindowContentRegionMax().x - ImGui::GetScrollX();
    const float baseX = ImGui::GetWindowPos().x + right;
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool overRow = ImGui::IsItemHovered();

    struct Toggle { const char* Icon; bool On; bool* Flag; const char* Tip; };
    const Toggle toggles[2] = {
        {box.Locked ? "lock" : "unlock", box.Locked, &box.Locked,
         box.Locked ? T("Unlock") : T("Lock")},
        {box.Visible ? "eye" : "eye-off", !box.Visible, &box.Visible,
         box.Visible ? T("Hide") : T("Show")},
    };
    float x = baseX - size - 2.0f;
    for (const Toggle& t : toggles) {
        const ImVec2 at(x, rowPos.y);
        const bool over = overRow && mouse.x >= at.x && mouse.x <= at.x + size &&
                          mouse.y >= at.y && mouse.y <= at.y + size;
        // Обычное состояние — тусклое: полсотни ярких значков подряд спорят с
        // именами, по которым список и читают. Наливаются цветом под курсором
        // и у того, что выключено или заперто.
        const glm::vec3 col = t.On      ? glm::vec3(0.95f, 0.78f, 0.30f)
                              : over    ? glm::vec3(0.80f, 0.82f, 0.88f)
                                        : glm::vec3(0.42f, 0.44f, 0.50f);
        EditorIcons::Overlay(at.x, at.y, size, t.Icon, col);
        if (over) {
            ImGui::SetTooltip("%s", t.Tip);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                host.PushUndoSnapshot();
                *t.Flag = !*t.Flag;
            }
        }
        x -= size + gap;
    }
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

    const std::vector<entt::entity> children = UiChildren(scene, e);
    // СТРОКА УСТРОЕНА ТАК ЖЕ, КАК В ИЕРАРХИИ СЦЕНЫ, и это не про красоту.
    // Глазок и замок стояли ПЕРЕД узлом дерева, то есть занимали то самое
    // место, куда ImGui ставит стрелку раскрытия и откуда считает отступ
    // вложенности: дерево разъезжалось, связи вести было не от чего, и глубина
    // читалась только по сдвигу подписи. Теперь узел начинается в начале
    // строки, а переключатели стоят у ПРАВОГО края — там же, где глаз в списке
    // объектов, и на одном месте независимо от глубины.
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DrawLinesNone |
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
        // УЗЕЛ БЕЗ ПОДПИСИ, подпись — рисунком вместе со значком: значок и
        // подпись это одна пара, и ставит их одна функция с одним зазором на
        // весь редактор (EditorIcons::DrawLabeled). Подпись внутри узла
        // потребовала бы пробелов под значок, ширина которых зависит от шрифта.
        const ImVec2 rowPos = ImGui::GetCursorScreenPos();
        const float indent = ImGui::GetTreeNodeToLabelSpacing();
        m_lines.Row(rowPos.y, depth, rowPos.x + ImGui::GetStyle().FramePadding.x,
                    rowPos.x + indent, !children.empty());
        open = ImGui::TreeNodeEx("##node", flags, "%s", "");
        m_rowPos = rowPos;
        m_rowIndent = indent;

        const NameComponent* name = reg.try_get<NameComponent>(e);
        // ЗНАЧОК — ПО ТИПУ ЭЛЕМЕНТА: панель, текст, кнопка узнаются по рисунку
        // до чтения имени, ровно как объекты сцены в списке слева.
        const ui::Preset* type = ui::FindPreset(box->Type);
        const ImU32 textCol = ImGui::GetColorU32(dim ? ImGuiCol_TextDisabled : ImGuiCol_Text);
        // Контейнер — значком ДРУГОГО цвета: он невидим в игре, и в дереве
        // его отличают от видимых элементов с первого взгляда.
        const ImU32 iconCol = (type && type->Container && !dim)
                                  ? ImGui::GetColorU32(EditorTheme::Color(EditorTheme::Role::Accent))
                                  : textCol;
        EditorIcons::DrawLabeled(ImGui::GetWindowDrawList(), ImVec2(rowPos.x + indent, rowPos.y),
                                 ImGui::GetTextLineHeight(), type ? type->Icon : "ui-empty",
                                 iconCol, name ? name->Name.c_str() : "Element", textCol);

        DrawRowToggles(host, *box, rowPos);
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
            // Запретный бросок (на себя или своего потомка) виден сразу: строка
            // не подсвечивается как цель, и отпускание ничего не делает.
            const ImGuiPayload* peek = ImGui::GetDragDropPayload();
            const bool allowed = !peek || !peek->IsDataType(kDragPayload) ||
                                 ui::CanReparent(scene, *(const entt::entity*)peek->Data, e);
            if (allowed) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kDragPayload)) {
                    const entt::entity dragged = *(const entt::entity*)p->Data;
                    host.PushUndoSnapshot();
                    ui::ReparentElement(scene, dragged, e);
                }
            } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
                ImGui::SetTooltip("%s", T("An element cannot go inside itself"));
            }
            ImGui::EndDragDropTarget();
        }

        DrawContextMenu(host, scene, e);
    }

    if (open && !children.empty()) {
        const ImVec2 rowPos = m_rowPos;
        const float indent = m_rowIndent;
        for (size_t i = 0; i < children.size(); ++i) {
            DropGap(host, scene, children[i], e);
            DrawNode(host, scene, children[i], depth + 1);
        }
        DropGap(host, scene, entt::null, e);
        // Связи к детям — ПОСЛЕ того, как они нарисованы: иначе неизвестно,
        // где кончился последний, и вертикаль пришлось бы вести наугад.
        m_lines.Draw(rowPos, indent, depth + 1);
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
            ui::ReparentElement(scene, e, entt::null);
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

    // Delete и Ctrl+D разбираем сами — и говорим об этом, чтобы общий
    // обработчик не сделал то же самое второй раз (см. HotkeyScope.h). До
    // этого Ctrl+D здесь давал ДВЕ копии: одну от панели, вторую от него.
    sage::editor::hotkeys::Claim(sage::editor::hotkeys::Key::Delete);
    sage::editor::hotkeys::Claim(sage::editor::hotkeys::Key::Duplicate);
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
