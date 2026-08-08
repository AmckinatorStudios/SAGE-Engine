// Пакет игры (.sagepak) и виртуальная файловая система поверх него.
//
// ЗАЧЕМ ТЕСТЫ ИМЕННО ЗДЕСЬ. Формат файла ломается молча: смещение, посчитанное
// на единицу не так, даёт не отказ, а ПОХОЖИЕ НА ПРАВДУ данные — сцену, в
// которой половина объектов уехала, или скрипт, обрывающийся на середине.
// Поймать это глазами нельзя, а у игрока оно выглядит как «игра сломалась».
#include "TestFramework.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "sage/assets/Pack.h"

namespace fs = std::filesystem;
using sage::assets::PackReader;
using sage::assets::PackWriter;

namespace {

std::vector<uint8_t> Bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::string Text(const std::vector<uint8_t>& b) {
    return std::string((const char*)b.data(), b.size());
}

fs::path TempDir(const char* name) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / name;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

} // namespace

TEST(Pack_roundtrip_keeps_every_byte) {
    const fs::path file = TempDir("sage_pack_rt") / "game.sagepak";

    // Три намеренно разных случая: хорошо сжимаемый текст, пустой файл и
    // случайные байты (которые deflate только раздует).
    const std::string big(4096, 'A');
    std::vector<uint8_t> noise(1024);
    for (size_t i = 0; i < noise.size(); ++i) noise[i] = (uint8_t)((i * 37 + 11) & 0xFF);

    PackWriter writer;
    writer.Add("scenes/main.sage", Bytes(big));
    writer.Add("assets/scripts/empty.lua", {});
    writer.Add("assets/noise.bin", noise);
    CHECK_EQ((int)writer.Count(), 3);
    CHECK_TRUE(writer.Save(file));

    PackReader reader;
    CHECK_TRUE(reader.Open(file));
    CHECK_EQ((int)reader.Count(), 3);

    std::vector<uint8_t> out;
    CHECK_TRUE(reader.Read("scenes/main.sage", out));
    CHECK_TRUE(Text(out) == big);

    // Пустой файл — тоже файл. Потерять его при упаковке значило бы, что
    // ссылка на него в сцене вдруг стала битой.
    CHECK_TRUE(reader.Read("assets/scripts/empty.lua", out));
    CHECK_TRUE(out.empty());

    CHECK_TRUE(reader.Read("assets/noise.bin", out));
    CHECK_TRUE(out == noise);

    CHECK_FALSE(reader.Read("нет/такого", out));
    CHECK_FALSE(reader.Contains("нет/такого"));
    CHECK_TRUE(reader.Contains("scenes/main.sage"));

    std::error_code ec;
    fs::remove_all(file.parent_path(), ec);
}

TEST(Pack_compresses_text_but_not_incompressible_data) {
    const fs::path dir = TempDir("sage_pack_size");
    const std::string big(64 * 1024, 'B');

    PackWriter writer;
    writer.Add("text.sage", Bytes(big));
    CHECK_TRUE(writer.Save(dir / "text.sagepak"));

    std::error_code ec;
    const uintmax_t packed = fs::file_size(dir / "text.sagepak", ec);
    // Повторяющийся текст обязан ужаться в разы. Если пакет размером с
    // исходник — сжатие не сработало, и главный смысл упаковки потерян.
    CHECK_TRUE(packed < big.size() / 4);

    // А несжимаемое не должно РАСТИ: deflate на случайных байтах даёт больше
    // исходника, и записывать его «сжатым» значило бы платить временем
    // распаковки за увеличенный размер.
    std::vector<uint8_t> noise(64 * 1024);
    for (size_t i = 0; i < noise.size(); ++i) noise[i] = (uint8_t)((i * 2654435761u) >> 13);
    PackWriter w2;
    w2.Add("noise.bin", noise);
    CHECK_TRUE(w2.Save(dir / "noise.sagepak"));
    const uintmax_t packedNoise = fs::file_size(dir / "noise.sagepak", ec);
    CHECK_TRUE(packedNoise < noise.size() + 1024);   // заголовок и оглавление, не больше

    fs::remove_all(dir, ec);
}

TEST(Pack_add_directory_skips_what_the_game_does_not_need) {
    const fs::path dir = TempDir("sage_pack_dir");
    fs::create_directories(dir / "project" / "scenes");
    fs::create_directories(dir / "project" / "assets");
    { std::ofstream f(dir / "project" / "scenes" / "main.sage"); f << "{}"; }
    { std::ofstream f(dir / "project" / "scenes" / "main.sage.meta"); f << "guid"; }
    { std::ofstream f(dir / "project" / "assets" / "hero.png"); f << "png"; }

    PackWriter writer;
    const size_t added = writer.AddDirectory(dir / "project", {".meta"});
    CHECK_EQ((int)added, 2);   // .meta не попал

    CHECK_TRUE(writer.Save(dir / "game.sagepak"));
    PackReader reader;
    CHECK_TRUE(reader.Open(dir / "game.sagepak"));
    CHECK_TRUE(reader.Contains("scenes/main.sage"));
    CHECK_TRUE(reader.Contains("assets/hero.png"));
    CHECK_FALSE(reader.Contains("scenes/main.sage.meta"));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(Pack_rejects_foreign_and_future_files) {
    const fs::path dir = TempDir("sage_pack_bad");

    // Чужой файл: не пакет вовсе.
    { std::ofstream f(dir / "not.sagepak", std::ios::binary); f << "это просто текст, не пакет"; }
    PackReader reader;
    CHECK_FALSE(reader.Open(dir / "not.sagepak"));
    CHECK_FALSE(reader.IsOpen());

    // Пакет будущей версии. Прочитать его по сегодняшним правилам — значит
    // получить мусор, который выглядит как данные, поэтому отказ обязателен.
    PackWriter writer;
    writer.Add("a.txt", Bytes("данные"));
    CHECK_TRUE(writer.Save(dir / "future.sagepak"));
    {
        std::fstream f(dir / "future.sagepak", std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(4);
        const unsigned char version99[4] = {99, 0, 0, 0};
        f.write((const char*)version99, 4);
    }
    PackReader future;
    CHECK_FALSE(future.Open(dir / "future.sagepak"));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Виртуальная файловая система: пакет, если он есть, иначе диск.
//
// ГЛАВНОЕ ЗДЕСЬ — ПРИОРИТЕТ. Рядом с собранной игрой могут остаться файлы от
// прошлой сборки, и прочитать их вместо упакованных значило бы запустить
// вчерашнюю логику с сегодняшними данными. Такое ищут очень долго.
TEST(Pack_vfs_prefers_the_package_over_stale_files_on_disk) {
    namespace vfs = sage::assets::vfs;
    const fs::path dir = TempDir("sage_pack_vfs");
    const fs::path saved = fs::current_path();
    fs::current_path(dir);

    // На диске — «вчерашняя» версия файла.
    fs::create_directories("scenes");
    { std::ofstream f("scenes/main.sage"); f << "СТАРОЕ"; }

    PackWriter writer;
    writer.Add("scenes/main.sage", Bytes("НОВОЕ"));
    writer.Add("assets/only-in-pack.lua", Bytes("только в пакете"));
    CHECK_TRUE(writer.Save("game.sagepak"));

    // Без пакета читается диск — так работает редактор, и правка .lua обязана
    // подхватываться сразу, без пересборки.
    std::string text;
    CHECK_FALSE(vfs::Mounted());
    CHECK_TRUE(vfs::ReadText("scenes/main.sage", text));
    CHECK_TRUE(text == "СТАРОЕ");
    CHECK_FALSE(vfs::Exists("assets/only-in-pack.lua"));

    CHECK_TRUE(vfs::Mount("game.sagepak"));
    CHECK_TRUE(vfs::Mounted());
    CHECK_TRUE(vfs::ReadText("scenes/main.sage", text));
    CHECK_TRUE(text == "НОВОЕ");                       // пакет победил диск
    CHECK_TRUE(vfs::Exists("assets/only-in-pack.lua"));
    CHECK_TRUE(vfs::ReadText("assets/only-in-pack.lua", text));
    CHECK_TRUE(text == "только в пакете");

    // Файла нет нигде — честный отказ, а не пустая строка: «прочитали пусто» и
    // «нет файла» это разные события, и путать их нельзя.
    CHECK_FALSE(vfs::ReadText("нет.sage", text));

    // Снятие пакета возвращает диск.
    vfs::Unmount();
    CHECK_FALSE(vfs::Mounted());
    CHECK_TRUE(vfs::ReadText("scenes/main.sage", text));
    CHECK_TRUE(text == "СТАРОЕ");

    std::error_code ec;
    fs::current_path(saved, ec);
    fs::remove_all(dir, ec);
}
