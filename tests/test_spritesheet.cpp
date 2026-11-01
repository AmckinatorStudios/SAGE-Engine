#include "TestFramework.h"

#include <vector>

#include "sage/ui/SpriteSheet.h"

// ---------------------------------------------------------------------------
// ЛИСТ СПРАЙТОВ: нарезка сеткой и по островам, имена, файл описания.
// ---------------------------------------------------------------------------
using sage::ui::SpriteGrid;
using sage::ui::SpriteSheet;

namespace {
// Картинка W×H (строки сверху вниз), непрозрачные прямоугольники из списка.
std::vector<unsigned char> Picture(int w, int h, std::initializer_list<glm::ivec4> boxes) {
    std::vector<unsigned char> px((size_t)w * h * 4, 0);
    for (const glm::ivec4& b : boxes)
        for (int y = b.y; y < b.y + b.w; ++y)
            for (int x = b.x; x < b.x + b.z; ++x) px[((size_t)y * w + x) * 4 + 3] = 255;
    return px;
}
} // namespace

TEST(SpriteSheet_grid_skips_empty_cells_and_names_by_rows) {
    // 4×2 клеток по 8 пикселей; закрашены (0,0), (2,0) и (1,1).
    const auto px = Picture(32, 16, {{0, 0, 8, 8}, {16, 0, 8, 8}, {8, 8, 8, 8}});
    SpriteSheet s;
    SpriteGrid g;
    g.CellW = 8;
    g.CellH = 8;
    g.Prefix = "cell";
    CHECK_EQ(s.SliceGrid(32, 16, g, px.data()), 3);
    CHECK_TRUE(s.Find("cell_0") != nullptr);
    CHECK_TRUE(s.Find("cell_1") == nullptr);   // пустая клетка
    CHECK_TRUE(s.Find("cell_2") != nullptr);
    CHECK_TRUE(s.Find("cell_5") != nullptr);   // вторая строка, вторая клетка
    CHECK_EQ(s.Find("cell_5")->Rect.x, 8);
    CHECK_EQ(s.Find("cell_5")->Rect.y, 8);

    // С промежутками и полем: клетки 6×6 через 2 пикселя от (1,1).
    SpriteSheet p;
    SpriteGrid q;
    q.CellW = q.CellH = 6;
    q.PadX = q.PadY = 2;
    q.OffX = q.OffY = 1;
    q.SkipEmpty = false;
    // x: 1, 9, 17, 25 (25 + 6 <= 32); y: 1, 9 — восемь клеток.
    CHECK_EQ(p.SliceGrid(32, 16, q), 8);
    CHECK_EQ(p.Find("sprite_7")->Rect.x, 25);
    CHECK_EQ(p.Find("sprite_7")->Rect.y, 9);
}

TEST(SpriteSheet_auto_finds_islands_in_reading_order) {
    // Три острова: два в верхнем ряду (правый — первым в картинке не по x),
    // один внизу; одиночный пиксель — шум, отбрасывается.
    const auto px = Picture(40, 30, {{20, 2, 6, 5}, {2, 3, 4, 4}, {5, 20, 10, 6}, {35, 25, 1, 1}});
    SpriteSheet s;
    CHECK_EQ(s.SliceAuto(px.data(), 40, 30, false, 0.05f, 2, "ico"), 3);
    CHECK_EQ(s.Regions[0].Rect.x, 20);    // верхний: y = 2
    CHECK_EQ(s.Regions[1].Rect.x, 2);     // y = 3
    CHECK_EQ(s.Regions[2].Rect.y, 20);
    CHECK_EQ(s.Regions[2].Rect.z, 10);
    CHECK_EQ(s.Regions[2].Rect.w, 6);
    // Строки снизу вверх дают те же прямоугольники от верхнего угла.
    std::vector<unsigned char> flipped(px.size());
    for (int y = 0; y < 30; ++y)
        std::copy(px.begin() + (size_t)y * 40 * 4, px.begin() + (size_t)(y + 1) * 40 * 4,
                  flipped.begin() + (size_t)(29 - y) * 40 * 4);
    SpriteSheet f;
    CHECK_EQ(f.SliceAuto(flipped.data(), 40, 30, true, 0.05f, 2, "ico"), 3);
    CHECK_EQ(f.Regions[2].Rect.y, 20);
}

TEST(SpriteSheet_names_are_unique_and_survive_the_file) {
    SpriteSheet s;
    s.Set({"slot", {0, 0, 18, 18}, glm::vec4(0.0f)});
    s.Set({"panel", {0, 18, 26, 26}, glm::vec4(4, 4, 4, 4)});
    CHECK_FALSE(s.Rename("slot", "panel"));       // имя занято
    CHECK_TRUE(s.Rename("slot", "cell"));
    s.Set({"cell", {1, 1, 16, 16}, glm::vec4(0.0f)});   // замена, не второй кусок
    CHECK_EQ((int)s.Regions.size(), 2);

    SpriteSheet back;
    std::string err;
    CHECK_TRUE(SpriteSheet::FromJsonString(s.ToJsonString(), back, err));
    CHECK_EQ((int)back.Regions.size(), 2);
    CHECK_EQ(back.Find("cell")->Rect.z, 16);
    CHECK_TRUE(back.Find("panel")->Slice == glm::vec4(4, 4, 4, 4));
    CHECK_TRUE(back.Find("cell")->Slice == glm::vec4(0.0f));
    CHECK_FALSE(SpriteSheet::FromJsonString("не json", back, err));
    CHECK_TRUE(SpriteSheet::SidecarPath("ui/gui.png") == "ui/gui.png.sagesheet");
}

// Из скрипта: нарезать картинку сеткой, сохранить лист, взять кусок по имени.
#include <filesystem>
#include <stb_image_write.h>
#include "sage/scene/Scene.h"
#include "sage/scripting/ScriptEngine.h"
TEST(SpriteSheet_script_cuts_saves_and_reads_by_name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "sage_sheet_test";
    std::filesystem::create_directories(dir);
    const std::string file = (dir / "sheet.png").string();
    const auto px = Picture(16, 8, {{0, 0, 7, 8}, {10, 2, 4, 4}});   // два острова через прозрачный промежуток
    CHECK_TRUE(stbi_write_png(file.c_str(), 16, 8, 4, px.data(), 16 * 4) != 0);
    ScriptEngine se;
    Scene scene("P");
    se.BindScene(scene);
    se.Lua()["file"] = file;
    se.Lua().script(R"LUA(
        s = sprite_sheet(file)
        made = s.grid{w = 8, h = 8, prefix = "cell"}
        s.set("frame", 0, 0, 8, 8, {2, 2, 2, 2})
        s.save()
        again = sprite_sheet(file)
        r = again.rect("frame")
        fx, fslice = r.x, r.slice[1]
        missing = again.rect("nope") == nil
        n = again.count()
        isl = sprite_sheet(file)
        found = isl.auto{prefix = "i"}
    )LUA");
    sol::state& L = se.Lua();
    CHECK_EQ(L["made"].get<int>(), 2);
    CHECK_EQ(L["n"].get<int>(), 3);
    CHECK_EQ(L["fx"].get<int>(), 0);
    CHECK_NEAR(L["fslice"].get<float>(), 2.0f, 1e-6f);
    CHECK_TRUE(L["missing"].get<bool>());
    CHECK_EQ(L["found"].get<int>(), 2);
    std::filesystem::remove_all(dir);
}
