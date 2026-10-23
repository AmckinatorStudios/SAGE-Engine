#include "sage/scene/SceneFlow.h"

#include <algorithm>
#include <mutex>

#include "sage/core/Log.h"
#include "sage/scene/Components.h"
#include "sage/scene/Prefab.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"

namespace sage::scene {

namespace {
std::mutex& FlowMutex() {
    static std::mutex m;
    return m;
}
SceneLocator& TheLocator() {
    static SceneLocator l;
    return l;
}

// Есть ли метка у самого объекта или у кого-то из предков: переносится только
// ВЕРХНИЙ помеченный — его потомки едут вместе с ним, второй раз их везти нельзя.
bool AncestorKept(const entt::registry& reg, entt::entity e) {
    for (int guard = 0; guard < 64; ++guard) {
        const auto* h = reg.try_get<HierarchyComponent>(e);
        if (!h || h->Parent == entt::null || !reg.valid(h->Parent)) return false;
        e = h->Parent;
        if (reg.all_of<KeepOnLoadComponent>(e)) return true;
    }
    return false;
}

// Поддерево — на верхний уровень другой сцены, с МИРОВЫМ положением: у
// объекта, который был чьим-то ребёнком, в новой сцене родителя нет, и
// локальные координаты унесли бы его в сторону.
GameObject CopyAsRoot(Scene& src, entt::entity root, Scene& dst) {
    const glm::mat4 world = src.WorldMatrix(root);
    GameObject copy = CopySubtree(src, root, dst, entt::null);
    copy.GetTransform().SetFromMatrix(world);
    return copy;
}
} // namespace

void SetSceneLocator(SceneLocator locator) {
    std::lock_guard<std::mutex> lock(FlowMutex());
    TheLocator() = std::move(locator);
}

std::string ResolveScene(const std::string& name) {
    SceneLocator l;
    {
        std::lock_guard<std::mutex> lock(FlowMutex());
        l = TheLocator();
    }
    return l.Resolve && !name.empty() ? l.Resolve(name) : std::string();
}

std::vector<std::string> ListScenes() {
    SceneLocator l;
    {
        std::lock_guard<std::mutex> lock(FlowMutex());
        l = TheLocator();
    }
    std::vector<std::string> out = l.List ? l.List() : std::vector<std::string>{};
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool SceneExists(const std::string& name) { return !ResolveScene(name).empty(); }

SceneTransit& Transit() {
    static SceneTransit t;
    return t;
}

void ResetTransit(const std::string& firstScene) {
    Transit() = SceneTransit{};
    Transit().Current = firstScene;
    Transit().First = firstScene;
}

void EnterScene(const std::string& name) {
    SceneTransit& t = Transit();
    t.Previous = t.Current;
    t.Current = name;
    t.ParamsJson = std::move(t.NextParams);
    t.NextParams.clear();
}

void DropNextParams() { Transit().NextParams.clear(); }

std::unique_ptr<Scene> TakeKept(Scene& from) {
    auto kept = std::make_unique<Scene>("Kept");
    entt::registry& reg = from.Registry();
    // По возрастанию номера: порядок объектов в новой сцене не должен зависеть
    // от истории создания и удаления сущностей.
    std::vector<std::pair<int, entt::entity>> marked;
    for (auto e : reg.view<KeepOnLoadComponent, IdComponent>())
        if (!AncestorKept(reg, e)) marked.emplace_back(reg.get<IdComponent>(e).Id, e);
    std::sort(marked.begin(), marked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [id, e] : marked) CopyAsRoot(from, e, *kept);
    return kept;
}

int PutKept(Scene& kept, Scene& to) {
    int moved = 0;
    entt::registry& dst = to.Registry();
    for (entt::entity root : kept.Roots()) {
        const std::string name = GameObject(&kept.Registry(), root).Name();
        // Тот же объект, оставленный в новой сцене её автором (менеджер игры
        // стоит в каждой сцене, чтобы любую можно было запустить отдельно), —
        // уступает пришедшему: у пришедшего живое состояние игры.
        std::vector<int> twins;
        for (auto e : dst.view<KeepOnLoadComponent, NameComponent, IdComponent>())
            if (dst.get<NameComponent>(e).Name == name && !AncestorKept(dst, e))
                twins.push_back(dst.get<IdComponent>(e).Id);
        for (int id : twins) to.RemoveObject(id);
        CopyAsRoot(kept, root, to);
        ++moved;
    }
    return moved;
}

std::vector<int> AddScene(Scene& into, Scene& from, const std::string& group, const glm::vec3* offset) {
    std::vector<int> made;
    for (entt::entity root : from.Roots()) {
        GameObject copy = CopyAsRoot(from, root, into);
        if (offset) copy.GetTransform().Position += *offset;
        into.Registry().emplace_or_replace<AdditiveGroupComponent>(copy.Entity(), AdditiveGroupComponent{group});
        made.push_back(copy.Id());
    }
    return made;
}

std::vector<int> LoadAdditive(Scene& into, const std::string& name, std::string& err, const std::string& group,
                              const glm::vec3* offset) {
    const std::string path = ResolveScene(name);
    if (path.empty()) {
        err = "сцена «" + name + "» не найдена";
        return {};
    }
    std::unique_ptr<Scene> loaded;
    try {
        loaded = SceneSerializer::Load(path);
    } catch (const std::exception& e) {
        err = "сцена «" + name + "» не загрузилась: " + e.what();
        return {};
    }
    // Окружение (небо, туман, свет мира) у добавленной сцены НЕ берётся: у
    // кадра оно одно, и кусок карты не должен перекрашивать небо уровня.
    return AddScene(into, *loaded, group.empty() ? name : group, offset);
}

int UnloadAdditive(Scene& scene, const std::string& group) {
    std::vector<int> ids;
    auto view = scene.Registry().view<AdditiveGroupComponent, IdComponent>();
    for (auto e : view)
        if (view.get<AdditiveGroupComponent>(e).Group == group) ids.push_back(view.get<IdComponent>(e).Id);
    for (int id : ids) scene.RemoveObject(id);
    return (int)ids.size();
}

} // namespace sage::scene
