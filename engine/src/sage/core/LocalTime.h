#pragma once
#include <ctime>

// ---------------------------------------------------------------------------
// МЕСТНОЕ ВРЕМЯ — дата и время по часовому поясу игрока.
//
// Нужно всему, что показывают человеку: «сохранено 7.11.26 в 7:53», имя снимка
// экрана, отметка в журнале игры. Скриптам библиотека os не открывается (она
// умеет и запускать программы, и удалять файлы), а дата без неё — только
// секунды с 1970 года по Гринвичу, которые игроку ничего не говорят.
// ---------------------------------------------------------------------------
namespace sage {

struct DateParts {
    int Year = 1970, Month = 1, Day = 1;      // месяц 1..12, день 1..31
    int Hour = 0, Minute = 0, Second = 0;
    int Weekday = 4;                          // 0 — воскресенье
};

// Секунды с 1970 года (UTC) -> местные дата и время.
inline DateParts LocalDate(long long unixSeconds) {
    const std::time_t t = (std::time_t)unixSeconds;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    DateParts d;
    d.Year = tm.tm_year + 1900;
    d.Month = tm.tm_mon + 1;
    d.Day = tm.tm_mday;
    d.Hour = tm.tm_hour;
    d.Minute = tm.tm_min;
    d.Second = tm.tm_sec;
    d.Weekday = tm.tm_wday;
    return d;
}

inline long long UnixNow() { return (long long)std::time(nullptr); }

} // namespace sage
