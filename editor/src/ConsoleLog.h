#pragma once
#include <string>
#include <vector>

#include "sage/core/Log.h"

// ---------------------------------------------------------------------------
// ЖУРНАЛ КОНСОЛИ — то, что панель Console показывает, без ImGui.
//
// Отдельно от панели ради проверки: правила у журнала свои и легко ломаются
// незаметно. Повтор сворачивается в одну строку со счётчиком; старое
// вытесняется, чтобы лог за час работы не съел память; фильтр ищет и по тексту,
// и по источнику; и — то, что раньше было сломано, — счётчики уровней на
// кнопках считают ТО, ЧТО ЛЕЖИТ В ЖУРНАЛЕ. После «Очистить» кнопка
// «Ошибки (12)» продолжала показывать двенадцать ошибок, которых больше нет, и
// по ней нельзя было понять, появилась ли новая.
// ---------------------------------------------------------------------------
class ConsoleLog {
public:
    struct Entry {
        LogLevel Level = LogLevel::Info;
        std::string Category;
        std::string Message;
        int Repeats = 1;     // сколько раз подряд пришло одно и то же
        std::string Stamp;   // «15:42:17» — когда пришло (а не когда показано)
    };

    // Что показывать. Отладка выключена по умолчанию: полезна точечно, а
    // включённой вытесняет из окна всё остальное.
    struct Filter {
        bool Debug = false;
        bool Info = true;
        bool Warn = true;
        bool Error = true;
        std::string Text;    // подстрока в сообщении или источнике
    };

    void Add(LogLevel level, const std::string& category, const std::string& message,
             const std::string& stamp);
    void Clear();

    const std::vector<Entry>& Entries() const { return m_entries; }
    bool Passes(const Entry& e, const Filter& f) const;

    // Сколько СТРОК каждого уровня сейчас в журнале (повторы — по числу
    // приходов: «x40» — это сорок предупреждений, а не одно).
    int Count(LogLevel level) const;

    // Видимые строки текстом — для копирования в отчёт.
    std::string Text(const Filter& f) const;
    static std::string LineText(const Entry& e);

    // Предел журнала: дальше старая четверть вытесняется.
    static constexpr size_t kLimit = 4000;

private:
    static int Bucket(LogLevel level);
    std::vector<Entry> m_entries;
    int m_counts[4] = {0, 0, 0, 0};   // отладка, сообщения, предупреждения, ошибки
};
