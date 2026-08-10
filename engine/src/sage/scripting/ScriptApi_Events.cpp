#include "ScriptEngine.h"

#include "sage/core/Log.h"

// ---------------------------------------------------------------------------
// События: sage.events.*
//
// ЗАЧЕМ ЭТО ПОЯВИЛОСЬ. До сих пор скрипты узнавали о происходящем ровно двумя
// способами, и оба плохи.
//
// Первый — ОПРОС каждый кадр. Столкновения приходилось разбирать вызовом
// sage.physics.Collisions() в OnUpdate; «игрок приземлился» вычислялось
// сравнением флага с прошлым кадром; «нажали кнопку действия» — тем же
// сравнением. Каждый такой самодельный детектор края теряет событие на
// длинном кадре и повторяет его на коротком, а главное — его пишет заново
// каждая игра.
//
// Второй — SendMessage/Broadcast между скриптами. Он адресуется СУЩНОСТЯМ:
// чтобы услышать сообщение, надо быть скриптом на объекте. Уровневый скрипт
// (правила игры, интерфейс, автосохранение) в этой схеме — гражданин второго
// сорта, а подписаться «на всё, что происходит в мире» нельзя вовсе.
//
// Здесь — обычная шина: подписался по имени, получил вызов. Подписчиком может
// быть кто угодно, включая замыкание внутри модуля, у которого нет ни
// сущности, ни файла-скрипта. Свои события игра шлёт тем же Emit, что и
// движок, — и её собственная логика ничем не отличается от встроенной.
//
// ПОЧЕМУ ВЫЗОВ, А НЕ ОЧЕРЕДЬ. Обработчики зовутся сразу из Emit, и это
// безопасно ровно потому, что движок сам никогда не зовёт Emit из чужого
// потока: физика отдаёт контакты опросом на главном потоке (см. PollContacts),
// а всё остальное и так живёт в кадре. Очередь добавила бы кадр задержки
// каждому событию — и «звук удара» отставал бы от удара.
//
// ВСТРОЕННЫЕ СОБЫТИЯ (движок шлёт их сам, игре достаточно подписаться):
//   "collision"        {a, b, began, point, normal, impulse}   — удар тел
//   "trigger"          {a, b, entered, point}                  — зона-сенсор
//   "character.land"   {object, velocity}                      — приземлился
//   "character.leave"  {object}                                — оторвался от опоры
//   "character.step"   {object, height}                        — взошёл на ступеньку
//   "character.blocked"{object}                                — упёрся в стену
//   "action.pressed"   {action}                                — нажато действие ввода
//   "action.released"  {action}
//   "pause"            {paused}
//   "quit"             {}
// ---------------------------------------------------------------------------

void ScriptEngine::RegisterEventsApi() {
    // Подписка. Возвращает номер, по которому подписку снимают: возвращать
    // саму функцию нельзя — Lua сравнивает замыкания по идентичности, и снять
    // подписку на `function() ... end`, объявленную по месту, было бы уже
    // невозможно.
    Bind("events", "On", "OnEvent", [this](const std::string& name, sol::protected_function fn) -> int {
        if (!fn.valid()) throw std::runtime_error("sage.events.On: обработчик не функция");
        const int id = m_nextHandler++;
        m_handlers[name].push_back({id, fn, false});
        return id;
    });

    // Одноразовая подписка: сработала — снялась. Без неё «дождаться первого
    // касания земли» пишется как подписка, которая первым делом снимает сама
    // себя, и в этой строчке ошибаются все.
    Bind("events", "Once", "OnceEvent", [this](const std::string& name, sol::protected_function fn) -> int {
        if (!fn.valid()) throw std::runtime_error("sage.events.Once: обработчик не функция");
        const int id = m_nextHandler++;
        m_handlers[name].push_back({id, fn, true});
        return id;
    });

    Bind("events", "Off", "OffEvent", [this](sol::object arg) {
        // Снять можно по номеру подписки или по имени события целиком.
        if (arg.is<int>()) {
            const int id = arg.as<int>();
            for (auto& [name, list] : m_handlers)
                for (Handler& h : list)
                    if (h.Id == id) h.Dead = true;
            return;
        }
        if (arg.is<std::string>()) {
            auto it = m_handlers.find(arg.as<std::string>());
            if (it != m_handlers.end())
                for (Handler& h : it->second) h.Dead = true;
        }
    });

    // Рассылка. Полезная нагрузка — одно значение (обычно таблица): набор
    // аргументов переменной длины заставил бы каждого подписчика знать
    // порядок и число полей отправителя, а таблица позволяет добавить поле,
    // не сломав никого.
    Bind("events", "Emit", "EmitEvent", [this](const std::string& name, sol::object payload) {
        DispatchEvent(name, payload);
    });

    // Сколько живых подписчиков у события. Нужно не для отладки: рассылка
    // тяжёлой нагрузки (собрать таблицу с десятком полей) имеет смысл только
    // если её кто-то слушает.
    Bind("events", "Count", "EventCount", [this](const std::string& name) -> int {
        auto it = m_handlers.find(name);
        if (it == m_handlers.end()) return 0;
        int n = 0;
        for (const Handler& h : it->second)
            if (!h.Dead) ++n;
        return n;
    });
}

// Рассылка одного события. Обработчики КОПИРУЮТСЯ перед вызовом: подписка или
// отписка изнутри обработчика — обычное дело («сработало — отпишись»), а она
// меняет вектор, по которому мы идём. Без копии первый же такой обработчик
// оставлял бы висячую ссылку.
void ScriptEngine::DispatchEvent(const std::string& name, sol::object payload) {
    auto it = m_handlers.find(name);
    if (it == m_handlers.end()) return;

    // Ограничение вложенности — по той же причине, что у SendMessage: два
    // обработчика, шлющие событие друг другу, иначе уронят движок
    // переполнением стека C++, а не понятной ошибкой в логе.
    if (m_eventDepth >= kMaxEventDepth) {
        LOG_ERROR("ScriptEngine") << "sage.events: превышена глубина вложенной рассылки ("
                                  << kMaxEventDepth << ") на событии '" << name
                                  << "' — обработчики шлют события друг другу по кругу";
        return;
    }
    ++m_eventDepth;

    std::vector<Handler> snapshot;
    snapshot.reserve(it->second.size());
    for (const Handler& h : it->second)
        if (!h.Dead) snapshot.push_back(h);

    for (const Handler& h : snapshot) {
        sol::protected_function_result r = payload.valid() ? h.Fn(payload) : h.Fn();
        if (!r.valid()) {
            const sol::error err = r;
            LOG_ERROR("ScriptEngine") << "Ошибка в обработчике события '" << name
                                      << "': " << err.what();
        }
        if (h.Once) {
            for (Handler& live : it->second)
                if (live.Id == h.Id) live.Dead = true;
        }
    }

    --m_eventDepth;

    // Снятые подписки убираем ОДНИМ проходом и только на верхнем уровне
    // рассылки: вложенный Emit по тому же событию иначе выдернул бы вектор
    // из-под внешнего цикла.
    if (m_eventDepth == 0) {
        for (auto& [key, list] : m_handlers) {
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [](const Handler& h) { return h.Dead; }),
                       list.end());
        }
    }
}

void ScriptEngine::DispatchEvent(const std::string& name, bool flag) {
    sol::table t = m_lua.create_table();
    t["value"] = flag;
    DispatchEvent(name, t);
}

// --- Встроенные события кадра -------------------------------------------------
//
// Зовётся хозяином кадра ПОСЛЕ физики и ввода, но ДО пользовательских OnUpdate:
// скрипт, обрабатывающий столкновение, должен успеть отреагировать в том же
// кадре, в котором оно случилось, — иначе взрыв отстаёт от удара на кадр, и
// это видно.
void ScriptEngine::DispatchFrameEvents() {
    if (!m_scene) return;

    // 1. Столкновения и зоны-сенсоры.
    if (m_physics) {
        for (const PhysicsScene::EntityContact& c : m_physics->Contacts()) {
            const char* name = c.Sensor ? "trigger" : "collision";
            if (m_handlers.find(name) == m_handlers.end()) continue;
            sol::table t = m_lua.create_table();
            t["a"] = GameObject(&m_scene->Registry(), c.A);
            t["b"] = GameObject(&m_scene->Registry(), c.B);
            if (c.Sensor) t["entered"] = c.Begin;
            else t["began"] = c.Begin;
            t["point"] = c.Point;
            t["normal"] = c.Normal;
            t["impulse"] = c.Impulse;
            DispatchEvent(name, t);
        }
    }

    // 2. Края контроллера персонажа. Их считает мотор (см. CharacterMotor):
    // «приземлился» и «взошёл на ступеньку» — края, а край, вычисленный
    // снаружи опросом раз в кадр, теряется ровно на длинном кадре, когда
    // просадка и без того портит впечатление.
    auto view = m_scene->Registry().view<CharacterControllerComponent>();
    for (auto e : view) {
        const CharacterControllerComponent& cc = view.get<CharacterControllerComponent>(e);
        if (!cc.Landed && !cc.LeftGround && !cc.Blocked && cc.StepUp <= 0.0f) continue;
        GameObject obj(&m_scene->Registry(), e);
        if (cc.Landed) {
            sol::table t = m_lua.create_table();
            t["object"] = obj;
            DispatchEvent("character.land", t);
        }
        if (cc.LeftGround) {
            sol::table t = m_lua.create_table();
            t["object"] = obj;
            DispatchEvent("character.leave", t);
        }
        if (cc.StepUp > 0.0f) {
            sol::table t = m_lua.create_table();
            t["object"] = obj;
            t["height"] = cc.StepUp;
            DispatchEvent("character.step", t);
        }
        if (cc.Blocked) {
            sol::table t = m_lua.create_table();
            t["object"] = obj;
            DispatchEvent("character.blocked", t);
        }
    }

    // 3. Именованные действия ввода. Событием, а не опросом: «нажал» — это
    // край, и на просадке WasActionPressed его теряет так же, как всё
    // остальное.
    if (m_input) {
        const bool wantPressed = m_handlers.count("action.pressed") != 0;
        const bool wantReleased = m_handlers.count("action.released") != 0;
        if (wantPressed || wantReleased) {
            for (const auto& [name, action] : m_input->All()) {
                if (wantPressed && action.WasPressed()) {
                    sol::table t = m_lua.create_table();
                    t["action"] = name;
                    DispatchEvent("action.pressed", t);
                }
                if (wantReleased && action.WasReleased()) {
                    sol::table t = m_lua.create_table();
                    t["action"] = name;
                    DispatchEvent("action.released", t);
                }
            }
        }
    }
}
