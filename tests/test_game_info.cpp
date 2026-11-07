// Сведения об игре (sage/assets/GameInfo.h): раздел "game" файла проекта,
// имя файла игры, иконка и ресурсы Windows (группа иконок, VERSIONINFO).
// Ресурсы собираются байтами на любой системе — здесь их разбирают обратно.
#include "TestFramework.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "sage/assets/GameInfo.h"
#include "sage/core/EngineContext.h"
#include "sage/render/Screenshot.h"

#include <stb_image.h>
#include <stb_image_write.h>

using namespace sage::assets;
namespace fs = std::filesystem;

namespace {
uint16_t U16(const std::vector<uint8_t>& b, size_t o) { return (uint16_t)(b[o] | (b[o + 1] << 8)); }
uint32_t U32(const std::vector<uint8_t>& b, size_t o) { return U16(b, o) | ((uint32_t)U16(b, o + 2) << 16); }
// Строка UTF-16 из ASCII — для поиска ключей и значений в ресурсе.
std::vector<uint8_t> Wide(const std::string& s) {
    std::vector<uint8_t> w;
    for (char c : s) { w.push_back((uint8_t)c); w.push_back(0); }
    return w;
}
bool Contains(const std::vector<uint8_t>& hay, const std::vector<uint8_t>& needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}
} // namespace

// Раздел "game" читается и пишется, чужие поля файла проекта не трогаются;
// без раздела имя игры — имя проекта, версия — 1.0.0.
TEST(GameInfo_project_section_round_trip) {
    nlohmann::json project = {{"name", "Demo"}, {"startScene", "main"}};
    GameInfo plain = GameInfo::FromProject(project);
    CHECK_EQ(plain.Name, std::string("Demo"));
    CHECK_EQ(plain.Version, std::string("1.0.0"));

    GameInfo g;
    g.Name = "Super Game";
    g.Description = "Test";
    g.Author = "Me";
    g.Version = "2.5.1";
    g.Icon = "assets/icon.png";
    g.WriteTo(project);
    CHECK_EQ(project["startScene"].get<std::string>(), std::string("main"));
    const GameInfo back = GameInfo::FromProject(project);
    CHECK_EQ(back.Name, std::string("Super Game"));
    CHECK_EQ(back.Author, std::string("Me"));
    CHECK_EQ(back.Icon, std::string("assets/icon.png"));

    // Через файл: чтение-правка-запись тоже бережёт остальные поля.
    const fs::path dir = fs::temp_directory_path() / "sage_gameinfo_test";
    fs::create_directories(dir);
    { std::ofstream(dir / "project.sageproj") << R"({"name": "Demo", "startScene": "level1"})"; }
    CHECK_TRUE(SaveGameInfo(dir, g));
    GameInfo loaded;
    CHECK_TRUE(LoadGameInfo(dir, loaded));
    CHECK_EQ(loaded.Version, std::string("2.5.1"));
    std::ifstream in(dir / "project.sageproj");
    const nlohmann::json j = nlohmann::json::parse(in);
    CHECK_EQ(j["startScene"].get<std::string>(), std::string("level1"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(GameInfo_file_name_and_version_numbers) {
    GameInfo g;
    g.Name = " My: Game? <2>. ";
    CHECK_EQ(g.FileName(), std::string("My Game 2"));
    g.Name = "???";
    CHECK_EQ(g.FileName(), std::string("Game"));
    g.Version = "1.12.3-beta";
    uint16_t v[4];
    g.VersionNumbers(v);
    CHECK_EQ(v[0], (uint16_t)1);
    CHECK_EQ(v[1], (uint16_t)12);
    CHECK_EQ(v[2], (uint16_t)3);
    CHECK_EQ(v[3], (uint16_t)0);
}

// Группа иконок: 7 размеров от 16 до 256, у каждого — PNG, номера подряд;
// 256 записан нулём (так требует формат).
TEST(GameInfo_icon_group_lists_png_images) {
    IconImage src;
    src.Width = src.Height = 64;
    src.Rgba.assign(64 * 64 * 4, 255);
    const IconResources r = BuildIconResources(src, 1);
    CHECK_EQ(r.Images.size(), (size_t)7);
    CHECK_EQ(U16(r.Group, 2), (uint16_t)1);        // тип — иконка
    CHECK_EQ(U16(r.Group, 4), (uint16_t)7);        // число картинок
    const uint8_t pngSig[4] = {0x89, 'P', 'N', 'G'};
    for (size_t i = 0; i < r.Images.size(); ++i) {
        const size_t e = 6 + i * 14;
        CHECK_EQ(U32(r.Group, e + 8), (uint32_t)r.Images[i].size());
        CHECK_EQ(U16(r.Group, e + 12), (uint16_t)(1 + i));
        CHECK_TRUE(std::memcmp(r.Images[i].data(), pngSig, 4) == 0);
    }
    CHECK_EQ(r.Group[6 + 6 * 14], (uint8_t)0);     // 256 -> 0
    CHECK_EQ(r.Group[6], (uint8_t)16);
}

// Уменьшение с учётом прозрачности: невидимые пиксели не красят край.
TEST(GameInfo_icon_resize_ignores_transparent_color) {
    IconImage src;
    src.Width = 2;
    src.Height = 2;
    // Слева красный, справа прозрачный синий.
    src.Rgba = {255, 0, 0, 255,   0, 0, 255, 0,
                255, 0, 0, 255,   0, 0, 255, 0};
    const IconImage one = ResizeIcon(src, 1);
    CHECK_EQ(one.Rgba[0], (uint8_t)255);           // красный, без синевы
    CHECK_EQ(one.Rgba[2], (uint8_t)0);
    CHECK_TRUE(one.Rgba[3] > 100 && one.Rgba[3] < 160);   // половина покрыта
}

// VERSIONINFO: длина корня — весь ресурс, подпись VS_FIXEDFILEINFO на месте,
// версия числами, строки (название, автор, версия) — UTF-16.
TEST(GameInfo_version_resource_layout) {
    GameInfo g;
    g.Name = "SAGE Craft";
    g.Author = "Amckinator";
    g.Description = "Voxel sandbox";
    g.Version = "1.7.10";
    const std::vector<uint8_t> v = BuildVersionResource(g, "SAGE Craft.exe");
    CHECK_EQ((size_t)U16(v, 0), v.size());
    CHECK_EQ(U16(v, 2), (uint16_t)52);             // sizeof(VS_FIXEDFILEINFO)
    CHECK_TRUE(Contains(v, Wide("VS_VERSION_INFO")));
    // Ключ (16 символов с нулём) после 6 байт заголовка, затем выравнивание.
    size_t fixed = 6 + 16 * 2;
    while (fixed % 4) ++fixed;
    CHECK_EQ(U32(v, fixed), 0xFEEF04BDu);
    CHECK_EQ(U32(v, fixed + 8), (1u << 16) | 7u);   // 1.7
    CHECK_EQ(U32(v, fixed + 12), (10u << 16) | 0u); // .10.0
    CHECK_TRUE(Contains(v, Wide("ProductName")));
    CHECK_TRUE(Contains(v, Wide("SAGE Craft")));
    CHECK_TRUE(Contains(v, Wide("CompanyName")));
    CHECK_TRUE(Contains(v, Wide("Amckinator")));
    CHECK_TRUE(Contains(v, Wide("Voxel sandbox")));
    CHECK_TRUE(Contains(v, Wide("Translation")));
}

// Иконка читается и без запущенного движка: так работает сборщик игры из
// командной строки (sage_build). Раньше чтение спрашивало базу ассетов и
// бросало «подсистема вне контекста движка» — сборка игры на Windows падала.
TEST(GameInfo_icon_loads_without_engine_context) {
    const fs::path dir = fs::temp_directory_path() / "sage_icon_noctx";
    fs::create_directories(dir);
    const fs::path png = dir / "icon.png";
    const unsigned char px[2 * 2 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    CHECK_TRUE(stbi_write_png(png.string().c_str(), 2, 2, 4, px, 2 * 4) != 0);

    // Как после загрузки текстур на этом потоке: иконка всё равно сверху вниз.
    stbi_set_flip_vertically_on_load_thread(true);
    sage::EngineContext* was = sage::EngineContext::SetCurrent(nullptr);
    IconImage icon;
    bool ok = false;
    try {
        ok = LoadIconImage(png.string(), icon);
    } catch (...) {
        ok = false;
    }
    sage::EngineContext::SetCurrent(was);
    CHECK_TRUE(ok);
    CHECK_EQ(icon.Width, 2);
    if (icon.Rgba.size() == 16) CHECK_EQ((int)icon.Rgba[0], 255);   // левый верхний — красный
    stbi_set_flip_vertically_on_load_thread(false);
    fs::remove_all(dir);
}

// Имя снимка экрана: ГГГГ-ММ-ДД_ЧЧ.ММ.СС.png, а второй за ту же секунду — _2,
// а не перезапись первого.
TEST(Screenshot_file_name_by_time_without_overwrite) {
    const fs::path dir = fs::temp_directory_path() / "sage_shots_name";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::tm t{};
    t.tm_year = 2026 - 1900;
    t.tm_mon = 10;
    t.tm_mday = 7;
    t.tm_hour = 4;
    t.tm_min = 5;
    t.tm_sec = 9;
    const std::string first = sage::render::ScreenshotFileName(dir.string(), t);
    CHECK_TRUE(first == (dir / "2026-11-07_04.05.09.png").string() ||
               first == dir.string() + "/2026-11-07_04.05.09.png");
    std::ofstream(fs::path(first)) << "x";
    const std::string second = sage::render::ScreenshotFileName(dir.string(), t);
    CHECK_TRUE(second.find("2026-11-07_04.05.09_2.png") != std::string::npos);
    fs::remove_all(dir);
}
