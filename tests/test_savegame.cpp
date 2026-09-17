// ===========================================================================
//  Сохранения игры — контейнер: заголовок отдельно от прогресса.
//
//  Проверяется ровно то, ради чего формат и переделан: меню читает сотню байт
//  вместо мегабайтов, файл жмётся, испорченный файл называется испорченным, а
//  предыдущее сохранение переживает запись следующего. Каждый из этих случаев
//  на глаз выглядит одинаково с исправным — заметить разницу можно только
//  числами.
// ===========================================================================
#include "TestFramework.h"

#include "sage/core/SaveGame.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Свой каталог сохранений на каждую проверку: тест не должен трогать реальный
// прогресс того, кто его запускает, и не должен видеть остатки соседнего теста.
std::string Sandbox(const char* name) {
    const std::string dir = (fs::temp_directory_path() / name).string();
    fs::remove_all(dir);
#ifdef _WIN32
    _putenv_s("APPDATA", dir.c_str());
#else
    setenv("XDG_DATA_HOME", dir.c_str(), 1);
#endif
    sage::save::SetGameName("TestGame");
    return dir;
}

// Прогресс настоящего размера: тысячи коротких чисел и повторяющихся ключей —
// ровно то, из чего состоит сохранение игры и что deflate жмёт в разы.
std::string BigPayload(int items) {
    std::string j = "{\"inventory\":[";
    for (int i = 0; i < items; ++i) {
        if (i) j += ',';
        j += "{\"id\":" + std::to_string(i) + ",\"count\":7,\"quality\":3,\"slot\":\"bag\"}";
    }
    j += "]}";
    return j;
}

size_t FileSize(const std::string& slot) {
    std::error_code ec;
    const fs::path p = fs::path(sage::save::Directory()) / (slot + ".sagesave");
    return (size_t)fs::file_size(p, ec);
}

} // namespace

TEST(SaveGame_compresses_a_real_sized_progress) {
    const std::string sandbox = Sandbox("sage_save_pack");
    const std::string payload = BigPayload(400);

    CHECK_TRUE(sage::save::Write("packed", payload, sage::save::WriteOptions{}));
    const size_t packed = FileSize("packed");

    sage::save::WriteOptions plain;
    plain.Compress = false;
    CHECK_TRUE(sage::save::Write("plain", payload, plain));
    const size_t raw = FileSize("plain");

    // Жмётся — и заметно: у прогресса из повторяющихся ключей запас велик.
    CHECK_TRUE(packed > 0 && raw > 0);
    CHECK_TRUE(packed * 2 < raw);

    // И читается обратно ТЕМ ЖЕ, чем был записан: сжатие не имеет права менять
    // содержимое, а проверить это можно только сравнением.
    std::string back;
    CHECK_TRUE(sage::save::Read("packed", back, nullptr));
    CHECK_TRUE(back.find("\"inventory\"") != std::string::npos);
    std::string backPlain;
    CHECK_TRUE(sage::save::Read("plain", backPlain, nullptr));
    CHECK_TRUE(back == backPlain);

    fs::remove_all(sandbox);
}

TEST(SaveGame_menu_reads_the_header_without_the_progress) {
    // Смысл всего контейнера: строчка в меню не должна стоить разбора
    // мегабайтов. Проверяем не время (оно на разных машинах разное), а факт —
    // заголовок читается и без прогресса, и до него.
    const std::string sandbox = Sandbox("sage_save_header");

    sage::save::WriteOptions o;
    o.Version = 4;
    o.MetaJson = R"({"chapter":"Пещера","playtime":7200})";
    CHECK_TRUE(sage::save::Write("main", BigPayload(300), o));

    sage::save::SlotInfo info;
    CHECK_TRUE(sage::save::ReadInfo("main", info));
    CHECK_EQ(info.Version, 4);
    CHECK_TRUE(info.SavedAtUnix > 0);
    CHECK_TRUE(info.Compressed);
    CHECK_FALSE(info.Broken);
    // Метка дошла целиком — её и показывают в карточке слота.
    CHECK_TRUE(info.Meta.find("Пещера") != std::string::npos);
    CHECK_TRUE(info.Meta.find("7200") != std::string::npos);

    // Заголовок меньше файла: значит он и правда отдельная часть, а не результат
    // разбора всего.
    CHECK_TRUE(info.Meta.size() < FileSize("main"));

    const std::vector<sage::save::SlotInfo> slots = sage::save::Slots();
    CHECK_EQ((int)slots.size(), 1);
    if (!slots.empty()) {
        CHECK_EQ(slots[0].Version, 4);
        CHECK_TRUE(slots[0].Meta.find("Пещера") != std::string::npos);
        CHECK_TRUE(slots[0].Bytes > 0);
    }
    fs::remove_all(sandbox);
}

TEST(SaveGame_keeps_the_previous_save_as_a_backup) {
    // Атомарная замена спасает от падения ПОСРЕДИ записи, но от самой записи не
    // спасает: игра, сохранившаяся в состояние, из которого не выбраться,
    // уносила с собой единственную рабочую копию.
    const std::string sandbox = Sandbox("sage_save_backup");

    CHECK_TRUE(sage::save::Write("main", R"({"day":1})", 1));
    CHECK_FALSE(sage::save::HasBackup("main"));   // первой записи откатывать не на что

    CHECK_TRUE(sage::save::Write("main", R"({"day":2})", 1));
    CHECK_TRUE(sage::save::HasBackup("main"));

    std::string payload;
    CHECK_TRUE(sage::save::Read("main", payload, nullptr));
    CHECK_TRUE(payload.find("2") != std::string::npos);

    CHECK_TRUE(sage::save::RestoreBackup("main"));
    CHECK_TRUE(sage::save::Read("main", payload, nullptr));
    CHECK_TRUE(payload.find("1") != std::string::npos);
    // Откат КОПИЕЙ, а не переименованием: он не должен быть односторонним.
    CHECK_TRUE(sage::save::HasBackup("main"));

    fs::remove_all(sandbox);
}

TEST(SaveGame_backup_can_be_refused) {
    const std::string sandbox = Sandbox("sage_save_nobackup");
    sage::save::WriteOptions o;
    o.KeepBackup = false;
    CHECK_TRUE(sage::save::Write("main", R"({"day":1})", o));
    CHECK_TRUE(sage::save::Write("main", R"({"day":2})", o));
    CHECK_FALSE(sage::save::HasBackup("main"));
    CHECK_FALSE(sage::save::RestoreBackup("main"));
    fs::remove_all(sandbox);
}

TEST(SaveGame_delete_takes_the_backup_with_it) {
    // Оставшийся .bak воскрес бы при следующем откате и выглядел бы как
    // «удаление не сработало».
    const std::string sandbox = Sandbox("sage_save_del");
    CHECK_TRUE(sage::save::Write("main", R"({"a":1})", 1));
    CHECK_TRUE(sage::save::Write("main", R"({"a":2})", 1));
    CHECK_TRUE(sage::save::HasBackup("main"));
    CHECK_TRUE(sage::save::Delete("main"));
    CHECK_FALSE(sage::save::Exists("main"));
    CHECK_FALSE(sage::save::HasBackup("main"));
    fs::remove_all(sandbox);
}

TEST(SaveGame_calls_a_damaged_file_damaged) {
    // Испорченный файл честнее назвать испорченным, чем отдать игре прогресс с
    // половиной значений: во втором случае игрок теряет партию молча.
    const std::string sandbox = Sandbox("sage_save_broken");
    CHECK_TRUE(sage::save::Write("main", BigPayload(50), 1));

    const fs::path file = fs::path(sage::save::Directory()) / "main.sagesave";
    const size_t size = (size_t)fs::file_size(file);
    {
        // Портим байт В ТЕЛЕ, а не в заголовке: заголовок остаётся читаемым, и
        // без контрольной суммы порча прошла бы незамеченной.
        std::fstream f(file, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp((std::streamoff)(size - 10));
        const char junk = 0x7F;
        f.write(&junk, 1);
    }

    std::string payload;
    CHECK_FALSE(sage::save::Read("main", payload, nullptr));
    // А в меню слот всё равно виден: игрок должен знать, что файл есть, и уметь
    // его удалить или откатить.
    sage::save::SlotInfo info;
    CHECK_TRUE(sage::save::ReadInfo("main", info));
    CHECK_EQ((int)sage::save::Slots().size(), 1);

    fs::remove_all(sandbox);
}

TEST(SaveGame_refuses_to_write_broken_json) {
    // Кривой JSON, записанный в файл, обнаружится при загрузке — то есть у
    // игрока и уже как «сохранение повреждено».
    const std::string sandbox = Sandbox("sage_save_badjson");
    CHECK_FALSE(sage::save::Write("main", "{это не json", 1));
    CHECK_FALSE(sage::save::Exists("main"));
    fs::remove_all(sandbox);
}

TEST(SaveGame_still_reads_the_old_plain_json_format) {
    // У игроков уже есть такие файлы, и первое же обновление игры не имеет
    // права их потерять.
    const std::string sandbox = Sandbox("sage_save_legacy");
    const fs::path dir = sage::save::Directory();
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "old.sagesave", std::ios::binary);
        f << R"({"sage_save_version":2,"savedAt":1700000000,"data":{"day":9,"hp":55}})";
    }

    std::string payload;
    int version = 0;
    CHECK_TRUE(sage::save::Read("old", payload, &version));
    CHECK_EQ(version, 2);
    CHECK_TRUE(payload.find("\"day\"") != std::string::npos);

    sage::save::SlotInfo info;
    CHECK_TRUE(sage::save::ReadInfo("old", info));
    CHECK_EQ(info.Version, 2);
    CHECK_EQ(info.SavedAtUnix, 1700000000LL);
    CHECK_FALSE(info.Compressed);

    // И перезаписывается уже НОВЫМ форматом — без просьбы мигрировать вручную.
    CHECK_TRUE(sage::save::Write("old", R"({"day":10})", 3));
    CHECK_TRUE(sage::save::ReadInfo("old", info));
    CHECK_EQ(info.Version, 3);

    fs::remove_all(sandbox);
}

TEST(SaveGame_a_slot_without_meta_reports_an_empty_object) {
    // Не пустую строку: игра читает метку как объект, и «нет метки» должно
    // выглядеть как объект без полей, а не как повод разбирать пустоту.
    const std::string sandbox = Sandbox("sage_save_nometa");
    CHECK_TRUE(sage::save::Write("main", R"({"a":1})", 1));
    sage::save::SlotInfo info;
    CHECK_TRUE(sage::save::ReadInfo("main", info));
    CHECK_TRUE(info.Meta == "{}");
    fs::remove_all(sandbox);
}

TEST(SaveGame_a_broken_meta_does_not_lose_the_progress) {
    // Метка — украшение меню, и ронять из-за неё сохранение нельзя: прогресс
    // важнее подписи к нему.
    const std::string sandbox = Sandbox("sage_save_badmeta");
    sage::save::WriteOptions o;
    o.MetaJson = "{сломано";
    CHECK_TRUE(sage::save::Write("main", R"({"day":4})", o));
    std::string payload;
    CHECK_TRUE(sage::save::Read("main", payload, nullptr));
    CHECK_TRUE(payload.find("4") != std::string::npos);
    fs::remove_all(sandbox);
}
