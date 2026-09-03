#include "sage/scene/Prefab.h"

#include <memory>
#include <unordered_map>
#include <vector>

#include "sage/core/Log.h"
#include "sage/scene/Components.h"
#include "sage/ui/UI.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"

namespace sage::scene {
namespace {

template <typename T>
void CopyIfPresent(GameObject& src, GameObject& dst) {
    if (const T* c = src.Registry()->try_get<T>(src.Entity())) {
        dst.Registry()->emplace_or_replace<T>(dst.Entity(), *c);
    }
}

// Разобранные префабы. Ключ — путь как его дали: тот же путь придёт и в
// следующий раз, а нормализовать его тут значило бы завести вторую политику
// путей рядом с той, что уже есть в базе ассетов.
std::unordered_map<std::string, std::shared_ptr<Scene>>& Cache() {
    static std::unordered_map<std::string, std::shared_ptr<Scene>> cache;
    return cache;
}

std::shared_ptr<Scene> LoadCached(const std::string& path) {
    auto& cache = Cache();
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;
    std::shared_ptr<Scene> loaded;
    try {
        loaded = std::shared_ptr<Scene>(SceneSerializer::Load(path).release());
    } catch (const std::exception& e) {
        LOG_ERROR("Prefab") << "Префаб не загрузился (" << path << "): " << e.what();
        // Неудачу тоже кладём в кэш — иначе игра, спавнящая блоки каждый кадр,
        // каждый кадр же ломилась бы в отсутствующий файл и заливала лог.
        cache[path] = nullptr;
        return nullptr;
    }
    cache[path] = loaded;
    return loaded;
}

// Корни префаба — сущности без живого родителя.
std::vector<entt::entity> RootsOf(Scene& scene) {
    std::vector<entt::entity> roots;
    auto& reg = scene.Registry();
    for (auto e : reg.view<IdComponent>()) {
        const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
        if (!h || h->Parent == entt::null || !reg.valid(h->Parent)) roots.push_back(e);
    }
    return roots;
}

} // namespace

void CopyAllComponents(GameObject& src, GameObject& dst) {
    dst.GetTransform() = src.GetTransform();
    dst.Renderer() = src.Renderer();
    CopyIfPresent<ScriptComponent>(src, dst);
    // Публичные переменные едут с объектом: префаб без своих настроек — это
    // объект, который после постановки в сцену надо настраивать заново.
    CopyIfPresent<VarsComponent>(src, dst);
    CopyIfPresent<CameraComponent>(src, dst);
    CopyIfPresent<LightComponent>(src, dst);
    CopyIfPresent<RigidBodyComponent>(src, dst);
    CopyIfPresent<ColliderComponent>(src, dst);
    CopyIfPresent<JointComponent>(src, dst);
    CopyIfPresent<ParticleEmitterComponent>(src, dst);
    CopyIfPresent<AnimatedModelComponent>(src, dst);
    CopyIfPresent<IKComponent>(src, dst);
    CopyIfPresent<ReflectionProbeComponent>(src, dst);
    // Интерфейс — набор компонентов, и копировать его надо целиком: пропустить
    // одну часть значит получить префаб-кнопку, которая не нажимается, или
    // панель без надписи.
    CopyIfPresent<sage::ui::Transform>(src, dst);
    CopyIfPresent<sage::ui::Fill>(src, dst);
    CopyIfPresent<sage::ui::Label>(src, dst);
    CopyIfPresent<sage::ui::Image>(src, dst);
    CopyIfPresent<sage::ui::Bar>(src, dst);
    CopyIfPresent<sage::ui::Icon>(src, dst);
    CopyIfPresent<sage::ui::Interactable>(src, dst);
    CopyIfPresent<sage::ui::TextInput>(src, dst);
    CopyIfPresent<sage::ui::Range>(src, dst);
    CopyIfPresent<sage::ui::Mask>(src, dst);
    CopyIfPresent<sage::ui::Layout>(src, dst);
    CopyIfPresent<sage::ui::Canvas>(src, dst);
    CopyIfPresent<sage::ui::Group>(src, dst);
    CopyIfPresent<GIStaticComponent>(src, dst);
    CopyIfPresent<CharacterControllerComponent>(src, dst);
    CopyIfPresent<ShaderParamsComponent>(src, dst);

    // Дальше — сброс того, что принадлежит ЭКЗЕМПЛЯРУ, а не шаблону. Скопируй
    // мы это как есть, две сущности делили бы один дескриптор тела, и удаление
    // одной уносило бы физику другой.
    if (auto* rb = dst.Registry()->try_get<RigidBodyComponent>(dst.Entity()))
        rb->RuntimeBody = sage::physics::kInvalidBody;
    if (auto* cc = dst.Registry()->try_get<CharacterControllerComponent>(dst.Entity())) {
        cc->Runtime = sage::physics::kInvalidCharacter;
        cc->Grounded = false;
    }
    if (auto* jc = dst.Registry()->try_get<JointComponent>(dst.Entity()))
        jc->RuntimeJoint = sage::physics::kInvalidJoint;
    if (auto* am = dst.Registry()->try_get<AnimatedModelComponent>(dst.Entity())) {
        am->Model.reset();
        am->Anim = sage::anim::Animator{};
        am->Ready = false;
    }
    // Индексы костей и залипшая опора — состояние экземпляра: у копии модель
    // загрузится заново, и цели должны разрешиться по именам с нуля.
    if (auto* ik = dst.Registry()->try_get<IKComponent>(dst.Entity()))
        for (IKGoal& g : ik->Goals) { g.Resolved = false; g.Locked = false; }
    // Снятая карта принадлежит ТОЧКЕ, а копия стоит в другой: делить её нельзя,
    // иначе копия отражала бы окружение оригинала.
    if (auto* rp = dst.Registry()->try_get<ReflectionProbeComponent>(dst.Entity())) {
        rp->Runtime.reset();
        rp->Dirty = true;
    }
    if (auto* pe = dst.Registry()->try_get<ParticleEmitterComponent>(dst.Entity()))
        pe->Accumulator = 0.0f;
}

GameObject CopySubtree(Scene& src, entt::entity srcRoot, Scene& dst, entt::entity dstParent) {
    GameObject s(&src.Registry(), srcRoot);
    GameObject d = dst.CreateObject(s.Name());
    CopyAllComponents(s, d);
    if (dstParent != entt::null) dst.SetParent(d.Entity(), dstParent);
    if (const HierarchyComponent* h = src.Registry().try_get<HierarchyComponent>(srcRoot)) {
        std::vector<entt::entity> kids = h->Children; // копия: SetParent мутирует список
        for (entt::entity k : kids)
            if (src.Registry().valid(k)) CopySubtree(src, k, dst, d.Entity());
    }
    return d;
}

bool SavePrefab(Scene& scene, entt::entity root, const std::string& path, std::string& err) {
    if (!scene.Registry().valid(root)) {
        err = "сущность не существует";
        return false;
    }
    Scene temp("Prefab");
    CopySubtree(scene, root, temp, entt::null);
    try {
        SceneSerializer::Save(temp, path);
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    // Файл переписан — разобранная копия в кэше устарела.
    Cache().erase(path);
    return true;
}

int InstantiatePrefab(Scene& scene, const std::string& path) {
    std::shared_ptr<Scene> prefab = LoadCached(path);
    if (!prefab) return -1;

    int firstRootId = -1;
    for (entt::entity r : RootsOf(*prefab)) {
        GameObject copy = CopySubtree(*prefab, r, scene, entt::null);
        if (firstRootId == -1) firstRootId = copy.Id();
    }
    return firstRootId;
}

int InstantiatePrefabAt(Scene& scene, const std::string& path, const glm::vec3& position) {
    const int id = InstantiatePrefab(scene, path);
    if (id < 0) return id;
    GameObject root = scene.Get(id);
    if (root.Valid()) root.GetTransform().Position = position;
    return id;
}

void ClearPrefabCache() { Cache().clear(); }

} // namespace sage::scene
