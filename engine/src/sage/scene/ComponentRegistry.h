#pragma once
#include <functional>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

#include "sage/vars/Value.h"

// ---------------------------------------------------------------------------
// РЕЕСТР КОМПОНЕНТОВ: свой компонент игры — наравне с движковыми.
//
// ЗАЧЕМ. Компонент, которого движок не знал по имени, жил только в памяти:
// сохранение сцены его не писало, копирование (Ctrl+D, префаб, вставка,
// перенос между сценами) теряло, ссылки на объекты внутри него при копии не
// переписывались. Добавить игре свой компонент значило править
// SceneSerializer.cpp, Prefab.cpp и Refs.cpp движка — то есть держать свою
// копию движка. А встроенные списки компонентов в этих файлах расходились
// между собой (так копия теряла тег, звук и твины — test_scene_lossless.cpp).
//
// Теперь компонент описывается ОДИН раз:
//
//     struct Health { float Value = 100; sage::vars::EntityRef LastHitBy; };
//
//     sage::scene::RegisterComponent(sage::scene::MakeComponentType<Health>(
//         "health",
//         [](const Health& h) { return nlohmann::json{{"value", h.Value}}; },
//         [](const nlohmann::json& j, Health& h) { h.Value = j.value("value", h.Value); },
//         [](Health& h, const auto& visit) { visit(h.LastHitBy); }));   // ссылки — по желанию
//
// и дальше сцена сама его пишет (objects[i].components.health), читает,
// копирует и переписывает в нём ссылки. Регистрировать — до загрузки первой
// сцены (в начале игры, рядом с RegisterIcon и RegisterPart).
//
// УСТОЙЧИВОСТЬ К ЧУЖИМ ФАЙЛАМ. Компонент, которого НЕТ в реестре (сцену
// открыли в редакторе без кода игры; компонент убрали из игры; файл от более
// новой версии), не теряется: его JSON хранится как есть (UnknownComponents) и
// пишется обратно при сохранении и копируется вместе с объектом. Иначе одно
// открытие сцены «не той» программой молча стирало бы данные игры.
// ---------------------------------------------------------------------------
namespace sage::scene {

using RefVisit = std::function<void(sage::vars::EntityRef&)>;

struct ComponentType {
    std::string Key;   // имя в файле: objects[i].components[Key]
    std::function<bool(const entt::registry&, entt::entity)> Has;
    std::function<nlohmann::json(const entt::registry&, entt::entity)> Save;
    std::function<void(entt::registry&, entt::entity, const nlohmann::json&)> Load;
    std::function<void(const entt::registry&, entt::entity, entt::registry&, entt::entity)> Copy;
    // Ссылки на другие объекты внутри компонента (необязательно).
    std::function<void(entt::registry&, entt::entity, const RefVisit&)> VisitRefs;
};

// Тот же ключ второй раз — замена: игра вправе переопределить свой тип.
void RegisterComponent(ComponentType type);
bool UnregisterComponent(const std::string& key);
const std::vector<ComponentType>& ComponentTypes();
const ComponentType* FindComponentType(const std::string& key);

// Компоненты файла, которых нет в реестре: сохраняются как есть.
struct UnknownComponents {
    nlohmann::json Data = nlohmann::json::object();   // ключ -> JSON
};

// Все ссылки сущности: публичные переменные, связи сигналов, компоненты
// реестра. Тем, кто переписывает ссылки (копирование, вставка), — только
// через неё.
void VisitEntityRefs(entt::registry& reg, entt::entity e, const RefVisit& visit);

// Для сериализатора и копирования (не для игры).
void SaveRegisteredComponents(const entt::registry& reg, entt::entity e, nlohmann::json& object);
void LoadRegisteredComponents(entt::registry& reg, entt::entity e, const nlohmann::json& object);
void CopyRegisteredComponents(const entt::registry& src, entt::entity se, entt::registry& dst, entt::entity de);

// Удобная сборка описания для обычной структуры T.
template <typename T, typename SaveFn, typename LoadFn>
ComponentType MakeComponentType(std::string key, SaveFn save, LoadFn load) {
    ComponentType t;
    t.Key = std::move(key);
    t.Has = [](const entt::registry& r, entt::entity e) { return r.all_of<T>(e); };
    t.Save = [save](const entt::registry& r, entt::entity e) { return nlohmann::json(save(r.get<T>(e))); };
    t.Load = [load](entt::registry& r, entt::entity e, const nlohmann::json& j) {
        T value{};
        load(j, value);
        r.emplace_or_replace<T>(e, std::move(value));
    };
    t.Copy = [](const entt::registry& s, entt::entity se, entt::registry& d, entt::entity de) {
        d.emplace_or_replace<T>(de, s.get<T>(se));
    };
    return t;
}

template <typename T, typename SaveFn, typename LoadFn, typename RefsFn>
ComponentType MakeComponentType(std::string key, SaveFn save, LoadFn load, RefsFn refs) {
    ComponentType t = MakeComponentType<T>(std::move(key), save, load);
    t.VisitRefs = [refs](entt::registry& r, entt::entity e, const RefVisit& visit) {
        if (T* c = r.try_get<T>(e)) refs(*c, visit);
    };
    return t;
}

} // namespace sage::scene
