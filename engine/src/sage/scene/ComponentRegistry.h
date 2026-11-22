#pragma once
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
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
    // Снять компонент: им реестр убирает недочитанный или недокопированный
    // компонент, если Load/Copy бросили на полпути. Обязательна.
    std::function<void(entt::registry&, entt::entity)> Remove;
    // Необязательна: без неё копия идёт через Save -> Load.
    std::function<void(const entt::registry&, entt::entity, entt::registry&, entt::entity)> Copy;
    // Ссылки на другие объекты внутри компонента (необязательно).
    std::function<void(entt::registry&, entt::entity, const RefVisit&)> VisitRefs;
};

// Описание отдаётся shared_ptr'ом на НЕИЗМЕНЯЕМУЮ запись: замена или снятие
// типа (в том числе из другого потока) не трогают описание, которое кто-то
// сейчас держит, — оно живёт, пока жив последний держатель.
using ComponentTypePtr = std::shared_ptr<const ComponentType>;

// ПОТОКОБЕЗОПАСНОСТЬ. Реестр под mutex'ом, но пользовательские Has/Save/Load/
// Copy/VisitRefs/Remove вызываются ВНЕ блокировки: обход идёт по снимку
// списка, снятому под блокировкой. Пользовательский код может сам звать
// реестр без взаимоблокировки. Встроенные типы движка регистрируются ровно
// один раз при первом обращении к реестру, из любого потока.
//
// Тот же ключ второй раз — ЗАМЕНА на месте (порядок записи сохраняется): игра
// вправе переопределить свой тип или встроенный. Неполное описание (нет ключа,
// Has, Save, Load или Remove) отвергается: false и строка в лог.
//
// Снятие типа не трогает уже созданные компоненты в сценах: пока тип снят,
// сохранение их не пишет. Снимать тип, пока его сцены живы, — ошибка игры.
bool RegisterComponent(ComponentType type);
bool UnregisterComponent(const std::string& key);
std::vector<ComponentTypePtr> ComponentTypes();
ComponentTypePtr FindComponentType(const std::string& key);

// Компоненты файла, которых нет в реестре: сохраняются как есть.
struct UnknownComponents {
    nlohmann::json Data = nlohmann::json::object();   // ключ -> JSON
};

// Все ссылки сущности: публичные переменные, связи сигналов, компоненты
// реестра. Тем, кто переписывает ссылки (копирование, вставка), — только
// через неё. Ссылки внутри UnknownComponents не видны: их устройство знает
// только отсутствующий код (такой JSON копируется как есть).
void VisitEntityRefs(entt::registry& reg, entt::entity e, const RefVisit& visit);

// Для сериализатора и копирования (не для игры).
//
// Save: ошибка Save любого компонента — исключение std::runtime_error с
// ключом компонента; object при этом не меняется. Молча пропустить нельзя:
// сохранение без компонента стирает его из файла.
// Load: компонент, чей Load бросил, снимается (не остаётся недочитанным), а
// его JSON уходит в UnknownComponents — следующее сохранение запишет его как
// был. Сама функция не бросает из-за чужого компонента.
// Copy: ошибка — исключение; компонент, на котором упало, снят с de.
// Откат остального (уже скопированного) — забота вызывающего (CopySubtree
// уничтожает всё созданное).
void SaveRegisteredComponents(const entt::registry& reg, entt::entity e, nlohmann::json& object);
void LoadRegisteredComponents(entt::registry& reg, entt::entity e, const nlohmann::json& object);
void CopyRegisteredComponents(const entt::registry& src, entt::entity se, entt::registry& dst, entt::entity de);

// СОСТОЯНИЕ ЭКЗЕМПЛЯРА ПРИ КОПИИ.
//
// Копия объекта (Ctrl+D, префаб, вставка, перенос между сценами) берёт
// НАСТРОЙКИ компонента, но не его рантайм: дескриптор физического тела, голос
// звука, загруженную модель, «уже запущено». Что из полей рантайм — знает
// только сам компонент, поэтому он и объявляет
//     void ResetInstanceState();
// а копирование зовёт его у копии. Раньше этот список жил в копировании сцены
// (Prefab.cpp) и знал внутренности физики, звука, анимации и рендера — и
// отставал от них: новое рантайм-поле компонента копия наследовала молча.
template <typename T, typename = void>
struct HasInstanceState : std::false_type {};
template <typename T>
struct HasInstanceState<T, std::void_t<decltype(std::declval<T&>().ResetInstanceState())>>
    : std::true_type {};

// Копия компонента T с s на d (если он есть у s): настройки — как есть,
// состояние экземпляра — сброшено (см. выше).
template <typename T>
void CopyComponent(const entt::registry& s, entt::entity se, entt::registry& d, entt::entity de) {
    const T* c = s.try_get<T>(se);
    if (!c) return;
    T& copy = d.emplace_or_replace<T>(de, *c);
    if constexpr (HasInstanceState<T>::value) copy.ResetInstanceState();
}

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
    t.Remove = [](entt::registry& r, entt::entity e) { r.remove<T>(e); };
    t.Copy = [](const entt::registry& s, entt::entity se, entt::registry& d, entt::entity de) {
        CopyComponent<T>(s, se, d, de);
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
