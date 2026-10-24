// ЖУРНАЛ КОНСОЛИ (editor/src/ConsoleLog.h): счётчики уровней, повторы,
// поиск, вытеснение старого.
#include "TestFramework.h"

#include <string>

#include "../editor/src/ConsoleLog.h"

TEST(ConsoleLog_clear_resets_level_counts) {
    // Было: после «Очистить» кнопка ошибок продолжала показывать ошибки,
    // которых в журнале больше нет.
    ConsoleLog log;
    log.Add(LogLevel::Error, "Shader", "не собрался", "10:00:00");
    log.Add(LogLevel::Warn, "Assets", "нет текстуры", "10:00:01");
    CHECK_EQ(log.Count(LogLevel::Error), 1);
    CHECK_EQ(log.Count(LogLevel::Warn), 1);
    log.Clear();
    CHECK_EQ(log.Count(LogLevel::Error), 0);
    CHECK_EQ(log.Count(LogLevel::Warn), 0);
    CHECK_TRUE(log.Entries().empty());
}

TEST(ConsoleLog_repeats_fold_into_one_line_but_count_every_time) {
    ConsoleLog log;
    for (int i = 0; i < 40; ++i) log.Add(LogLevel::Warn, "Render", "одно и то же", "10:00:00");
    log.Add(LogLevel::Warn, "Render", "другое", "10:00:02");
    CHECK_EQ(log.Entries().size(), (size_t)2);
    CHECK_EQ(log.Entries()[0].Repeats, 40);
    CHECK_EQ(log.Count(LogLevel::Warn), 41);
}

TEST(ConsoleLog_search_ignores_case_and_looks_at_the_source) {
    ConsoleLog log;
    log.Add(LogLevel::Info, "Shader", "собран", "10:00:00");
    log.Add(LogLevel::Info, "Audio", "Звук загружен", "10:00:00");
    ConsoleLog::Filter f;
    f.Text = "shader";
    CHECK_TRUE(log.Passes(log.Entries()[0], f));
    CHECK_FALSE(log.Passes(log.Entries()[1], f));
}

TEST(ConsoleLog_debug_is_hidden_until_asked) {
    ConsoleLog log;
    log.Add(LogLevel::Debug, "Net", "пакет", "10:00:00");
    ConsoleLog::Filter f;
    CHECK_FALSE(log.Passes(log.Entries()[0], f));
    f.Debug = true;
    CHECK_TRUE(log.Passes(log.Entries()[0], f));
}

TEST(ConsoleLog_old_lines_leave_and_take_their_counts) {
    ConsoleLog log;
    for (size_t i = 0; i < ConsoleLog::kLimit + 10; ++i)
        log.Add(LogLevel::Error, "X", "ошибка " + std::to_string(i), "10:00:00");
    CHECK_TRUE(log.Entries().size() <= ConsoleLog::kLimit);
    CHECK_EQ(log.Count(LogLevel::Error), (int)log.Entries().size());
}

TEST(ConsoleLog_copied_text_has_time_source_and_repeats) {
    ConsoleLog log;
    log.Add(LogLevel::Warn, "Assets", "нет файла", "10:00:00");
    log.Add(LogLevel::Warn, "Assets", "нет файла", "10:00:05");
    CHECK_EQ(log.Text(ConsoleLog::Filter{}), std::string("10:00:05 [Assets] нет файла (x2)\n"));
}
