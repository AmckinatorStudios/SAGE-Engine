#include "HierarchyPanel.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "EditorHost.h"
#include "Project.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/scene/UIScene.h"
#include "../Localization.h"

namespace sui = sage::ui::sui;
using sui::Icon;

namespace {

// Какой значок у сущности. Порядок проверок — от самого «говорящего»
// компонента к самому общему: у камеры со скриптом важнее, что это камера, а
// меш есть почти у всего и потому проверяется последним.
//
// По значку иерархия читается одним взглядом: в списке из полусотни «Object»
// глазу не за что зацепиться, а «свет / камера / зонд / модель» видно сразу.
Icon EntityIcon(entt::registry& reg, entt::entity e) {
    if (reg.all_of<CameraComponent>(e)) return Icon::Camera;
    // Солнце — не лампа. Направленный свет один на сцену и задаёт всё её
    // настроение, поэтому в списке он обязан отличаться с первого взгляда.
    if (const LightComponent* lc = reg.try_get<LightComponent>(e))
        return lc->Kind == LightComponent::Type::Directional ? Icon::Sun : Icon::Light;
    if (reg.all_of<ReflectionProbeComponent>(e)) return Icon::Sphere;
    if (reg.all_of<ParticleEmitterComponent>(e)) return Icon::Particles;
    if (reg.all_of<AnimatedModelComponent>(e)) return Icon::Bone;
    if (reg.all_of<sage::ui::UIDocumentComponent>(e)) return Icon::Panel;
    if (reg.all_of<RigidBodyComponent>(e) || reg.all_of<ColliderComponent>(e))
        return Icon::Physics;
    if (reg.all_of<ScriptComponent>(e)) return Icon::Script;
    if (const MeshRendererComponent* mr = reg.try_get<MeshRendererComponent>(e)) {
        if (mr->Ref.type == MeshRef::Type::Model) return Icon::Mesh;
        if (mr->Ref.type == MeshRef::Type::Sphere) return Icon::Sphere;
        if (mr->Ref.type != MeshRef::Type::None) return Icon::Cube;
    }
    return Icon::File;
}

// Поиск без учёта регистра — по обеим раскладкам не ищем, но заглавные буквы
// человек в списке объектов набирает как придётся.
bool Contains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

std::string RowId(int id) { return std::to_string(id); }

} // namespace

// ============================================================================
//  Сборка
// ============================================================================

void HierarchyPanel::Build(sui::UIContext& ui, sui::UIElement* root) {
    m_ui = &ui;
    root->Vertical(0.0f);

    // --- Поиск ---
    sui::UIElement* top = ui.CreateIn<sui::UIElement>(root);
    top->SetName("SearchRow");
    top->Horizontal(4.0f);
    top->SetStretch(true, false);
    top->SetHeight(28.0f);
    top->Padding(sage::ui::UIEdges(6.0f, 4.0f, 6.0f, 2.0f));

    m_search = ui.CreateIn<sui::SearchBox>(top, std::string(T("Search entity...")));
    m_search->SetStretch(true, false);
    // Поле подхватывает уже стоящий фильтр: панель собирается при первом
    // показе, а поиск мог быть задан раньше — из прогона без человека или из
    // восстановленного состояния. Иначе список сужен, а поле пустое, и понять,
    // почему видно три объекта из тринадцати, нельзя.
    if (!m_filter.empty()) m_search->SetValue(m_filter);
    m_search->OnChanged([this](const std::string& text) {
        m_filter = text;
        // Слепок сбрасывается принудительно: сцена не менялась, а список —
        // обязан.
        m_stamp = 0;
    });

    // --- Дерево ---
    m_tree = ui.CreateIn<sui::Tree>(root);
    m_tree->SetStretch(true, true);
    m_tree->OnSelect([this](const std::string& rowId, bool additive) {
        const int id = std::atoi(rowId.c_str());
        if (!m_host) return;
        if (additive) m_host->ToggleSelection(id);
        else m_host->SetSelectedId(id);
    });
    m_tree->OnToggle([this](const std::string& rowId) {
        const int id = std::atoi(rowId.c_str());
        if (m_collapsed.count(id)) m_collapsed.erase(id);
        else m_collapsed.insert(id);
        m_stamp = 0;
    });
    m_tree->OnVisibility([this](const std::string& rowId) {
        const entt::entity e = Resolve(rowId);
        if (e == entt::null || !m_host) return;
        entt::registry& reg = m_host->CurrentScene().Registry();
        // Скрытие — правка сцены, а не «вид редактора»: оно сохраняется вместе
        // со сценой, значит обязано попадать в отмену наравне с переносом.
        m_host->PushUndoSnapshot();
        if (reg.all_of<HiddenComponent>(e)) reg.remove<HiddenComponent>(e);
        else reg.emplace<HiddenComponent>(e);
        m_stamp = 0;
    });
    m_tree->OnActivate([this](const std::string& rowId) {
        const int id = std::atoi(rowId.c_str());
        if (!m_host) return;
        m_host->SetSelectedId(id);
        m_host->FocusSelected();   // двойной щелчок — «показать мне это»
    });
    m_tree->OnContext([this](const std::string& rowId, glm::vec2 at) {
        const int id = std::atoi(rowId.c_str());
        if (!m_host || !m_menu) return;
        // Правый щелчок по объекту ВНЕ набора переключает выделение на него;
        // по объекту из набора — набор сохраняется, и Duplicate/Delete
        // применятся ко всем выбранным.
        if (!m_host->IsSelected(id)) m_host->SetSelectedId(id);
        m_menuTarget = id;
        m_menu->OpenAt(at);
    });
    // Перетаскивание строки на строку — смена родителя; мимо строк — открепить
    // в корень. Это единственный способ построить иерархию мышью, и без него
    // «вложить объект в группу» делается только через контекстное меню одного
    // конкретного вида.
    m_tree->OnDrop([this](const std::string& dragged, const std::string& onto) {
        if (!m_host) return;
        Scene& scene = m_host->CurrentScene();
        const int childId = std::atoi(dragged.c_str());
        const int parentId = onto.empty() ? -1 : std::atoi(onto.c_str());
        // Родитель внутри собственного поддерева — цикл, а не «странная
        // вёрстка»: сцена ушла бы в бесконечный обход при первой же матрице.
        if (parentId > 0) {
            const entt::entity child = Resolve(dragged);
            entt::entity p = Resolve(onto);
            while (p != entt::null && scene.Registry().valid(p)) {
                if (p == child) return;
                p = scene.ParentOf(p);
            }
        }
        m_host->PushUndoSnapshot();
        scene.SetParentById(childId, parentId);
        if (parentId > 0) m_collapsed.erase(parentId);   // ветку видно сразу
        m_stamp = 0;
    });
    m_tree->OnContextEmpty([this](glm::vec2 at) {
        if (!m_menu) return;
        m_menuTarget = 0;
        m_menu->OpenAt(at);
    });

    // --- Итог внизу ---
    m_summary = ui.CreateIn<sui::Label>(root, std::string());
    m_summary->SetName("Summary");
    m_summary->SetStyle("Caption");
    m_summary->SetStretch(true, false);
    m_summary->SetHeight(18.0f);
    m_summary->Padding(sage::ui::UIEdges(8.0f, 0.0f, 8.0f, 2.0f));

    // --- Контекстное меню ---
    //
    // ОДНО меню на панель, а не по меню на строку: пунктов десяток, а строк
    // бывает полторы сотни, и меню под каждую — полторы тысячи узлов ради
    // одного открытого. Цель запоминается в m_menuTarget при открытии.
    m_menu = ui.Create<sui::Popup>();
    m_menu->AddItem(T("Create Empty"), [this] {
        if (!m_host) return;
        m_host->PushUndoSnapshot();
        m_host->SetSelectedId(m_host->CurrentScene().CreateObject("Empty").Id());
        m_stamp = 0;
    });
    m_menu->AddItem(T("Create Cube"), [this] {
        if (!m_host) return;
        m_host->PushUndoSnapshot();
        m_host->SetSelectedId(m_host->CreateCubeEntity("Cube").Id());
        m_stamp = 0;
    });
    m_menu->AddSeparator();
    m_menu->AddItem(T("Create Child"), [this] {
        if (!m_host || m_menuTarget == 0) return;
        Scene& scene = m_host->CurrentScene();
        const entt::entity parent = Resolve(RowId(m_menuTarget));
        if (parent == entt::null) return;
        m_host->PushUndoSnapshot();
        GameObject child = scene.CreateObject("Child");
        scene.SetParent(child.Entity(), parent);
        // Ветка раскрывается сама: созданного ребёнка надо ВИДЕТЬ, иначе
        // действие выглядит не сработавшим.
        m_collapsed.erase(m_menuTarget);
        m_host->SetSelectedId(child.Id());
        m_stamp = 0;
    });
    m_menu->AddItem(T("Duplicate"), [this] {
        if (m_host && m_menuTarget != 0) { m_host->DuplicateSelected(); m_stamp = 0; }
    });
    m_menu->AddItem(T("Save as Prefab"), [this] {
        if (!m_host || m_menuTarget == 0) return;
        const entt::entity e = Resolve(RowId(m_menuTarget));
        if (e == entt::null) return;
        std::error_code ec;
        std::filesystem::path dir = m_host->CurrentProject().Dir() / "assets";
        std::filesystem::create_directories(dir, ec);
        std::string safe = m_host->CurrentScene().Registry().get<NameComponent>(e).Name;
        for (char& c : safe)
            if (c == '/' || c == '\\' || c == ':') c = '_';
        std::string err;
        if (!m_host->SaveSelectedAsPrefab(dir / (safe + ".sageprefab"), err))
            m_host->SetStatusMessage("Prefab save failed: " + err);
    });
    m_menu->AddItem(T("Unparent"), [this] {
        if (!m_host || m_menuTarget == 0) return;
        const entt::entity e = Resolve(RowId(m_menuTarget));
        if (e == entt::null) return;
        m_host->PushUndoSnapshot();
        m_host->CurrentScene().SetParent(e, entt::null);
        m_stamp = 0;
    });
    m_menu->AddSeparator();
    m_menu->AddItem(T("Delete"), [this] {
        if (m_host && m_menuTarget != 0) { m_host->DeleteSelected(); m_stamp = 0; }
    });

    Rebuild();
}

// ============================================================================
//  Обновление
// ============================================================================

entt::entity HierarchyPanel::Resolve(const std::string& rowId) const {
    if (!m_host) return entt::null;
    Scene& scene = m_host->CurrentScene();
    const int id = std::atoi(rowId.c_str());
    entt::registry& reg = scene.Registry();
    auto view = reg.view<IdComponent>();
    for (auto e : view)
        if (view.get<IdComponent>(e).Id == id) return e;
    return entt::null;
}

bool HierarchyPanel::Matches(Scene& scene, entt::entity e) const {
    entt::registry& reg = scene.Registry();
    if (m_filter.empty()) return true;
    if (const NameComponent* n = reg.try_get<NameComponent>(e))
        if (Contains(n->Name, m_filter)) return true;
    // Родитель, чей ребёнок подходит, остаётся в списке: иначе найденный
    // объект висел бы в воздухе без своей ветки.
    if (const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e))
        for (entt::entity k : h->Children)
            if (reg.valid(k) && Matches(scene, k)) return true;
    return false;
}

void HierarchyPanel::Collect(Scene& scene, entt::entity e, int depth,
                             std::vector<sui::TreeItem>& out) {
    entt::registry& reg = scene.Registry();
    if (!reg.valid(e) || !reg.all_of<IdComponent, NameComponent>(e)) return;
    if (!Matches(scene, e)) return;

    const int id = reg.get<IdComponent>(e).Id;
    const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
    const bool hasChildren = h && !h->Children.empty();
    // При поиске ветки раскрыты всегда: смысл поиска в том, чтобы показать
    // найденное, а не спрятать его в свёрнутом родителе.
    const bool expanded = !m_filter.empty() || !m_collapsed.count(id);

    sui::TreeItem item;
    item.Id = RowId(id);
    item.Text = reg.get<NameComponent>(e).Name;
    item.Icon = sage::ui::icons::Icons::Name(EntityIcon(reg, e));
    item.Depth = depth;
    item.HasChildren = hasChildren;
    item.Expanded = expanded;
    item.Selected = m_host && m_host->IsSelected(id);
    // Своя метка, а не наследованная: строка показывает СОСТОЯНИЕ ОБЪЕКТА, и
    // «глаз» ребёнка спрятанной группы обязан остаться открытым — ребёнка никто
    // не прятал, его не видно из-за родителя.
    item.Visible = !reg.all_of<HiddenComponent>(e);
    // Спрятанный поддеревом — приглушён: почему объекта нет в кадре, видно из
    // списка, а не только по родителю.
    if (sage::ecs::IsHidden(reg, e)) item.Tint = sage::ui::UIColor(0.55f, 0.58f, 0.64f, 1.0f);
    out.push_back(std::move(item));

    if (!expanded || !hasChildren) return;
    // Копия детей и стабильный порядок по id: обход не должен зависеть от того,
    // в каком порядке entt хранит сущности.
    std::vector<entt::entity> kids = h->Children;
    std::sort(kids.begin(), kids.end(), [&](entt::entity a, entt::entity b) {
        const IdComponent* ia = reg.try_get<IdComponent>(a);
        const IdComponent* ib = reg.try_get<IdComponent>(b);
        return (ia ? ia->Id : 0) < (ib ? ib->Id : 0);
    });
    for (entt::entity k : kids) Collect(scene, k, depth + 1, out);
}

void HierarchyPanel::Rebuild() {
    if (!m_host || !m_tree) return;
    Scene& scene = m_host->CurrentScene();
    entt::registry& reg = scene.Registry();

    std::vector<std::pair<int, entt::entity>> roots;
    auto view = reg.view<IdComponent, NameComponent>();
    for (auto e : view) {
        const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
        const bool hasParent = h && h->Parent != entt::null && reg.valid(h->Parent);
        if (!hasParent) roots.push_back({view.get<IdComponent>(e).Id, e});
    }
    std::sort(roots.begin(), roots.end());

    std::vector<sui::TreeItem> items;
    items.reserve(roots.size() * 2);
    for (auto& [id, e] : roots) Collect(scene, e, 0, items);
    m_tree->SetItems(std::move(items));

    if (m_summary) {
        const std::string text = scene.Name() + "  •  " +
                                 std::to_string(m_tree->Items().size()) + " / " +
                                 std::to_string(scene.Count());
        m_summary->SetText(text);
    }
}

void HierarchyPanel::Sync(float dt) {
    (void)dt;
    if (!m_host || !m_tree) return;
    Scene& scene = m_host->CurrentScene();
    entt::registry& reg = scene.Registry();

    // Пересобирать список каждый кадр не нужно и вредно: SetItems при равном
    // содержимом всё равно проходит по всем строкам, а кадр редактора и так
    // занят. Слепок ловит ровно то, от чего зависит СПИСОК: сколько объектов,
    // кто выбран, что спрятано и как называется сцена.
    std::size_t stamp = 1469598103934665603ull;
    auto mix = [&stamp](std::size_t v) {
        stamp ^= v + 0x9e3779b9ull + (stamp << 6) + (stamp >> 2);
    };
    mix(scene.Count());
    mix(std::hash<std::string>{}(scene.Name()));
    auto view = reg.view<IdComponent, NameComponent>();
    for (auto e : view) {
        const int id = view.get<IdComponent>(e).Id;
        mix((std::size_t)id);
        mix(std::hash<std::string>{}(view.get<NameComponent>(e).Name));
        if (m_host->IsSelected(id)) mix(0x51ed270bull);
        if (reg.all_of<HiddenComponent>(e)) mix(0x9e3779b1ull);
        if (const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e))
            mix((std::size_t)h->Children.size() * 31u + (h->Parent == entt::null ? 0u : 7u));
    }
    if (stamp == m_stamp) return;
    m_stamp = stamp;
    Rebuild();
}

void HierarchyPanel::OnAssetDropped(const std::string& path, glm::vec2 at) {
    if (!m_host || !m_tree || path.empty()) return;
    // Строка под курсором ищется по прямоугольникам строк дерева — тем же
    // способом, каким дерево находит цель собственного перетаскивания.
    const std::string row = m_tree->RowAt(at);
    if (!row.empty()) {
        if (!m_host->ApplyAssetToEntity(std::atoi(row.c_str()), path))
            m_host->SetStatusMessage(T("This file cannot be applied to an object"));
        return;
    }
    // Точки под курсором в списке нет — это не трёхмерный вид, — поэтому объект
    // встаёт в начало координат, как при создании через меню Entity.
    if (!m_host->AddAssetToScene(path))
        m_host->SetStatusMessage(T("Only a model or a prefab can be added to the scene"));
    m_stamp = 0;
}

void HierarchyPanel::SetFilter(const std::string& text) {
    m_filter = text;
    if (m_search) m_search->SetValue(text);
    m_stamp = 0;   // сцена не менялась, а список обязан
}

int HierarchyPanel::VisibleCount() const {
    return m_tree ? (int)m_tree->Items().size() : 0;
}

std::string HierarchyPanel::VisibleText() const {
    std::string out;
    if (!m_tree) return out;
    for (const sui::TreeItem& item : m_tree->Items()) {
        if (!out.empty()) out += "\n";
        out += item.Text;
    }
    return out;
}
