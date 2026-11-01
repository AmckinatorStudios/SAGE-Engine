#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// КАТАЛОГ API СКРИПТОВ — одно описание каждой функции движка для всех языков
// и всех инструментов.
//
// ЧТО БЫЛО. Функция регистрировалась в Lua (Bind -> sol2), а её описание для
// проверки типов, подсказок редактора и снимка совместимости собиралось
// ОТДЕЛЬНО — регулярными выражениями по тексту C++ (scripts/gen_script_api.py).
// Типы, которые регулярка не видела, становились «nil»: у половины функций
// (Clamp, Cross, HasSave…) проверщик считал, что они ничего не возвращают.
// Функции модулей игр и плагинов (RegisterScriptModule) не попадали ни в
// подсказки, ни в проверку вовсе — их видел только Lua.
//
// ЧТО ТЕПЕРЬ. Регистрация сама кладёт сюда запись: имя, модуль, псевдонимы,
// типы аргументов и возврата, ВЫВЕДЕННЫЕ КОМПИЛЯТОРОМ из сигнатуры C++
// (sage/scripting/ScriptApi.h), и признак «доступна любому языку». Каталог
// читают все, кому нужно знать API: проверщик SAGE Lua, подсказки редактора,
// внешние языки, инструмент sage_api. Описывать функцию второй раз не нужно
// нигде.
//
// ПОЧЕМУ СЛОЙ lang. Проверщик типов (sage/lang) живёт ниже слоя скриптов и
// регистрации не видит; каталог — общее место встречи: слой скриптов пишет,
// слой языка читает. Здесь только описания: вызов функции принадлежит хозяину
// состояния скриптов (ApiBindings в sage/scripting/ScriptApi.h) — у него те же
// сроки жизни, что у захваченных функцией объектов.
// ---------------------------------------------------------------------------
namespace sage::lang {

// Тип в каталоге — имя, общее для всех языков и инструментов:
//   nil, boolean, integer, number, string, Vec2, Vec3, Vec4, Quat, Entity,
//   Asset, table, function, any — и имена usertype'ов движка (Material,
//   Camera…), если функция их принимает или отдаёт.
struct ApiParam {
    std::string Name;      // имя аргумента (может быть пустым — тогда argN)
    std::string Type;
    bool Optional = false;
};

struct ApiEntry {
    std::string Module;                 // "noise"; пусто — верхний уровень
    std::string Name;                   // "Perlin"
    // Полное имя: "sage.noise.Perlin" (или просто Name без модуля).
    std::string Qualified;
    // Другие имена, под которыми скрипт видит ТУ ЖЕ функцию: короткое имя
    // модуля ("noise.Perlin"), прежнее глобальное ("Perlin").
    std::vector<std::string> Aliases;
    std::vector<ApiParam> Params;
    bool Variadic = false;              // после Params — сколько угодно
    bool Unknown = false;               // сигнатуру вывести нельзя (шаблонная лямбда)
    std::vector<std::string> Returns;   // пусто — ничего; несколько — кортеж
    std::string Doc;
    std::string Origin = "engine";      // "engine", "module:<имя>", "plugin:<имя>"
    // Есть ли универсальный вызов (значения vars::Value): функцию может звать
    // ЛЮБОЙ язык, а не только тот, где она привязана напрямую.
    bool Portable = false;

    // Подпись для людей: "x: number, y?: number -> number".
    std::string Signature() const;
};

class ApiRegistry {
public:
    static ApiRegistry& Instance();

    // Добавить запись. Повтор той же функции с той же сигнатурой — не ошибка
    // (каждое состояние скриптов регистрирует API заново). Конфликт — другое:
    // имя или псевдоним уже занят функцией с другой сигнатурой либо другим
    // полным именем. Он записывается (Conflicts) и возвращается строкой; пусто —
    // всё в порядке. Первая запись остаётся в силе.
    std::string Add(ApiEntry entry);

    // По полному имени или псевдониму. Копия — каталог могут дописывать
    // из другого потока.
    bool Find(const std::string& name, ApiEntry& out) const;
    bool Has(const std::string& name) const;
    std::vector<ApiEntry> All() const;          // по полному имени
    std::vector<std::string> Modules() const;   // имена модулей

    // Конфликты имён, найденные при регистрации (по строке на каждый).
    std::vector<std::string> Conflicts() const;

    // Растёт с каждой новой записью: кто кэширует выборку, видит, что пора
    // перечитать (подсказки редактора, проверщик).
    uint64_t Version() const;

    // Снять записи источника (выгрузка плагина, снятие модуля).
    int RemoveOrigin(const std::string& origin);

    // Каталог целиком — JSON для внешних инструментов и языков:
    // {"functions":[{"name","module","aliases","params":[{name,type,optional}],
    //   "variadic","returns","doc","origin","portable"}]}.
    std::string ToJson() const;

    // Подсказки LuaLS/EmmyLua (---@meta): объявления модулей и функций под
    // всеми именами. Для редакторов кода — с функциями игр и плагинов.
    std::string ToLuaHints() const;

    // Только для тестов: начать с чистого листа.
    void ClearForTests();

private:
    mutable std::mutex m_mutex;
    std::vector<ApiEntry> m_entries;
    std::vector<std::string> m_conflicts;
    uint64_t m_version = 0;
    const ApiEntry* FindLocked(const std::string& name) const;
};

} // namespace sage::lang
