#include "TestFramework.h"

#include <vector>

#include "sage/assets/SpriteSheet.h"
#include <limits>
#include "sage/assets/SpriteResolver.h"
#include "sage/assets/Pack.h"
#include "sage/assets/GameBuild.h"
#include "sage/core/AtomicFile.h"
#include "sage/core/Paths.h"
#include <filesystem>

// ---------------------------------------------------------------------------
// ЛИСТ СПРАЙТОВ: нарезка сеткой и по островам, имена, файл описания.
// ---------------------------------------------------------------------------
using sage::assets::SpriteGrid;
using sage::assets::SpriteSheet;

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
    CHECK_EQ(s.Regions()[0].Rect.x, 20);    // верхний: y = 2
    CHECK_EQ(s.Regions()[1].Rect.x, 2);     // y = 3
    CHECK_EQ(s.Regions()[2].Rect.y, 20);
    CHECK_EQ(s.Regions()[2].Rect.z, 10);
    CHECK_EQ(s.Regions()[2].Rect.w, 6);
    // Строки снизу вверх дают те же прямоугольники от верхнего угла.
    std::vector<unsigned char> flipped(px.size());
    for (int y = 0; y < 30; ++y)
        std::copy(px.begin() + (size_t)y * 40 * 4, px.begin() + (size_t)(y + 1) * 40 * 4,
                  flipped.begin() + (size_t)(29 - y) * 40 * 4);
    SpriteSheet f;
    CHECK_EQ(f.SliceAuto(flipped.data(), 40, 30, true, 0.05f, 2, "ico"), 3);
    CHECK_EQ(f.Regions()[2].Rect.y, 20);
}

TEST(SpriteSheet_names_are_unique_and_survive_the_file) {
    SpriteSheet s;
    s.Set({"slot", {0, 0, 18, 18}, glm::vec4(0.0f)});
    s.Set({"panel", {0, 18, 26, 26}, glm::vec4(4, 4, 4, 4)});
    CHECK_FALSE(s.Rename("slot", "panel"));       // имя занято
    CHECK_TRUE(s.Rename("slot", "cell"));
    s.Set({"cell", {1, 1, 16, 16}, glm::vec4(0.0f)});   // замена, не второй кусок
    CHECK_EQ((int)s.Regions().size(), 2);

    SpriteSheet back;
    std::string err;
    CHECK_TRUE(SpriteSheet::FromJsonString(s.ToJsonString(), back, err));
    CHECK_EQ((int)back.Regions().size(), 2);
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

TEST(SpriteSheet_legacy_ids_survive_rename_and_round_trip) {
    const std::string legacy = R"({"version":2,"sprites":[{"name":"run_1","rect":[0,0,2,2]},
        {"name":"run_2","rect":[2,0,2,2]}],"sequences":[{"name":"run","frames":["run_1","run_2"],"fps":2}]})";
    SpriteSheet sheet, reopened;
    std::string err;
    const bool migrated = SpriteSheet::FromJsonString(legacy, sheet, err) &&
                          SpriteSheet::FromJsonString(legacy, reopened, err);
    CHECK_TRUE(migrated);
    if (!migrated) return;
    const auto region = sheet.Find("run_1")->Id();
    const auto sequence = sheet.FindSequence("run")->Id();
    CHECK_TRUE(region.Valid() && sequence.Valid() && region != sequence);
    const sage::assets::SpriteRef ref{sage::AssetGuid{4, 1}, sequence, sage::assets::SpriteRef::Kind::Sequence};
    CHECK_TRUE(ref.Valid());
    CHECK_TRUE(reopened.Find("run_1")->Id() == region);
    CHECK_TRUE(reopened.FindSequence("run")->Id() == sequence);
    CHECK_TRUE(sheet.Rename("run_1", "start"));
    const bool loaded = SpriteSheet::FromJsonString(sheet.ToJsonString(), reopened, err);
    CHECK_TRUE(loaded);
    if (!loaded) return;
    CHECK_TRUE(reopened.Find(region) && reopened.Find(region)->Name == "start");
    CHECK_TRUE(reopened.FindSequence(sequence)->FrameIdAt(0) == region);
    CHECK_TRUE(reopened.Remove("start"));
    CHECK_TRUE(reopened.FindSequence(sequence)->FrameAt(0) == "run_2");
    CHECK_TRUE(reopened.Remove("run_2"));
    CHECK_FALSE(reopened.FindSequence(sequence)->FrameIdAt(0).Valid());
}

TEST(SpriteSheet_validation_preserves_destination_on_failure) {
    SpriteSheet sheet;
    std::string err;
    sheet.Set(sage::assets::SpriteRegion("keep", {0, 0, 1, 1}));
    const auto original = sheet.ToJsonString();
    for (const char* json : {
             R"({"version":4})",
             R"({"sprites":[{"name":"x","rect":[0,0,0,2]}]})",
             R"({"sprites":[{"name":"x","rect":[0,0,2,2],"pivot":[2,0]}]})",
             R"({"sprites":[{"name":"x","rect":[0,0,2,2],"slice":[-1,0,0,0]}]})",
             R"({"sequences":[{"name":"x","frames":["missing"]}]})",
             R"({"sequences":[{"name":"x","fps":0}]})",
             R"({"version":3,"textureNineSlice":{},"sprites":[{"id":"00000000000000010000000000000001","name":"x","rect":[0,0,1,1]},{"id":"00000000000000010000000000000001","name":"y","rect":[0,0,1,1]}]})"}) {
        CHECK_FALSE(SpriteSheet::FromJsonString(json, sheet, err));
        CHECK_FALSE(err.empty());
        CHECK_EQ(sheet.ToJsonString(), original);
    }
}

TEST(SpriteSheet_long_frame_numbers_and_large_timestamps) {
    using sage::assets::SpriteRegion;
    SpriteSheet sheet;
    sheet.Set(SpriteRegion("f_99999999999999999999999999999", {0, 0, 1, 1}));
    sheet.Set(SpriteRegion("f_2", {1, 0, 1, 1}));
    CHECK_EQ(sheet.SequenceFromPrefix("frames", "f_", 2), 2);
    const auto* seq = sheet.FindSequence("frames");
    CHECK_TRUE(seq != nullptr);
    if (!seq) return;
    CHECK_EQ(seq->FrameAt(0), std::string("f_2"));
    CHECK_EQ(seq->FrameAt(std::numeric_limits<float>::infinity()), std::string("f_2"));
    CHECK_TRUE(seq->FrameIdAt(std::numeric_limits<float>::max()).Valid());
    sheet.SetPlayback(0, 2, false);
    CHECK_EQ(seq->FrameAt(std::numeric_limits<float>::max()), std::string("f_99999999999999999999999999999"));
}

TEST(SpriteResolver_keeps_generations_and_resolves_packed_atlas) {
    namespace fs = std::filesystem;
    using namespace sage::assets;
    const auto root = fs::temp_directory_path() / ("sage-sprites-" + sage::AssetGuid::Generate().ToString());
    fs::create_directories(root);
    struct Cleanup {
        fs::path Path;
        ~Cleanup() { vfs::Unmount(); std::error_code ec; fs::remove_all(Path, ec); }
    } cleanup{root};
    // A 1x1 uncompressed, 24-bit BMP. Use an independent sibling image to exercise v3 atlas publication.
    const unsigned char bmp[58] = {
        0x42,0x4d,58,0,0,0,0,0,0,0,54,0,0,0,40,0,0,0,1,0,0,0,1,0,0,0,1,0,24,0,
        0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,16,32,0
    };
    const std::string pixels(reinterpret_cast<const char*>(bmp), sizeof(bmp));
    auto image = sage::PathToUtf8(root / "revision.bmp");
    auto document = sage::PathToUtf8(root / "base.bmp.sagesheet");
    std::string error;
    SpriteSheet sheet;
    sheet.Set(SpriteRegion("frame", {0, 0, 1, 1}));
    sheet.SequenceFromPrefix("animation", "frame");
    CHECK_TRUE(sheet.SetImageFile("revision.bmp"));
    CHECK_TRUE(sheet.SaveFile(document, error));
    sage::AssetDatabase database;
    database.ScanProject(sage::PathToUtf8(root));
    SpriteResolver resolver;
    SpriteRef region{database.GuidOf("base.bmp.sagesheet"), sheet.Find("frame")->Id(), SpriteRef::Kind::Region};
    SpriteRef sequence{region.Asset, sheet.FindSequence("animation")->Id(), SpriteRef::Kind::Sequence};
    CHECK_FALSE(bool(resolver.Resolve(database, region)));
    CHECK_TRUE(sage::WriteFileAtomic(image, pixels, error));
    const auto first = resolver.Resolve(database, region);
    CHECK_TRUE(bool(first));
    if (!first) return;
    CHECK_TRUE(first.Resource == resolver.Resolve(database, sequence).Resource);
    CHECK_TRUE(first.Resource == resolver.ResolveSheet(database, region.Asset).Resource);
    CHECK_TRUE(first.Resource == resolver.ResolveImage(database, "base.bmp").Resource);
    CHECK_TRUE(first.Resource == resolver.ResolveImage(database, sage::PathToUtf8(root / "base.bmp")).Resource);
    CHECK_EQ(first.UV(), glm::vec4(0, 0, 1, 1));
    CHECK_TRUE(sheet.Rename("frame", "renamed"));
    CHECK_TRUE(sheet.SaveFile(document, error));
    const auto updated = resolver.Resolve(database, sequence);
    CHECK_TRUE(bool(updated));
    if (!updated) return;
    CHECK_TRUE(first.Resource != updated.Resource);
    CHECK_TRUE(first.Resource->Image == updated.Resource->Image);
    CHECK_EQ(updated.Region()->Name, std::string("renamed"));
    CHECK_TRUE(sage::WriteFileAtomic(image, "broken file", error));
    const auto fallback = resolver.Resolve(database, region);
    CHECK_TRUE(fallback.Resource == updated.Resource);
    CHECK_FALSE(fallback.Diagnostic.empty());
    CHECK_TRUE(sage::WriteFileAtomic(image, pixels + 'x', error));
    CHECK_EQ(resolver.Poll(database, 1), size_t(1));
    CHECK_TRUE(resolver.Resolve(database, region).Resource->Image != updated.Resource->Image);

    const auto beforeMove = resolver.Resolve(database, region);
    const auto moved = root / "assets";
    fs::create_directory(moved);
    fs::rename(sage::PathFromUtf8(document), moved / "base.bmp.sagesheet");
    fs::rename(sage::PathFromUtf8(document + ".meta"), moved / "base.bmp.sagesheet.meta");
    fs::rename(sage::PathFromUtf8(image), moved / "revision.bmp");
    document = sage::PathToUtf8(moved / "base.bmp.sagesheet");
    image = sage::PathToUtf8(moved / "revision.bmp");
    database.ScanProject(sage::PathToUtf8(root));
    const auto relocated = resolver.Resolve(database, region);
    CHECK_TRUE(relocated && relocated.Resource != beforeMove.Resource);
    CHECK_TRUE(database.GuidOf("assets/base.bmp.sagesheet") == region.Asset);
    CHECK_TRUE(sage::WriteFileAtomic(document, "invalid JSON", error));
    const auto brokenSheet = resolver.Resolve(database, region);
    CHECK_TRUE(brokenSheet.Resource == relocated.Resource && !brokenSheet.Diagnostic.empty());
    CHECK_TRUE(sheet.SaveFile(document, error));
    CHECK_TRUE(resolver.Resolve(database, region).Diagnostic.empty());

    GameBuildRequest request;
    request.ProjectDir = root;
    request.OutputDir = root / "Builds";
    request.ProjectName = "SpriteFixture";
    request.Player = root / "fixture-player.bin";
    request.AssetIndex = database.ExportIndex();
    request.SpriteReferences = {region, sequence};
    CHECK_TRUE(sage::WriteFileAtomic(sage::PathToUtf8(request.Player), "fixture executable", error));
    const auto built = BuildGame(request);
    CHECK_TRUE(built.Ok);
    if (!built.Ok) return;
    request.SpriteReferences.front().Item = sage::AssetGuid::Generate();
    CHECK_FALSE(BuildGame(request).Ok);
    fs::remove(sage::PathFromUtf8(document));
    fs::remove(sage::PathFromUtf8(image));
    database.ForgetMissingProjectFiles();
    CHECK_FALSE(bool(resolver.Resolve(database, region)));
    CHECK_TRUE(vfs::Mount(built.GameDir / "game.sagepak"));
    sage::AssetDatabase packed;
    packed.LoadIndex(request.AssetIndex);
    resolver.Clear();
    CHECK_TRUE(bool(resolver.Resolve(packed, sequence)));
}
