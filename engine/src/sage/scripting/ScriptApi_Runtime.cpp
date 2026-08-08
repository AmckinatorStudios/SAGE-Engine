#include "ScriptEngine.h"

#include "sage/core/Log.h"
#include "sage/core/SaveGame.h"

#include <nlohmann/json.hpp>
#include <vector>
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

namespace {

// --- sol::table <-> JSON ------------------------------------------------------
//
// Игра думает таблицами Lua, файл хранит JSON. Перевод здесь, в одном месте, и
// он намеренно ограничен: числа, строки, булевы, вложенные таблицы и массивы.
// Функции, userdata и сущности НЕ сохраняются — сохранить указатель на объект
// прошлого запуска нельзя в принципе, а сделать вид, что можно, значит вернуть
// игроку мусор вместо прогресса.
nlohmann::json TableToJson(const sol::table& table, int depth,
                           std::vector<const void*>& openTables) {
    // ЦИКЛЫ ловим по факту, а не глубиной рекурсии.
    //
    // Одной только глубины мало, и это выяснилось падением: у t.self = t каждый
    // уровень держит ОТКРЫТЫЙ обход таблицы (lua_next требует ключ на стеке всё
    // время обхода), и три десятка вложенных обходов переполняют стек Lua. Игра
    // при этом не падает сразу — она падает потом, при закрытии состояния, и
    // связать это с сохранением уже невозможно.
    //
    // Поэтому помним таблицы, обход которых сейчас открыт: повторная встреча —
    // это цикл, и в JSON он не представим ни при какой глубине.
    const void* id = table.pointer();
    for (const void* open : openTables) {
        if (open == id) return nlohmann::json::object();
    }
    if (depth > 16) return nlohmann::json::object();
    openTables.push_back(id);
    struct PopGuard {
        std::vector<const void*>& V;
        ~PopGuard() { V.pop_back(); }
    } guard{openTables};

    // Массив или словарь решается по ключам: таблица Lua — и то и другое сразу,
    // а в JSON это разные вещи. Считаем массивом только сплошную нумерацию с 1.
    bool isArray = table.size() > 0;
    if (isArray) {
        size_t counted = 0;
        for (const auto& kv : table) {
            if (kv.first.get_type() != sol::type::number) { isArray = false; break; }
            ++counted;
        }
        if (counted != table.size()) isArray = false;
    }

    nlohmann::json out = isArray ? nlohmann::json::array() : nlohmann::json::object();
    auto convert = [&](const sol::object& v) -> nlohmann::json {
        switch (v.get_type()) {
            case sol::type::number: {
                const double d = v.as<double>();
                // Целое пишем целым: иначе счётчик предметов возвращается как
                // 7.0 и «== 7» в скрипте продолжает работать, а вот вывод в
                // интерфейсе внезапно показывает 7.0.
                if (d == (double)(long long)d) return (long long)d;
                return d;
            }
            case sol::type::boolean: return v.as<bool>();
            case sol::type::string: return v.as<std::string>();
            case sol::type::table: return TableToJson(v.as<sol::table>(), depth + 1, openTables);
            default: return nullptr;
        }
    };

    for (const auto& kv : table) {
        const nlohmann::json value = convert(kv.second);
        if (value.is_null()) continue;   // несохраняемое молча пропускаем
        if (isArray) out.push_back(value);
        else if (kv.first.get_type() == sol::type::string) out[kv.first.as<std::string>()] = value;
        else if (kv.first.get_type() == sol::type::number)
            out[std::to_string(kv.first.as<long long>())] = value;
    }
    return out;
}

sol::object JsonToLua(const nlohmann::json& j, sol::state_view lua) {
    if (j.is_boolean()) return sol::make_object(lua, j.get<bool>());
    if (j.is_number_integer()) return sol::make_object(lua, j.get<long long>());
    if (j.is_number()) return sol::make_object(lua, j.get<double>());
    if (j.is_string()) return sol::make_object(lua, j.get<std::string>());
    if (j.is_array()) {
        sol::table t = lua.create_table((int)j.size(), 0);
        int i = 1;
        for (const auto& v : j) t[i++] = JsonToLua(v, lua);
        return t;
    }
    if (j.is_object()) {
        sol::table t = lua.create_table();
        for (auto it = j.begin(); it != j.end(); ++it) t[it.key()] = JsonToLua(it.value(), lua);
        return t;
    }
    return sol::nil;
}

} // namespace

// --- Сохранения игры: ПРОГРЕСС ИГРОКА ----------------------------------------
//
// Раньше сериализовалась только сцена — редакторный формат. Прогресс игрока
// сохранять было нечем, и игре оставалось либо писать файлы самой в обход
// движка, либо не сохраняться вовсе. Подробно о разнице и о том, почему
// сохранения лежат в пользовательском каталоге и пишутся через переименование,
// — в sage/core/SaveGame.h.
void ScriptEngine::RegisterSaveApi() {
    Bind("save", "Write", "SaveGame",
         [](const std::string& slot, sol::table data, sol::optional<int> version) -> bool {
             std::vector<const void*> open;
             return sage::save::Write(slot, TableToJson(data, 0, open).dump(),
                                      version.value_or(1));
         });

    // Возвращает таблицу или nil. Именно nil, а не пустая таблица: «сохранения
    // нет» и «сохранение есть, но пустое» — разные вещи, и игра обязана уметь
    // их различать, иначе новый игрок попадает в конец игры с нулями.
    Bind("save", "Read", "LoadGame",
         [this](const std::string& slot) -> sol::object {
             std::string payload;
             int version = 1;
             if (!sage::save::Read(slot, payload, &version)) return sol::nil;
             try {
                 return JsonToLua(nlohmann::json::parse(payload), m_lua);
             } catch (const std::exception& e) {
                 LOG_ERROR("Save") << "Сохранение не разобралось (" << slot << "): " << e.what();
                 return sol::nil;
             }
         });

    // Версия формата, которой записан слот, — отдельно от данных: игра решает,
    // мигрировать ли, ДО того как начнёт их читать.
    Bind("save", "Version", "SaveVersion", [](const std::string& slot) -> int {
        std::string payload;
        int version = 0;
        return sage::save::Read(slot, payload, &version) ? version : 0;
    });

    Bind("save", "Exists", "HasSave",
         [](const std::string& slot) { return sage::save::Exists(slot); });
    Bind("save", "Delete", "DeleteSave",
         [](const std::string& slot) { return sage::save::Delete(slot); });
    Bind("save", "Directory", "SaveDirectory", []() { return sage::save::Directory(); });

    // Список слотов для меню «Продолжить»: имя, время и версия — без чтения
    // самого прогресса, потому что ради строчки в меню грузить его незачем.
    Bind("save", "Slots", "SaveSlots", [this]() -> sol::table {
        sol::table out = m_lua.create_table();
        int i = 1;
        for (const sage::save::SlotInfo& s : sage::save::Slots()) {
            sol::table row = m_lua.create_table();
            row["name"] = s.Name;
            row["savedAt"] = s.SavedAtUnix;
            row["version"] = s.Version;
            row["bytes"] = (long long)s.Bytes;
            out[i++] = row;
        }
        return out;
    });
}

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


// ---------------------------------------------------------------------------
//  Ход игры: смена сцены, пауза, перезапуск, выход, масштаб времени
//
//  ЗАЧЕМ ЭТО ПОЯВИЛОСЬ. Без смены сцены игра на движке может быть длиной ровно
//  в одну сцену: меню → уровень 1 → уровень 2 → титры собрать было нельзя, и
//  обойти это из скрипта было нечем — SceneManager существовал только в C++.
//  Это был самый крупный практический пробел движка: не «картинка не та», а
//  «игру нельзя доделать до конца».
//
//  ПОЧЕМУ ВСЁ ЗДЕСЬ — ЗАПРОСЫ, А НЕ ДЕЙСТВИЯ. Скрипт зовёт эти функции изнутри
//  OnUpdate, то есть когда движок идёт по сущностям текущей сцены, а сам скрипт
//  держит на них ссылки. Сменить сцену прямо там — значит уничтожить реестр под
//  ногами у обхода. Поэтому функция лишь запоминает намерение, а выполняет его
//  хозяин кадра между кадрами (см. ScriptEngine::TakeSceneRequest).
// ---------------------------------------------------------------------------
void ScriptEngine::RegisterGameFlowApi() {
    // sage.scene.Load("level2") — по ИМЕНИ сцены, без пути и расширения.
    // Скрипт не должен знать, где лежат сцены проекта и как они называются на
    // диске: это забота хозяина кадра, и в собранной игре раскладка другая.
    Bind("scene", "Load", "LoadScene", [this](const std::string& name) {
        if (name.empty()) {
            LOG_WARN("Script") << "sage.scene.Load: пустое имя сцены — пропускаю";
            return;
        }
        m_pendingScene = name;
        m_sceneRequested = true;
    });

    // Перезапуск текущей сцены — «начать уровень заново». Отдельно от Load,
    // потому что скрипт не обязан знать имя сцены, в которой живёт.
    Bind("game", "Restart", nullptr, [this]() { m_restartRequested = true; });

    // Выход из игры. В редакторе Play-режим просто останавливается — там это
    // «выйти из игры», а не «закрыть редактор».
    Bind("game", "Quit", nullptr, [this]() { m_quitRequested = true; });

    Bind("game", "Pause", nullptr, [this](bool paused) { m_paused = paused; });
    Bind("game", "IsPaused", nullptr, [this]() { return m_paused; });

    // Масштаб времени: 0.5 — замедление, 2 — ускорение, 0 — стоп.
    //
    // Отрицательный не принимаем. Обратное время звучит заманчиво, но физика,
    // анимация и таймеры к нему не готовы: получилась бы не перемотка, а
    // разъезжающееся состояние, которое ищут неделю.
    Bind("time", "SetScale", nullptr, [this](float scale) {
        m_timeScale = scale < 0.0f ? 0.0f : scale;
    });
    Bind("time", "Scale", nullptr, [this]() { return m_timeScale; });
}
