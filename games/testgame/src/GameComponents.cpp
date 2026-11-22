#include "GameComponents.h"

#include <nlohmann/json.hpp>

#include "sage/scene/ComponentRegistry.h"

// Компоненты testgame в реестре движка: сцена пишет их в
// objects[i].components.testgame_*, читает, копирует (Ctrl+D, префаб) — без
// единой строки в движке. Отсутствующее в файле поле оставляет значение по
// умолчанию (старые сцены читаются); поле не того типа — исключение, и реестр
// сохраняет JSON компонента как есть, а не теряет его.
namespace {

using nlohmann::json;
using sage::scene::MakeComponentType;
using sage::scene::RegisterComponent;

json Vec3ToJson(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }

glm::vec3 Vec3FromJson(const json& j, const char* key, glm::vec3 fallback) {
    auto it = j.find(key);
    if (it == j.end()) return fallback;
    return {it->at(0).get<float>(), it->at(1).get<float>(), it->at(2).get<float>()};
}

} // namespace

void RegisterGameComponents() {
    RegisterComponent(MakeComponentType<HealthComponent>(
        "testgame_health",
        [](const HealthComponent& c) { return json{{"current", c.Current}, {"max", c.Max}}; },
        [](const json& j, HealthComponent& c) {
            c.Current = j.value("current", c.Current);
            c.Max = j.value("max", c.Max);
        }));

    RegisterComponent(MakeComponentType<PickupComponent>(
        "testgame_pickup",
        [](const PickupComponent& c) { return json{{"score", c.Score}, {"radius", c.Radius}}; },
        [](const json& j, PickupComponent& c) {
            c.Score = j.value("score", c.Score);
            c.Radius = j.value("radius", c.Radius);
        }));

    RegisterComponent(MakeComponentType<PatrolComponent>(
        "testgame_patrol",
        [](const PatrolComponent& c) {
            return json{{"pointA", Vec3ToJson(c.PointA)}, {"pointB", Vec3ToJson(c.PointB)},
                        {"speed", c.Speed}, {"damage", c.Damage}, {"towardB", c.TowardB}};
        },
        [](const json& j, PatrolComponent& c) {
            c.PointA = Vec3FromJson(j, "pointA", c.PointA);
            c.PointB = Vec3FromJson(j, "pointB", c.PointB);
            c.Speed = j.value("speed", c.Speed);
            c.Damage = j.value("damage", c.Damage);
            c.TowardB = j.value("towardB", c.TowardB);
        }));

    RegisterComponent(MakeComponentType<PortalComponent>(
        "testgame_portal",
        [](const PortalComponent& c) {
            return json{{"targetScene", c.TargetScene}, {"spawnPos", Vec3ToJson(c.SpawnPos)}, {"radius", c.Radius}};
        },
        [](const json& j, PortalComponent& c) {
            c.TargetScene = j.value("targetScene", c.TargetScene);
            c.SpawnPos = Vec3FromJson(j, "spawnPos", c.SpawnPos);
            c.Radius = j.value("radius", c.Radius);
        }));

    // Метка без полей: entt не хранит значений пустых типов, и get<T>() у них
    // не возвращает ссылку — общий MakeComponentType на ней не собирается.
    // Описание — руками, фактом наличия.
    sage::scene::ComponentType collider;
    collider.Key = "testgame_static_collider";
    collider.Has = [](const entt::registry& r, entt::entity e) { return r.all_of<StaticColliderComponent>(e); };
    collider.Save = [](const entt::registry&, entt::entity) { return json::object(); };
    collider.Load = [](entt::registry& r, entt::entity e, const json&) {
        r.emplace_or_replace<StaticColliderComponent>(e);
    };
    collider.Remove = [](entt::registry& r, entt::entity e) { r.remove<StaticColliderComponent>(e); };
    collider.Copy = [](const entt::registry&, entt::entity, entt::registry& d, entt::entity de) {
        d.emplace_or_replace<StaticColliderComponent>(de);
    };
    RegisterComponent(std::move(collider));
}
