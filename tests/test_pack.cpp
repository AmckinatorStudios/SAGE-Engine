// Пакет игры (.sagepak) и виртуальная файловая система поверх него.
//
// ЗАЧЕМ ТЕСТЫ ИМЕННО ЗДЕСЬ. Формат файла ломается молча: смещение, посчитанное
// на единицу не так, даёт не отказ, а ПОХОЖИЕ НА ПРАВДУ данные — сцену, в
// которой половина объектов уехала, или скрипт, обрывающийся на середине.
// Поймать это глазами нельзя, а у игрока оно выглядит как «игра сломалась».
#include "TestFramework.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "sage/assets/Pack.h"
#include "sage/assets/import/ObjMtl.h"
#include "sage/render/ModelLoader.h"
#include "sage/render/MeshData.h"

#include <stb_image.h>

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

// ---------------------------------------------------------------------------
// ЗАЩИТА ПАКЕТА (версия 2).
//
// Имена файлов и содержимое не должны читаться «в лоб»: пакет открывали
// блокнотом и архиватором, видели там сцены и скрипты текстом — и правили.
// ---------------------------------------------------------------------------
namespace {
std::vector<uint8_t> ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
bool Contains(const std::vector<uint8_t>& hay, const std::string& needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}
} // namespace

TEST(Pack_v2_hides_file_names_and_content) {
    const fs::path file = TempDir("sage_pack_hide") / "game.sagepak";
    PackWriter writer;
    writer.Add("scripts/boss_level_logic.lua", Bytes("local SECRET_BALANCE = 9000"));
    writer.Add("scenes/main.sage", Bytes(std::string(2000, 'x') + "PLAINTEXT_MARKER"));
    CHECK_TRUE(writer.Save(file));

    const std::vector<uint8_t> raw = ReadAll(file);
    CHECK_FALSE(Contains(raw, "boss_level_logic"));
    CHECK_FALSE(Contains(raw, "SECRET_BALANCE"));
    CHECK_FALSE(Contains(raw, "scenes/main.sage"));

    PackReader reader;
    CHECK_TRUE(reader.Open(file));
    CHECK_EQ((int)reader.Version(), 2);
    std::vector<uint8_t> out;
    CHECK_TRUE(reader.Read("scripts/boss_level_logic.lua", out));
    CHECK_TRUE(Text(out) == "local SECRET_BALANCE = 9000");
    std::error_code ec;
    fs::remove_all(file.parent_path(), ec);
}

// Изменённый байт — это ОТКАЗ читать файл, а не «чуть другая» сцена: игра,
// пошедшая по подправленным данным молча, хуже игры, которая сказала «пакет
// повреждён».
TEST(Pack_v2_refuses_tampered_data_and_index) {
    const fs::path dir = TempDir("sage_pack_tamper");
    // Несжимаемые байты: запись ляжет как есть, и смещение 56+100 точно внутри неё.
    std::vector<uint8_t> big(4096);
    uint32_t x = 2463534242u;
    for (size_t i = 0; i < big.size(); ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        big[i] = (uint8_t)x;
    }
    PackWriter writer;
    writer.Add("a.bin", big);
    CHECK_TRUE(writer.Save(dir / "game.sagepak"));

    // Данные: байт посреди первого (и единственного) файла.
    fs::copy_file(dir / "game.sagepak", dir / "data.sagepak");
    {
        std::fstream f(dir / "data.sagepak", std::ios::binary | std::ios::in | std::ios::out);
        f.seekg(56 + 100);
        char c = 0;
        f.read(&c, 1);
        c ^= 0x5A;
        f.seekp(56 + 100);
        f.write(&c, 1);
    }
    PackReader data;
    CHECK_TRUE(data.Open(dir / "data.sagepak"));
    std::vector<uint8_t> out;
    CHECK_FALSE(data.Read("a.bin", out));
    CHECK_TRUE(out.empty());

    // Оглавление: последний байт файла — его хвост.
    fs::copy_file(dir / "game.sagepak", dir / "index.sagepak");
    {
        const uintmax_t size = fs::file_size(dir / "index.sagepak");
        std::fstream f(dir / "index.sagepak", std::ios::binary | std::ios::in | std::ios::out);
        f.seekg((std::streamoff)size - 1);
        char c = 0;
        f.read(&c, 1);
        c ^= 0x01;
        f.seekp((std::streamoff)size - 1);
        f.write(&c, 1);
    }
    PackReader index;
    CHECK_FALSE(index.Open(dir / "index.sagepak"));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Пакеты ВЕРСИИ 1 (без шифрования) обязаны читаться: так собраны шаблоны
// проектов и игры, выпущенные до защиты.
TEST(Pack_reads_version1_packages) {
    const fs::path dir = TempDir("sage_pack_v1");
    const std::string body = "{\"v1\":true}";
    std::vector<uint8_t> file;
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) file.push_back((uint8_t)(v >> (i * 8))); };
    auto u64 = [&](uint64_t v) { for (int i = 0; i < 8; ++i) file.push_back((uint8_t)(v >> (i * 8))); };
    u32(0x4B415053);   // 'SPAK'
    u32(1);
    u32(1);
    u32(0);
    const uint64_t indexOffset = 24 + body.size();
    u64(indexOffset);
    file.insert(file.end(), body.begin(), body.end());
    const std::string name = "scenes/old.sage";
    u32((uint32_t)name.size());
    file.insert(file.end(), name.begin(), name.end());
    u64(24);
    u64(body.size());
    u64(body.size());
    u32(0);
    { std::ofstream f(dir / "old.sagepak", std::ios::binary); f.write((const char*)file.data(), (std::streamsize)file.size()); }

    PackReader reader;
    CHECK_TRUE(reader.Open(dir / "old.sagepak"));
    CHECK_EQ((int)reader.Version(), 1);
    std::vector<uint8_t> out;
    CHECK_TRUE(reader.Read("scenes/old.sage", out));
    CHECK_TRUE(Text(out) == body);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Ход записи доходит до конца, а отмена не оставляет полупакета: половина
// пакета хуже никакого — игра открыла бы её и развалилась на первом файле.
TEST(Pack_save_reports_progress_and_cancel_removes_the_file) {
    const fs::path dir = TempDir("sage_pack_progress");
    PackWriter writer;
    for (int i = 0; i < 100; ++i) writer.Add("f" + std::to_string(i) + ".txt", Bytes("данные"));
    size_t last = 0, calls = 0;
    CHECK_TRUE(writer.Save(dir / "ok.sagepak", [&](size_t done, size_t total) {
        CHECK_EQ((int)total, 100);
        last = done;
        ++calls;
        return true;
    }));
    CHECK_EQ((int)last, 100);
    CHECK_TRUE(calls > 1);

    CHECK_FALSE(writer.Save(dir / "cancel.sagepak", [](size_t, size_t) { return false; }));
    CHECK_FALSE(fs::exists(dir / "cancel.sagepak"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Файлы с диска читаются лениво, и имена — в UTF-8: папка «Уровни» на
// Windows уходила в пакет байтами ANSI и потом не находилась.
TEST(Pack_add_directory_keeps_utf8_names) {
    const fs::path dir = TempDir("sage_pack_utf8");
    fs::create_directories(dir / "proj" / fs::u8path(u8"Уровни"));
    { std::ofstream f(dir / "proj" / fs::u8path(u8"Уровни") / fs::u8path(u8"первый.sage")); f << "{}"; }
    PackWriter writer;
    CHECK_EQ((int)writer.AddDirectory(dir / "proj", {}, "assets"), 1);
    CHECK_TRUE(writer.Save(dir / "game.sagepak"));
    PackReader reader;
    CHECK_TRUE(reader.Open(dir / "game.sagepak"));
    CHECK_TRUE(reader.Contains(u8"assets/Уровни/первый.sage"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Абсолютный путь внутри папки игры — тот же, что относительный: движок ищет
// свои ресурсы «рядом с exe» абсолютным путём, а они теперь в пакете. Путь с
// «..» внутри (так модели ссылаются на свои текстуры) тоже находится.
TEST(Pack_vfs_finds_absolute_and_dotdot_paths_inside_the_game) {
    namespace vfs = sage::assets::vfs;
    const fs::path dir = TempDir("sage_pack_abs");
    PackWriter writer;
    writer.Add("assets/fonts/ui.ttf", Bytes("шрифт"));
    writer.Add("assets/tex/a.png", Bytes("картинка"));
    CHECK_TRUE(writer.Save(dir / "game.sagepak"));
    CHECK_TRUE(vfs::Mount(dir / "game.sagepak"));
    std::string text;
    const std::string abs = (fs::absolute(dir) / "assets" / "fonts" / "ui.ttf").generic_string();
    CHECK_TRUE(vfs::Exists(abs));
    CHECK_TRUE(vfs::ReadText(abs, text));
    CHECK_TRUE(text == "шрифт");
    CHECK_TRUE(vfs::ReadText("assets/models/../tex/a.png", text));
    CHECK_TRUE(text == "картинка");
    CHECK_FALSE(vfs::ListFiles(fs::absolute(dir / "assets" / "tex").generic_string(), ".png").empty());
    vfs::Unmount();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ЗАГРУЗЧИКИ ЧИТАЮТ ИЗ ПАКЕТА. stb, tinyobj и tinygltf открывали файлы сами, и
// в собранной игре не грузилась ни одна текстура и модель — пакет был
// подключён, а читали мимо него (так и выглядел лог игрока: «can't fopen»).
TEST(Pack_loaders_read_images_and_obj_models_from_the_package) {
    namespace vfs = sage::assets::vfs;
    const fs::path dir = TempDir("sage_pack_loaders");
    // Картинка 2x2 в TGA без сжатия — её понимает stb, и собрать её руками просто.
    std::vector<uint8_t> tga = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 24, 0};
    for (int i = 0; i < 4; ++i) { tga.push_back(10); tga.push_back(20); tga.push_back(200); }
    const std::string obj = "mtllib box.mtl\nusemtl Red\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    const std::string mtl = "newmtl Red\nKd 1 0 0\n";
    PackWriter writer;
    writer.Add("assets/tex/dot.tga", tga);
    writer.Add("assets/models/box.obj", Bytes(obj));
    writer.Add("assets/models/box.mtl", Bytes(mtl));
    CHECK_TRUE(writer.Save(dir / "game.sagepak"));
    CHECK_TRUE(vfs::Mount(dir / "game.sagepak"));

    int w = 0, h = 0, c = 0;
    unsigned char* px = vfs::LoadImage("assets/tex/dot.tga", &w, &h, &c, 4);
    CHECK_TRUE(px != nullptr);
    CHECK_EQ(w, 2);
    CHECK_EQ(h, 2);
    if (px) {
        CHECK_EQ((int)px[0], 200);   // BGR -> RGBA: красный канал
        stbi_image_free(px);
    }

    std::string objText, mtlText;
    CHECK_TRUE(sage::assets::ReadObjWithMtl("assets/models/box.obj", objText, mtlText));
    CHECK_TRUE(mtlText.find("newmtl Red") != std::string::npos);
    const sage::render::MeshData mesh = ModelLoader::LoadMeshData("assets/models/box.obj");
    CHECK_EQ((int)mesh.Indices.size(), 3);

    vfs::Unmount();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ПАПКА СБОРОК ВНУТРИ ПРОЕКТА В ПАКЕТ НЕ ЕДЕТ: иначе каждая сборка везла бы в
// себе предыдущую игру, и пакет рос бы с каждым нажатием «Собрать».
TEST(Pack_add_directory_skips_the_builds_folder) {
    const fs::path dir = TempDir("sage_pack_skipdirs");
    fs::create_directories(dir / "scenes");
    fs::create_directories(dir / "Builds" / "Game");
    fs::create_directories(dir / "BuildsNotes");
    { std::ofstream f(dir / "scenes" / "main.sage"); f << "{}"; }
    { std::ofstream f(dir / "Builds" / "Game" / "game.sagepak"); f << "old"; }
    { std::ofstream f(dir / "BuildsNotes" / "todo.txt"); f << "x"; }
    PackWriter writer;
    CHECK_EQ((int)writer.AddDirectory(dir, {}, "", {dir / "Builds"}), 2);
    CHECK_TRUE(writer.Has("scenes/main.sage"));
    CHECK_TRUE(writer.Has("BuildsNotes/todo.txt"));
    CHECK_FALSE(writer.Has("Builds/Game/game.sagepak"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}
