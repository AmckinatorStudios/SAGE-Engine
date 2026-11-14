#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include <type_traits>
#include <vector>

#include "sage/core/SystemAccess.h"
#include "sage/ecs/Registry.h"
#include "sage/jobs/JobSystem.h"

// ---------------------------------------------------------------------------
// Параллельный обход сущностей и отложенные структурные правки.
//
// ЧТО БЕЗОПАСНО ПАРАЛЛЕЛЬНО В entt. Читать и менять ЗНАЧЕНИЯ существующих
// компонентов разных сущностей из разных потоков можно: каждый поток трогает
// свою память. Нельзя из параллельного кода менять СОСТАВ реестра — создавать
// и удалять сущности, добавлять и снимать компоненты: это перекладывает
// массивы пулов под ногами у соседей. Такие правки копятся в CommandBuffer и
// применяются в главном потоке после прохода (Flush), в детерминированном
// порядке.
//
// ПРОВЕРКА. ParallelEach<const A, B>(...) сверяет типы с объявлением доступа
// текущей системы: const — чтение, без const — запись. Не объявила —
// нарушение в лог (SystemAccess.h).
//
//     sage::ecs::ParallelEach<const Transform, Velocity>(reg,
//         [dt](entt::entity, const Transform& t, Velocity& v) { v.Value += t.Up() * dt; });
// ---------------------------------------------------------------------------
namespace sage::ecs {

template <class C>
inline void CheckComponentUse() {
    using Bare = std::remove_const_t<C>;
    sage::CheckComponentAccess(sage::ComponentIdOf<Bare>(), !std::is_const_v<C>);
}

// fn(entity, C&...) для каждой сущности, у которой есть все Components.
// Сущности собираются заранее (в главном потоке вызвавшего) — порядок
// обхода и разбиение на куски детерминированы.
template <class... Components, class Fn>
void ParallelEach(Registry& reg, Fn&& fn, std::size_t grain = 128,
                  sage::jobs::Category cat = sage::jobs::Category::ECS) {
    static_assert(sizeof...(Components) > 0, "ParallelEach: перечислите компоненты");
    (CheckComponentUse<Components>(), ...);
    auto view = reg.view<std::remove_const_t<Components>...>();
    std::vector<Entity> entities;
    for (Entity e : view) entities.push_back(e);
    if (entities.empty()) return;
    sage::jobs::ParallelOptions po;
    po.Grain = std::max<std::size_t>(1, grain);
    po.Cat = cat;
    po.Name = "ecs.parallel_each";
    sage::JobSystem::Get().ParallelFor(entities.size(), [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) {
            const Entity ent = entities[i];
            fn(ent, static_cast<Components&>(reg.get<std::remove_const_t<Components>>(ent))...);
        }
    }, po);
}

// Отложенные структурные правки реестра из параллельного кода.
//
// Порядок применения не зависит от того, какой поток успел первым: каждая
// правка несёт ключ (обычно — индекс сущности в обходе), Flush применяет их
// по ключу, а при равных — в порядке поступления от одного потока. Кадр
// воспроизводим при любом числе воркеров.
class CommandBuffer {
public:
    using Command = std::function<void(Registry&)>;

    void Push(uint64_t key, Command cmd) {
        std::lock_guard<sage::sync::SpinLock> lk(m_lock);
        m_cmds.push_back({key, m_seq++, std::move(cmd)});
    }
    void Destroy(uint64_t key, Entity e) {
        Push(key, [e](Registry& r) {
            if (r.valid(e)) r.destroy(e);
        });
    }
    template <class C, class... Args>
    void Emplace(uint64_t key, Entity e, Args... args) {
        Push(key, [e, args...](Registry& r) {
            if (r.valid(e)) r.emplace_or_replace<C>(e, args...);
        });
    }
    template <class C>
    void Remove(uint64_t key, Entity e) {
        Push(key, [e](Registry& r) {
            if (r.valid(e)) r.remove<C>(e);
        });
    }

    // Применить всё (главный поток, вне параллельной фазы). Возвращает число.
    std::size_t Flush(Registry& reg) {
        std::vector<Item> cmds;
        {
            std::lock_guard<sage::sync::SpinLock> lk(m_lock);
            cmds.swap(m_cmds);
            m_seq = 0;
        }
        std::stable_sort(cmds.begin(), cmds.end(), [](const Item& a, const Item& b) {
            return a.Key != b.Key ? a.Key < b.Key : a.Seq < b.Seq;
        });
        for (Item& it : cmds) it.Cmd(reg);
        return cmds.size();
    }
    std::size_t Size() const {
        std::lock_guard<sage::sync::SpinLock> lk(m_lock);
        return m_cmds.size();
    }

private:
    struct Item {
        uint64_t Key;
        uint64_t Seq;
        Command Cmd;
    };
    mutable sage::sync::SpinLock m_lock;
    std::vector<Item> m_cmds;
    uint64_t m_seq = 0;
};

} // namespace sage::ecs
