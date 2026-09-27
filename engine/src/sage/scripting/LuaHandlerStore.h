#pragma once
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include <sol/sol.hpp>

#include "sage/events/Events.h"

// ---------------------------------------------------------------------------
// ФУНКЦИИ LUA, ПОДПИСАННЫЕ НА ШИНУ СЦЕНЫ.
//
// Шина принадлежит сцене, а сцена ПЕРЕЖИВАЕТ интерпретатор Lua: Stop в
// редакторе сносит скрипты и возвращает ту же сцену. Если бы обработчик шины
// держал sol-функцию прямо в своём замыкании, при разрушении сцены он снимал бы
// ссылку в уже мёртвом интерпретаторе — падение при выходе, когда всё сделано.
//
// Поэтому сами функции лежат ЗДЕСЬ (у хозяина интерпретатора, объявленного
// раньше), а в шину уходит замыкание со СЛАБОЙ ссылкой на этот блок и номером.
// Хранилище умерло — замыкание молча ничего не делает, а снять его можно когда
// угодно. Тот же приём, что у запросов контроллера персонажа (SolidQueries).
//
// Одно хранилище на подсистему: у него своя группа шины (Bus::NewGroup), и
// Clear/DisconnectGroup снимают всё, что подсистема подписала, разом.
// ---------------------------------------------------------------------------
namespace sage::scripting {

class LuaHandlerStore {
public:
    // Как позвать функцию с событием: каждый API решает сам, в каком виде
    // событие видно скрипту (таблица-событие нового API, таблица-нагрузка
    // старого sage.events).
    // owner — кто подписал (экземпляр скрипта): его подписки, заведённые
    // изнутри обработчика, принадлежат ему же и уйдут вместе с ним.
    using Invoke = std::function<void(const sol::protected_function&, const sage::events::Event&,
                                      int owner)>;

    explicit LuaHandlerStore(Invoke invoke);
    ~LuaHandlerStore();
    LuaHandlerStore(const LuaHandlerStore&) = delete;
    LuaHandlerStore& operator=(const LuaHandlerStore&) = delete;

    // Номер соединения шины (0 — не заведено: причина в логе шины).
    int Connect(sage::events::Bus& bus, int object, const std::string& name,
                sol::protected_function fn, bool once, int owner);

    int Group() const { return m_group; }
    size_t Size() const;

    // Забыть функции соединений, которых в шине уже нет (сняты, сработали как
    // одноразовые, объект уничтожен). Зовётся изредка — см. Connect.
    void Prune(const sage::events::Bus& bus);

    // Снять все подписки хранилища с шины и забыть функции.
    void Clear(sage::events::Bus* bus);

private:
    struct Entry {
        sol::protected_function Fn;
        bool Once = false;
        int Connection = 0;
        const sage::events::Bus* Bus = nullptr;   // шина соединения (их может быть две)
        int Owner = 0;
    };
    struct Shared {
        Invoke Call;
        std::unordered_map<int, Entry> Fns;   // по своему ключу, не по номеру шины
    };
    std::shared_ptr<Shared> m_shared;
    int m_group = 0;
    int m_nextKey = 1;
    size_t m_pruneAt = 64;
};

} // namespace sage::scripting
