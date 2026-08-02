#include "ScriptEngine.h"

#include "sage/core/Log.h"
#include <algorithm>
#include <cctype>

// ---------------------------------------------------------------------------
// Время, сообщения, параметры запуска: sage.time.*, sage.msg.*, sage.app.*
//
// Часть Lua-API движка. Раньше ВСЕ привязки жили в одном ScriptEngine.cpp на
// 1800 строк: 126 функций, восемнадцать областей, и чтобы дописать одну
// строчку про анимацию, приходилось листать интерфейс, физику и таймеры.
// Определения разъехались по файлам ScriptApi_*.cpp — по файлу на область;
// объявления методов остались в ScriptEngine.h, поэтому порядок регистрации
// по-прежнему записан в одном месте (RegisterEngineApi) и не зависит от того,
// в каком файле лежит тело.
// ---------------------------------------------------------------------------

void ScriptEngine::RegisterTimerApi() {
    // --- Таймеры: отложенные/повторяющиеся вызовы без ручного хранения
    // "сколько осталось" в самом скрипте. Возвращают id для CancelTimer. ---
    Bind("time", "Schedule", "Schedule", [this](float seconds, sol::protected_function fn) -> int {
        int id = m_nextTimerId++;
        m_scheduled.push_back({id, seconds, 0.0f, false, false, std::move(fn)});
        return id;
    });
    Bind("time", "Repeat", "Repeat", [this](float intervalSeconds, sol::protected_function fn) -> int {
        int id = m_nextTimerId++;
        m_scheduled.push_back({id, intervalSeconds, intervalSeconds, true, false, std::move(fn)});
        return id;
    });
    Bind("time", "Cancel", "CancelTimer", [this](int id) {
        for (auto& call : m_scheduled) {
            if (call.Id == id) { call.Cancelled = true; break; }
        }
    });

    // --- Корутины: последовательности во времени как линейный код —
    // StartCoroutine(function() ... wait(1.0) ... end) вместо ручного
    // стейт-машины из Schedule-вызовов. wait() определён Lua-обвязкой ниже,
    // это просто именованная обёртка над coroutine.yield для читаемости.
    //
    // ВАЖНО: sol::coroutine должен строиться НАПРЯМУЮ из sol::function —
    // sol2 сам создаёт для неё новый Lua-поток (thread) с правильной
    // внутренней настройкой стека вызова. Если вместо этого передать сюда
    // результат ручного coroutine.create(fn), первый же resume падает с
    // "attempt to call a thread value" — sol2 ожидает управлять созданием
    // потока сам, а не оборачивать уже готовый Lua-thread.
    Bind("time", "StartCoroutine", "StartCoroutine", [this](sol::function fn) {
        // sol::coroutine, построенный НАПРЯМУЮ из функции главного Lua-состояния
        // (sol::coroutine co = fn;), не создаёт для неё отдельный Lua-поток —
        // lua_resume() в его реализации вызывается на lua_state() САМОЙ fn,
        // то есть на главном состоянии. Пока активна только ОДНА такая
        // "корутина", это незаметно работает случайно; как только их две
        // одновременно, вторая резюмится на ТОМ ЖЕ lua_State*, что и первая,
        // и по факту продолжает выполнение первой вместо своей функции.
        // Поэтому явно создаём отдельный Lua-поток (sol::thread) и переносим
        // на его стек функцию через lua_xmove перед тем, как обернуть в
        // sol::coroutine — так каждая корутина резюмится на СВОЁМ потоке.
        sol::thread runner = sol::thread::create(m_lua.lua_state());
        lua_State* runnerState = runner.state().lua_state();
        fn.push();
        lua_xmove(fn.lua_state(), runnerState, 1);
        sol::coroutine co(runnerState, -1);
        m_coroutines.push_back({std::move(co), 0.0f, std::move(runner)});
    });

    // wait(seconds) — именованная обёртка над coroutine.yield для читаемости тела
    // корутины (см. StartCoroutine выше). Регистрируется здесь же, рядом со своим
    // единственным потребителем.
    sol::protected_function_result bootstrap = m_lua.script(
        "function wait(seconds) return coroutine.yield(seconds or 0) end",
        sol::script_pass_on_error);
    if (!bootstrap.valid()) {
        sol::error err = bootstrap;
        LOG_ERROR("ScriptEngine") << "Не удалось зарегистрировать встроенную функцию wait(): " << err.what();
    }
}

void ScriptEngine::RegisterMessagingApi() {
    // --- Сообщения между скриптами: событийная модель для «компоненты общаются
    // друг с другом». Скрипт объявляет хук OnMessage(entity, name, data) (как
    // OnStart/OnUpdate); другой скрипт шлёт ему SendMessage(target, name, data)
    // (target — GameObject или его Id) или всем сразу Broadcast(name, data).
    // data — любое значение Lua (число/строка/таблица) или отсутствует (nil).
    // Так поведения связываются без глобальных переменных и жёстких ссылок. ---
    Bind("msg", "Send", "SendMessage", [this](sol::object target, const std::string& name, sol::object data) {
        int id = -1;
        if (target.is<GameObject>()) {
            GameObject o = target.as<GameObject>();
            if (o.Valid()) id = o.Id();
        } else if (target.is<int>()) {
            id = target.as<int>();
        }
        if (id >= 0) DispatchMessage(id, name, data);
    });
    Bind("msg", "Broadcast", "Broadcast", [this](const std::string& name, sol::object data) {
        DispatchMessage(-1, name, data);
    });
}

void ScriptEngine::SetLaunchArg(const std::string& key, const std::string& value) {
    if (!key.empty()) m_launchArgs[key] = value;
}

void ScriptEngine::SetLaunchArgsFromString(const std::string& args) {
    size_t i = 0;
    while (i < args.size()) {
        while (i < args.size() && std::isspace((unsigned char)args[i])) ++i;
        size_t start = i;
        while (i < args.size() && !std::isspace((unsigned char)args[i])) ++i;
        if (start == i) break;
        std::string token = args.substr(start, i - start);
        // Ведущие дефисы («--autopilot») отбрасываем: и такая запись, и голое
        // «autopilot» — привычные способы задать флаг, различать их незачем.
        size_t dash = token.find_first_not_of('-');
        if (dash == std::string::npos) continue;
        token = token.substr(dash);
        size_t eq = token.find('=');
        if (eq == std::string::npos) SetLaunchArg(token, "1");
        else SetLaunchArg(token.substr(0, eq), token.substr(eq + 1));
    }
}

void ScriptEngine::RegisterLaunchArgsApi() {
    // LaunchArg("seed") -> строка или nil; LaunchFlag("autopilot") -> bool
    // (истина для "1"/"true"/"yes"). Скрипты игры так узнают о режиме запуска,
    // не имея доступа ни к ОС, ни к командной строке.
    Bind("app", "Arg", "LaunchArg", [this](const std::string& key) -> sol::optional<std::string> {
        auto it = m_launchArgs.find(key);
        if (it == m_launchArgs.end()) return sol::nullopt;
        return it->second;
    });
    Bind("app", "Flag", "LaunchFlag", [this](const std::string& key) -> bool {
        auto it = m_launchArgs.find(key);
        if (it == m_launchArgs.end()) return false;
        const std::string& v = it->second;
        return v == "1" || v == "true" || v == "yes" || v == "on";
    });
}

