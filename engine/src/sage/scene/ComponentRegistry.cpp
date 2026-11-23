#include "sage/scene/ComponentRegistry.h"
#include "sage/scene/SceneLayer.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>

#include "sage/core/Log.h"
#include "sage/render/ObjectEffects.h"
#include "sage/scene/AiAgentComponent.h"
#include "sage/scene/BuiltinComponents.h"
#include "sage/scene/VisualScriptComponent.h"
#include "sage/scene/Signals.h"
#include "sage/vars/Refs.h"

namespace sage::scene {

namespace {

using TypeList = std::vector<ComponentTypePtr>;
using Snapshot = std::shared_ptr<const TypeList>;

// Список хранится неизменяемым снимком: запись подменяет снимок целиком под
// блокировкой, чтение берёт под ней копию shared_ptr (одна атомарная операция)
// и дальше обходит список без блокировки. Так пользовательские функции
// компонентов не зовутся под mutex'ом, а замена/снятие типа во время чужого
// обхода не инвалидирует ни список, ни описание.
struct Registry {
    std::mutex Lock;
    Snapshot Types = std::make_shared<const TypeList>();
};

Registry& Reg() {
    static Registry r;
    return r;
}

// Поток, который сейчас регистрирует встроенные: их Register* снова зовут
// RegisterComponent, и повторный вход в call_once был бы взаимоблокировкой.
thread_local bool t_registeringBuiltins = false;

// Встроенные компоненты, описанные реестром, — при первом обращении к нему:
// статический инициализатор в другом файле статической библиотеки
// компоновщик вправе выбросить, и компонент молча перестал бы писаться.
// Прежний `static bool done` не был потокобезопасным: два потока могли
// зарегистрировать встроенные дважды или прочитать реестр без них.
void EnsureBuiltins() {
    if (t_registeringBuiltins) return;
    static std::once_flag once;
    std::call_once(once, [] {
        t_registeringBuiltins = true;
        struct Reset {
            ~Reset() { t_registeringBuiltins = false; }
        } reset;
        RegisterAiAgentComponent();
        RegisterSceneLayerComponent();
        RegisterVisualScriptComponent();
        sage::render::RegisterObjectEffectsComponent();
        RegisterBuiltinSceneComponents();
    });
}

Snapshot Types() {
    EnsureBuiltins();
    Registry& r = Reg();
    std::lock_guard<std::mutex> lock(r.Lock);
    return r.Types;
}

// Пользовательская функция компонента: любое исключение (и не std тоже)
// превращается в runtime_error с ключом компонента — вызывающий решает, что
// делать, а текст говорит, какой компонент и на каком шаге.
template <typename Fn>
auto CallUser(const ComponentType& t, const char* step, Fn&& fn) -> decltype(fn()) {
    try {
        return fn();
    } catch (const std::exception& ex) {
        throw std::runtime_error("компонент «" + t.Key + "»: " + step + ": " + ex.what());
    } catch (...) {
        throw std::runtime_error("компонент «" + t.Key + "»: " + step + ": неизвестное исключение");
    }
}

// Снять недоделанный компонент. Сбой самого снятия — тоже ошибка: её нельзя
// проглотить, но и прятать за ней исходную причину нельзя — пишем в лог.
void RemovePartial(const ComponentType& t, entt::registry& reg, entt::entity e) {
    try {
        if (t.Has(reg, e)) t.Remove(reg, e);
    } catch (const std::exception& ex) {
        LOG_ERROR("Scene") << "компонент «" << t.Key << "»: не снялся после сбоя: " << ex.what();
    } catch (...) {
        LOG_ERROR("Scene") << "компонент «" << t.Key << "»: не снялся после сбоя (неизвестное исключение)";
    }
}

} // namespace

bool RegisterComponent(ComponentType type) {
    if (type.Key.empty() || !type.Has || !type.Save || !type.Load || !type.Remove) {
        LOG_ERROR("Scene") << "RegisterComponent: у типа «" << type.Key
                           << "» нет ключа или функций Has/Save/Load/Remove — не зарегистрирован";
        return false;
    }
    EnsureBuiltins();
    auto entry = std::make_shared<const ComponentType>(std::move(type));
    Registry& r = Reg();
    std::lock_guard<std::mutex> lock(r.Lock);
    auto next = std::make_shared<TypeList>(*r.Types);
    auto it = std::find_if(next->begin(), next->end(),
                           [&](const ComponentTypePtr& t) {
                               // Встроенный «audio» в объекте и «audio» игры в components —
                               // разные записи файла; одна не заменяет другую.
                               return t->Key == entry->Key && t->TopLevel == entry->TopLevel;
                           });
    if (it != next->end()) *it = std::move(entry);
    else next->push_back(std::move(entry));
    r.Types = std::move(next);
    return true;
}

bool UnregisterComponent(const std::string& key) {
    EnsureBuiltins();
    Registry& r = Reg();
    std::lock_guard<std::mutex> lock(r.Lock);
    auto next = std::make_shared<TypeList>(*r.Types);
    // Сначала тип игры (components), потом встроенный верхнего уровня.
    auto it = std::find_if(next->begin(), next->end(),
                           [&](const ComponentTypePtr& t) { return t->Key == key && !t->TopLevel; });
    if (it == next->end())
        it = std::find_if(next->begin(), next->end(), [&](const ComponentTypePtr& t) { return t->Key == key; });
    if (it == next->end()) return false;
    next->erase(it);
    r.Types = std::move(next);
    return true;
}

std::vector<ComponentTypePtr> ComponentTypes() { return *Types(); }

ComponentTypePtr FindComponentType(const std::string& key) {
    const Snapshot all = Types();
    ComponentTypePtr top;
    for (const ComponentTypePtr& t : *all) {
        if (t->Key != key) continue;
        if (!t->TopLevel) return t;   // тип игры (components) — первым, как в Unregister
        if (!top) top = t;
    }
    return top;
}

void VisitEntityRefs(entt::registry& reg, entt::entity e, const RefVisit& visit) {
    if (!visit || !reg.valid(e)) return;
    // Публичные переменные — забота vars (он ниже сцены и про сцену не знает).
    sage::vars::VisitEntityRefs(reg, e, visit);
    // Цели связей сигналов: копия кнопки из префаба обязана звать СВОЮ дверь,
    // а не дверь оригинала.
    if (sage::signals::SignalLinksComponent* sl = reg.try_get<sage::signals::SignalLinksComponent>(e))
        for (sage::signals::Link& link : sl->Links) visit(link.Target);
    const Snapshot all = Types();
    for (const ComponentTypePtr& t : *all)
        if (t->VisitRefs && t->Has(reg, e)) t->VisitRefs(reg, e, visit);
}

void SaveRegisteredComponents(const entt::registry& reg, entt::entity e, nlohmann::json& object) {
    nlohmann::json comps = nlohmann::json::object();
    // Сначала неизвестные — чтобы зарегистрированный тип с тем же ключом
    // (код игры вернулся) записал своё, свежее.
    if (const UnknownComponents* u = reg.try_get<UnknownComponents>(e)) {
        for (auto it = u->Data.begin(); it != u->Data.end(); ++it) comps[it.key()] = it.value();
        for (auto it = u->TopLevel.begin(); it != u->TopLevel.end(); ++it) object[it.key()] = it.value();
    }
    const Snapshot all = Types();
    for (const ComponentTypePtr& tp : *all) {
        const ComponentType& t = *tp;
        if (!CallUser(t, "Has", [&] { return t.Has(reg, e); })) continue;
        // Ошибка НЕ глотается: сцена без компонента, записанная поверх
        // прежнего файла, стёрла бы его данные. Сохранение целиком отменяется.
        nlohmann::json data = CallUser(t, "сохранение", [&] { return t.Save(reg, e); });
        if (!t.TopLevel) comps[t.Key] = std::move(data);
        else if (!t.Fields.empty() && data.is_object())
            for (auto it = data.begin(); it != data.end(); ++it) object[it.key()] = it.value();
        else if (!t.Fields.empty() && !data.is_null())
            throw std::runtime_error("компонент «" + t.Key + "»: сохранение вернуло не объект");
        else if (!data.is_null()) object[t.Key] = std::move(data);
    }
    if (!comps.empty()) object["components"] = std::move(comps);
}

namespace {

// Прочитать один компонент. Не прочитался — компонент снимаем (недочитанный
// хуже отсутствующего: игра работала бы с мусором), JSON храним как есть в
// unknown: следующее сохранение не имеет права стереть то, что этот код не понял.
void LoadOne(const ComponentType& t, entt::registry& reg, entt::entity e, const std::string& key,
             const nlohmann::json& data, nlohmann::json& unknown) {
    bool had = false;
    try {
        had = CallUser(t, "Has", [&] { return t.Has(reg, e); });
        CallUser(t, "загрузка", [&] { t.Load(reg, e, data); });
    } catch (const std::exception& ex) {
        if (!had) RemovePartial(t, reg, e);
        LOG_ERROR("Scene") << ex.what() << " — сохранён как есть";
        if (t.Fields.empty()) unknown[key] = data;
        else   // составной: каждый ключ — на своё место в объекте
            for (auto it = data.begin(); it != data.end(); ++it) unknown[it.key()] = it.value();
    }
}

} // namespace

void LoadRegisteredComponents(entt::registry& reg, entt::entity e, const nlohmann::json& object) {
    const Snapshot all = Types();
    nlohmann::json unknown = nlohmann::json::object();
    nlohmann::json unknownTop = nlohmann::json::object();
    // Верхний уровень: ключ ищется в самом объекте; нет ключа — нет компонента.
    for (const ComponentTypePtr& tp : *all) {
        if (!tp->TopLevel) continue;
        if (!tp->Fields.empty()) {
            nlohmann::json part = nlohmann::json::object();
            for (const std::string& k : tp->Fields)
                if (auto f = object.find(k); f != object.end()) part[k] = *f;
            if (!part.empty()) LoadOne(*tp, reg, e, tp->Key, part, unknownTop);
            continue;
        }
        std::string key = tp->Key;
        auto f = object.find(key);
        for (size_t i = 0; f == object.end() && i < tp->OldKeys.size(); ++i)
            f = object.find(key = tp->OldKeys[i]);
        if (f != object.end()) LoadOne(*tp, reg, e, key, *f, unknownTop);
    }
    auto it = object.find("components");
    if (it != object.end() && !it->is_object()) {
        // Не объект — ключей компонентов в нём нет, читать нечего; причина — в лог.
        LOG_ERROR("Scene") << "поле components объекта — не объект, пропущено";
    } else if (it != object.end()) {
        for (auto c = it->begin(); c != it->end(); ++c) {
            ComponentTypePtr tp;
            for (const ComponentTypePtr& t : *all)
                if (!t->TopLevel && t->Key == c.key()) {
                    tp = t;
                    break;
                }
            if (!tp) unknown[c.key()] = c.value();
            else LoadOne(*tp, reg, e, c.key(), c.value(), unknown);
        }
    }
    if (!unknown.empty() || !unknownTop.empty())
        reg.emplace_or_replace<UnknownComponents>(e, UnknownComponents{std::move(unknown), std::move(unknownTop)});
}

void CopyRegisteredComponents(const entt::registry& src, entt::entity se, entt::registry& dst, entt::entity de) {
    if (const UnknownComponents* u = src.try_get<UnknownComponents>(se)) dst.emplace_or_replace<UnknownComponents>(de, *u);
    const Snapshot all = Types();
    for (const ComponentTypePtr& tp : *all) {
        const ComponentType& t = *tp;
        if (!CallUser(t, "Has", [&] { return t.Has(src, se); })) continue;
        try {
            if (t.Copy) {
                CallUser(t, "копирование", [&] { t.Copy(src, se, dst, de); });
            } else {
                // Без своей копии — через файл: медленнее, но ничего не теряется.
                nlohmann::json j = CallUser(t, "сохранение", [&] { return t.Save(src, se); });
                CallUser(t, "загрузка", [&] { t.Load(dst, de, j); });
            }
        } catch (...) {
            // Недокопированный компонент не оставляем; остальное откатывает
            // вызывающий. Ошибка уходит наверх — это не пустой перехват.
            RemovePartial(t, dst, de);
            throw;
        }
    }
}

} // namespace sage::scene
