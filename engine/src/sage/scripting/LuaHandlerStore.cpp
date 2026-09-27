#include "sage/scripting/LuaHandlerStore.h"

#include <algorithm>

namespace sage::scripting {

LuaHandlerStore::LuaHandlerStore(Invoke invoke)
    : m_shared(std::make_shared<Shared>()), m_group(sage::events::Bus::NewGroup()) {
    m_shared->Call = std::move(invoke);
}

LuaHandlerStore::~LuaHandlerStore() = default;

size_t LuaHandlerStore::Size() const { return m_shared->Fns.size(); }

int LuaHandlerStore::Connect(sage::events::Bus& bus, int object, const std::string& name,
                             sol::protected_function fn, bool once, int owner) {
    const int key = m_nextKey++;
    std::weak_ptr<Shared> weak = m_shared;
    sage::events::ConnectOptions opt;
    opt.Once = once;
    opt.Group = m_group;
    opt.Owner = owner;
    const int id = bus.Connect(object, name, [weak, key](const sage::events::Event& e) {
        std::shared_ptr<Shared> s = weak.lock();
        if (!s) return;   // хозяин интерпретатора уже ушёл
        auto it = s->Fns.find(key);
        if (it == s->Fns.end()) return;
        // Копия ДО вызова: обработчик волен подписать ещё десять функций, и
        // таблица переедет под ногами.
        const sol::protected_function call = it->second.Fn;
        const int owner = it->second.Owner;
        if (it->second.Once) s->Fns.erase(it);
        if (s->Call) s->Call(call, e, owner);
    }, opt);
    if (id == 0) return 0;
    m_shared->Fns[key] = Entry{std::move(fn), once, id, &bus, owner};

    // Соединения, снятые без ведома хранилища (объект уничтожен, disconnect
    // из скрипта), оставляют здесь функцию. Чистим изредка, по росту: игра,
    // подписывающая по обработчику на каждую пулю, иначе копила бы их до Stop.
    if (m_shared->Fns.size() >= m_pruneAt) {
        Prune(bus);
        m_pruneAt = std::max<size_t>(64, m_shared->Fns.size() * 2);
    }
    return id;
}

void LuaHandlerStore::Prune(const sage::events::Bus& bus) {
    for (auto it = m_shared->Fns.begin(); it != m_shared->Fns.end();) {
        if (it->second.Bus == &bus && !bus.IsConnected(it->second.Connection))
            it = m_shared->Fns.erase(it);
        else ++it;
    }
}

void LuaHandlerStore::Clear(sage::events::Bus* bus) {
    if (bus) bus->DisconnectGroup(m_group);
    m_shared->Fns.clear();
}

} // namespace sage::scripting
