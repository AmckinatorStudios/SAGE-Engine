#include "sage/scripting/LuaHandlerStore.h"

#include <algorithm>

namespace sage::scripting {

LuaHandlerStore::LuaHandlerStore(Invoke invoke)
    : m_shared(std::make_shared<Shared>()), m_group(sage::events::Bus::NewGroup()) {
    m_shared->Call = std::move(invoke);
}

LuaHandlerStore::~LuaHandlerStore() = default;

size_t LuaHandlerStore::Size() const { return m_shared->Fns.size(); }

bool LuaHandlerStore::AllOnMainThread() const {
    for (const auto& [key, e] : m_shared->Fns) {
        lua_State* L = e.Fn.lua_state();
        if (L && sol::main_thread(L, L) != L) return false;
    }
    return true;
}

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
    // ССЫЛКА — НА ГЛАВНЫЙ ПОТОК Lua, а не на тот, из которого подписали.
    // sol::protected_function помнит lua_State, на котором её взяли, и снимает
    // ссылку (luaL_unref) через него же. Подписка из корутины (тесты на Lua
    // идут корутинами, игра — тоже, когда ждёт) запоминала поток корутины;
    // корутину собирал сборщик мусора, а при выходе из Play Clear() звал
    // luaL_unref на освобождённом потоке — и писал в чужую память. Куча
    // портилась молча, а падало потом где попало: в деструкторе Jolt, при
    // закрытии Lua (AddressSanitizer: heap-use-after-free в lua_rawgeti).
    //
    // НОВОЙ ПЕРЕМЕННОЙ, а не присваиванием: operator= у ссылок sol2 оставляет
    // ссылке ЕЁ прежний поток и переносит значение туда же.
    lua_State* L = fn.lua_state();
    lua_State* main = L ? sol::main_thread(L, L) : nullptr;
    if (main && main != L) {
        // Через стек главного потока: реестр у потоков общий, и ссылка,
        // взятая заново на main, принадлежит уже ему.
        fn.push(main);
        sol::protected_function onMain(main, -1);
        lua_pop(main, 1);
        m_shared->Fns.emplace(key, Entry{std::move(onMain), once, id, &bus, owner});
    } else {
        m_shared->Fns.emplace(key, Entry{std::move(fn), once, id, &bus, owner});
    }

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
