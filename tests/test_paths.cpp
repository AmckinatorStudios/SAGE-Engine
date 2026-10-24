// ============================================================================
//  Кодировка путей: окружение и байты, которые не UTF-8.
//
//  ЧТО ЗДЕСЬ ПРОВЕРЯЕТСЯ И ПОЧЕМУ. Редактор не запускался на русской Windows.
//  Не «работал странно» — не запускался вовсе, у всех, у кого имя пользователя
//  написано кириллицей. В логе оставались ровно две строки:
//
//      [INFO ] [Editor] SAGE Editor запускается...
//      [ERROR] [Engine] ФАТАЛЬНАЯ ОШИБКА ПРИ ЗАПУСКЕ: filesystem error:
//                       Cannot convert character sequence: Illegal byte sequence
//
//  Виновата одна строка — чтение APPDATA через std::getenv. Узкое окружение
//  Windows отдаёт «C:\Users\Владимир\AppData\Roaming» байтами CP1251, а
//  std::filesystem::path из узкой строки ждёт UTF-8 и на чужих байтах БРОСАЕТ.
//
//  Отсюда два требования, которые тесты и закрепляют:
//    1) значение окружения превращается в путь без потерь и без исключений;
//    2) НИКАКИЕ байты, поданные в PathFromUtf8, не приводят к исключению —
//       испорченное имя это ненайденный файл, а не смерть программы.
//
//  На Linux первое выполняется само собой (путь там — просто байты), но тесты
//  всё равно нужны: тот же код собирается для Windows, и сломать его правкой,
//  проверенной только на Linux, — ровно то, как эта ошибка и появилась.
// ============================================================================
#include "TestFramework.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "sage/core/Paths.h"

namespace fs = std::filesystem;

// Кириллица в значении переменной окружения доезжает до пути целиком.
TEST(Paths_env_var_with_cyrillic_becomes_a_usable_path) {
    const std::string value =
#ifdef _WIN32
        "C:\\Users\\Владимир\\Documents\\SAGE Projects";
#else
        "/home/владимир/Документы/SAGE Projects";
#endif
#ifdef _WIN32
    _putenv_s("SAGE_TEST_UNICODE_DIR", value.c_str());
#else
    setenv("SAGE_TEST_UNICODE_DIR", value.c_str(), 1);
#endif

    const fs::path p = sage::EnvPath("SAGE_TEST_UNICODE_DIR");
    CHECK_TRUE(!p.empty());
    // Обратно — той же строкой: перекодировка не имеет права терять символы.
    CHECK_EQ(sage::PathToUtf8(p), value);
    // Имя последней части читается как имя, а не как набор вопросительных знаков.
    CHECK_EQ(sage::PathToUtf8(p.filename()), std::string("SAGE Projects"));
}

// Незаданной переменной соответствует пустой путь, а не выдуманный.
TEST(Paths_env_var_that_is_not_set_gives_an_empty_path) {
    CHECK_TRUE(sage::EnvPath("SAGE_TEST_DEFINITELY_NOT_SET_42").empty());
    CHECK_TRUE(sage::EnvString("SAGE_TEST_DEFINITELY_NOT_SET_42").empty());
    CHECK_TRUE(sage::EnvPath(nullptr).empty());
}

// Пустое значение — это тоже «не задано»: путь "" открыть нельзя, и подставлять
// его как базу для настроек значило бы писать их в корень файловой системы.
TEST(Paths_empty_env_var_gives_an_empty_path) {
#ifdef _WIN32
    _putenv_s("SAGE_TEST_EMPTY_DIR", "");
#else
    setenv("SAGE_TEST_EMPTY_DIR", "", 1);
#endif
    CHECK_TRUE(sage::EnvPath("SAGE_TEST_EMPTY_DIR").empty());
}

// ГЛАВНОЕ. Байты, которые не являются корректным UTF-8, не должны бросать.
// Именно исключение здесь и убивало редактор ещё до создания окна.
TEST(Paths_broken_bytes_do_not_throw) {
    // «Владимир» в CP1251 — ровно то, что приходило из getenv на русской
    // Windows. Как UTF-8 эта последовательность некорректна.
    const std::string cp1251 = "\xC2\xEB\xE0\xE4\xE8\xEC\xE8\xF0";
    bool threw = false;
    fs::path p;
    try {
        p = sage::PathFromUtf8(cp1251);
    } catch (...) {
        threw = true;
    }
    CHECK_FALSE(threw);

    // И совсем произвольный мусор — тоже не повод падать.
    threw = false;
    try {
        (void)sage::PathFromUtf8(std::string("\xFF\xFE\x80\x80/scene.sage"));
    } catch (...) {
        threw = true;
    }
    CHECK_FALSE(threw);

    // Пустая строка — пустой путь, без исключений и без "." из ниоткуда.
    CHECK_TRUE(sage::PathFromUtf8("").empty());
}

// Обычный путь в UTF-8 проходит туда и обратно без изменений.
TEST(Paths_utf8_round_trip_keeps_the_path_intact) {
    const std::string utf8 =
#ifdef _WIN32
        "D:\\Игры\\Моя игра\\scenes\\главная.sage";
#else
        "/tmp/Игры/Моя игра/scenes/главная.sage";
#endif
    const fs::path p = sage::PathFromUtf8(utf8);
    CHECK_EQ(sage::PathToUtf8(p), utf8);
    CHECK_EQ(sage::PathToUtf8(p.filename()), std::string("главная.sage"));
    CHECK_EQ(sage::PathToUtf8(p.extension()), std::string(".sage"));
}

// SystemToUtf8 не портит то, что и так UTF-8 (на Linux это тождество, на
// Windows с манифестом UTF-8 — тоже), и не падает на пустоте.
TEST(Paths_system_to_utf8_is_safe_on_empty_and_ascii) {
    CHECK_TRUE(sage::SystemToUtf8(nullptr).empty());
    CHECK_TRUE(sage::SystemToUtf8("").empty());
    CHECK_EQ(sage::SystemToUtf8("C:/Games/Demo"), std::string("C:/Games/Demo"));
}

// Домашняя папка либо пуста, либо это настоящий путь — но не исключение.
// Раньше HomeDir() на Windows строилась из getenv напрямую и падала там же,
// где и всё остальное.
TEST(Paths_home_dir_never_throws) {
    bool threw = false;
    try {
        (void)sage::HomeDir();
        (void)sage::DefaultProjectsDir();
        (void)sage::UserFolders();
    } catch (...) {
        threw = true;
    }
    CHECK_FALSE(threw);
}

// Программа, запущенная из Finder или меню рабочего стола, получает текущей
// папкой корень диска — писать лог туда нельзя. EnsureWritableWorkDir уводит
// её в папку настроек; запуск из обычной папки (терминал, IDE) не трогает.
TEST(Paths_root_working_dir_moves_to_a_writable_one) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path before = fs::current_path(ec);
    const fs::path cfg = fs::temp_directory_path() / "sage_workdir_cfg";
#if !defined(_WIN32)
    const char* oldXdg = std::getenv("XDG_CONFIG_HOME");
    const std::string keep = oldXdg ? oldXdg : "";
    setenv("XDG_CONFIG_HOME", cfg.string().c_str(), 1);
#endif
    // Обычная папка — как была.
    CHECK_TRUE(sage::EnsureWritableWorkDir("probe") == fs::current_path(ec));
    CHECK_TRUE(fs::current_path(ec) == before);
    // Корень — в папку настроек, и туда можно писать.
    fs::current_path(before.root_path(), ec);
    const fs::path moved = sage::EnsureWritableWorkDir("probe");
    CHECK_TRUE(moved != moved.root_path());
    // equivalent, а не ==: на macOS /var — ссылка на /private/var, и одна и
    // та же папка пишется двумя путями.
    CHECK_TRUE(fs::equivalent(fs::current_path(ec), moved, ec));
    { std::ofstream f("probe.log"); f << "ok"; CHECK_TRUE(f.good()); }
    fs::current_path(before, ec);
#if !defined(_WIN32)
    if (oldXdg) setenv("XDG_CONFIG_HOME", keep.c_str(), 1);
    else unsetenv("XDG_CONFIG_HOME");
#endif
    fs::remove_all(cfg, ec);
}
