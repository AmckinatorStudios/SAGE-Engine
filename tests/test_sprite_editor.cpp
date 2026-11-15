// ===========================================================================
//  РЕДАКТОР СПРАЙТОВ: формат листа v2, живой предпросмотр сетки, острова,
//  «добавить/заменить», ручки, точка опоры, история, сохранение двух файлов
//  (editor/src/sprite/SpriteEditModel.h, sage/ui/SpriteSheet.h).
// ===========================================================================
#include "TestFramework.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "../editor/src/sprite/SpriteEditModel.h"
#include "sage/core/Paths.h"
#include "sage/ui/SpriteSheet.h"

namespace fs = std::filesystem;
namespace sp = sage::editor::sprite;
using sage::ui::NineSlice;
using sage::ui::SliceFill;
using sage::ui::SpriteGrid;
using sage::ui::SpriteRegion;
using sage::ui::SpriteSheet;

namespace {
std::vector<unsigned char> Boxes(int w, int h, std::initializer_list<glm::ivec4> boxes) {
    std::vector<unsigned char> px((size_t)w * h * 4, 0);
    for (const glm::ivec4& b : boxes)
        for (int y = b.y; y < b.y + b.w; ++y)
            for (int x = b.x; x < b.x + b.z; ++x) px[((size_t)y * w + x) * 4 + 3] = 255;
    return px;
}

fs::path Scratch(const char* name) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / name;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

std::string ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
} // namespace

TEST(SpriteEditor_grid_preview_marks_cells_outside_and_empty) {
    // 40×16, клетки 16×16: две целиком внутри, третья (32..48) выходит за край.
    const auto px = Boxes(40, 16, {{0, 0, 16, 16}});
    SpriteGrid g;
    g.CellW = g.CellH = 16;
    const auto cells = sage::ui::GridCells(40, 16, g, px.data());
    CHECK_EQ((int)cells.size(), 3);
    CHECK_TRUE(cells[0].Inside && !cells[0].Empty);
    CHECK_TRUE(cells[1].Inside && cells[1].Empty);
    CHECK_TRUE(!cells[2].Inside);
    // В куски идут только годные: пустая пропущена, вылезшая — тоже.
    CHECK_EQ((int)sp::GridRects(40, 16, g, px.data()).size(), 1);
    g.SkipEmpty = false;
    CHECK_EQ((int)sp::GridRects(40, 16, g, px.data()).size(), 2);
}

TEST(SpriteEditor_grid_margin_and_offset_limit_the_area) {
    // Поле 4 со всех сторон у 40×40: область 32×32, клетки 16 — четыре.
    SpriteGrid g;
    g.CellW = g.CellH = 16;
    g.SkipEmpty = false;
    g.Margin = glm::ivec4(4);
    auto rects = sp::GridRects(40, 40, g, nullptr);
    CHECK_EQ((int)rects.size(), 4);
    CHECK_EQ(rects[0].x, 4);
    CHECK_EQ(rects[0].y, 4);
    // Сдвиг 8 по X внутри области: в строке остаётся одна клетка.
    g.OffX = 8;
    rects = sp::GridRects(40, 40, g, nullptr);
    CHECK_EQ((int)rects.size(), 2);
    CHECK_EQ(rects[0].x, 12);
    // Промежутки: клетки 8 через 2 в 30 пикселях — 0, 10, 20.
    SpriteGrid s;
    s.CellW = s.CellH = 8;
    s.PadX = s.PadY = 2;
    s.SkipEmpty = false;
    rects = sp::GridRects(30, 8, s, nullptr);
    CHECK_EQ((int)rects.size(), 3);
    CHECK_EQ(rects[2].x, 20);
}

TEST(SpriteEditor_islands_respect_min_size_and_padding) {
    const auto px = Boxes(40, 20, {{5, 5, 6, 4}, {30, 10, 1, 1}});
    sage::ui::SpriteAuto p;
    p.MinW = p.MinH = 2;
    auto found = sage::ui::FindIslands(px.data(), 40, 20, p);
    CHECK_EQ((int)found.size(), 1);   // точка 1×1 — пыль
    CHECK_EQ(found[0].x, 5);
    CHECK_EQ(found[0].z, 6);
    p.Padding = 2;
    found = sage::ui::FindIslands(px.data(), 40, 20, p);
    CHECK_EQ(found[0].x, 3);
    CHECK_EQ(found[0].y, 3);
    CHECK_EQ(found[0].z, 10);
    CHECK_EQ(found[0].w, 8);
    // Min Width и Min Height — каждый сам по себе: остров 6×4 уже 7 по
    // ширине — пропущен, хотя по высоте годен.
    p.Padding = 0;
    p.MinW = 6;
    p.MinH = 4;
    CHECK_EQ((int)sage::ui::FindIslands(px.data(), 40, 20, p).size(), 1);
    p.MinW = 7;
    CHECK_EQ((int)sage::ui::FindIslands(px.data(), 40, 20, p).size(), 0);
    p.MinW = 1;
    p.MinH = 5;
    CHECK_EQ((int)sage::ui::FindIslands(px.data(), 40, 20, p).size(), 0);
}

TEST(SpriteEditor_add_keeps_existing_and_skips_twins_replace_clears) {
    SpriteSheet s;
    SpriteRegion keep;
    keep.Name = "hero";
    keep.Rect = {0, 0, 16, 16};
    keep.Pivot = {0.5f, 1.0f};
    s.Regions.push_back(keep);
    const std::vector<glm::ivec4> rects = {{0, 0, 16, 16}, {16, 0, 16, 16}};
    // Добавить: кусок на месте «hero» второй раз не заводится, его точка
    // опоры не затирается.
    CHECK_EQ(sp::ApplySlices(s, rects, "cell", sp::SliceApply::Add), 1);
    CHECK_EQ((int)s.Regions.size(), 2);
    CHECK_NEAR(s.Find("hero")->Pivot.y, 1.0f, 1e-6f);
    CHECK_TRUE(s.Find("cell_0") != nullptr);
    // Заменить: прежние куски уходят целиком.
    CHECK_EQ(sp::ApplySlices(s, rects, "cell", sp::SliceApply::Replace), 2);
    CHECK_TRUE(s.Find("hero") == nullptr);
    CHECK_TRUE(s.Find("cell_1") != nullptr);
}

TEST(SpriteEditor_hit_prefers_the_smallest_and_marquee_hits_overlaps) {
    SpriteSheet s;
    SpriteRegion big, small;
    big.Name = "frame";
    big.Rect = {0, 0, 64, 64};
    small.Name = "gem";
    small.Rect = {10, 10, 8, 8};
    s.Regions = {small, big};   // крупная — позже, «сверху»
    CHECK_EQ(sp::HitTest(s, {12.0f, 12.0f}), 0);
    CHECK_EQ(sp::HitTest(s, {40.0f, 40.0f}), 1);
    CHECK_EQ(sp::HitTest(s, {80.0f, 1.0f}), -1);
    CHECK_EQ((int)sp::HitRect(s, {0, 0, 5, 5}).size(), 1);
    CHECK_EQ((int)sp::HitRect(s, {9, 9, 3, 3}).size(), 2);
}

TEST(SpriteEditor_handles_never_flip_or_leave_the_texture) {
    const glm::ivec4 r{10, 10, 20, 20};
    // Левая сторона дальше правой — прямоугольник не выворачивается.
    glm::ivec4 a = sp::ResizeRect(r, sp::HLeft, {50, 0}, 64, 64);
    CHECK_EQ(a.z, 1);
    CHECK_EQ(a.x, 29);
    // Правый нижний угол за картинку — упирается в край.
    a = sp::ResizeRect(r, sp::HBottomRight, {100, 100}, 64, 64);
    CHECK_EQ(a.x + a.z, 64);
    CHECK_EQ(a.y + a.w, 64);
    // Сдвиг упирается в края целиком, размер не меняется.
    a = sp::MoveRect(r, {-50, 100}, 64, 64);
    CHECK_EQ(a.x, 0);
    CHECK_EQ(a.y, 44);
    CHECK_EQ(a.z, 20);
    // Рамка по двум углам — в любом порядке, внутри картинки.
    a = sp::RectFromCorners({30, 40}, {-5, 10}, 64, 64);
    CHECK_EQ(a.x, 0);
    CHECK_EQ(a.y, 10);
    CHECK_EQ(a.z, 30);
    CHECK_EQ(a.w, 30);
}

TEST(SpriteEditor_pivot_presets_and_custom) {
    CHECK_NEAR(sp::PivotPreset(0).x, 0.0f, 1e-6f);
    CHECK_NEAR(sp::PivotPreset(4).x, 0.5f, 1e-6f);
    CHECK_NEAR(sp::PivotPreset(7).y, 1.0f, 1e-6f);   // низ по центру
    CHECK_EQ(sp::PivotPresetOf({0.5f, 1.0f}), 7);
    CHECK_EQ(sp::PivotPresetOf({0.3f, 0.7f}), sp::kPivotCustom);
}

TEST(SpriteEditor_names_unique_and_borders_clamped) {
    SpriteSheet s;
    SpriteRegion a, b;
    a.Name = "a";
    b.Name = "b";
    s.Regions = {a, b};
    CHECK_TRUE(!sp::NameFree(s, "b", 0));
    CHECK_TRUE(sp::NameFree(s, "b", 1));   // своё имя — не занято
    CHECK_TRUE(!sp::NameFree(s, "", 0));
    CHECK_TRUE(sp::NameFree(s, "c", 0));
    // Поля девятины не перехлёстываются и не отрицательны.
    const glm::vec4 c = sp::ClampBorder({-3.0f, 10.0f, 30.0f, 12.0f}, 20.0f, 16.0f);
    CHECK_NEAR(c.x, 0.0f, 1e-6f);
    CHECK_NEAR(c.z, 20.0f, 1e-6f);
    CHECK_NEAR(c.y + c.w, 16.0f, 1e-6f);
}

TEST(SpriteEditor_history_undo_redo_and_new_edit_clears_redo) {
    sp::SpriteDoc doc;
    sp::SpriteHistory h;
    h.Push(doc);
    doc.Sheet.Regions.push_back(SpriteRegion{"one"});
    h.Push(doc);
    doc.Sheet.Regions.push_back(SpriteRegion{"two"});
    CHECK_TRUE(h.Undo(doc));
    CHECK_EQ((int)doc.Sheet.Regions.size(), 1);
    CHECK_TRUE(h.Redo(doc));
    CHECK_EQ((int)doc.Sheet.Regions.size(), 2);
    CHECK_TRUE(h.Undo(doc));
    CHECK_TRUE(h.Undo(doc));
    CHECK_EQ((int)doc.Sheet.Regions.size(), 0);
    CHECK_TRUE(!h.Undo(doc));
    h.Push(doc);   // новая правка — повторять больше нечего
    CHECK_TRUE(!h.CanRedo());
    // Отмена до сохранённого вида снимает «несохранено».
    sp::SpriteDoc saved;
    CHECK_EQ(sp::Fingerprint(doc), sp::Fingerprint(saved));
}

TEST(SpriteEditor_sheet_v2_keeps_slicing_pivot_and_per_sprite_nineslice) {
    SpriteSheet s;
    s.Slicing.Method = sage::ui::SpriteSlicing::Mode::Automatic;
    s.Slicing.Grid.CellW = 24;
    s.Slicing.Grid.Margin = {1, 2, 3, 4};
    s.Slicing.Grid.SkipEmpty = false;
    s.Slicing.Auto.AlphaCutoff = 0.25f;
    s.Slicing.Auto.Padding = 3;
    SpriteRegion frame;
    frame.Name = "frame";
    frame.Rect = {0, 0, 32, 32};
    frame.Slice = {8, 6, 8, 6};
    frame.EdgeFill = SliceFill::Tile;
    frame.DrawCenter = false;
    frame.Pivot = {0.0f, 1.0f};
    SpriteRegion button;
    button.Name = "button";
    button.Rect = {32, 0, 32, 16};
    button.Slice = {4, 4, 4, 4};   // своя девятина: заливка не наследуется от frame
    s.Regions = {frame, button};
    const std::string text = s.ToJsonString();
    CHECK_TRUE(text.find("\"version\": 2") != std::string::npos);
    SpriteSheet r;
    std::string err;
    CHECK_TRUE(SpriteSheet::FromJsonString(text, r, err));
    CHECK_TRUE(r.Slicing.Method == sage::ui::SpriteSlicing::Mode::Automatic);
    CHECK_EQ(r.Slicing.Grid.CellW, 24);
    CHECK_EQ(r.Slicing.Grid.Margin.w, 4);
    CHECK_TRUE(!r.Slicing.Grid.SkipEmpty);
    CHECK_NEAR(r.Slicing.Auto.AlphaCutoff, 0.25f, 1e-6f);
    CHECK_EQ(r.Slicing.Auto.Padding, 3);
    const SpriteRegion* f = r.Find("frame");
    CHECK_TRUE(f && f->EdgeFill == SliceFill::Tile && !f->DrawCenter);
    CHECK_NEAR(f->Pivot.y, 1.0f, 1e-6f);
    const NineSlice n = f->NineSliceOf();
    CHECK_NEAR(n.Top, 6.0f, 1e-6f);
    CHECK_TRUE(!n.DrawCenter);
    const SpriteRegion* b = r.Find("button");
    CHECK_TRUE(b && b->EdgeFill == SliceFill::Stretch && b->DrawCenter);
}

TEST(SpriteEditor_old_sheet_loads_with_defaults) {
    // Лист прошлой версии: без версии, девятина ключом «slice», без точки
    // опоры и без настроек нарезки.
    const std::string old = R"({"sprites":[{"name":"panel","rect":[0,0,48,48],"slice":[8,8,8,8]},
                                           {"name":"icon","rect":[48,0,16,16]}]})";
    SpriteSheet s;
    std::string err;
    CHECK_TRUE(SpriteSheet::FromJsonString(old, s, err));
    CHECK_EQ((int)s.Regions.size(), 2);
    CHECK_NEAR(s.Find("panel")->Slice.x, 8.0f, 1e-6f);
    CHECK_NEAR(s.Find("icon")->Pivot.x, 0.5f, 1e-6f);   // по умолчанию — середина
    CHECK_NEAR(s.Find("icon")->Pivot.y, 0.5f, 1e-6f);
    CHECK_TRUE(s.Find("icon")->DrawCenter);
    CHECK_TRUE(s.Slicing.Method == sage::ui::SpriteSlicing::Mode::Grid);
}

TEST(SpriteEditor_save_writes_only_what_exists_atomically) {
    const fs::path dir = Scratch("sage_sprite_editor_save");
    const std::string image = sage::PathToUtf8(dir / "panel.png");
    std::string err;
    // Только девятина картинки — листа рядом не появляется.
    sp::SpriteDoc doc;
    doc.Texture.SetBorder({4, 4, 4, 4});
    CHECK_TRUE(sp::SaveDoc(image, doc, err));
    CHECK_TRUE(fs::exists(dir / "panel.png.sage9"));
    CHECK_TRUE(!fs::exists(dir / "panel.png.sagesheet"));
    // С кусками — лист есть; временных файлов не остаётся.
    doc.Sheet.Regions.push_back(SpriteRegion{"a", {0, 0, 8, 8}});
    CHECK_TRUE(sp::SaveDoc(image, doc, err));
    CHECK_TRUE(fs::exists(dir / "panel.png.sagesheet"));
    CHECK_TRUE(!fs::exists(dir / "panel.png.sagesheet.tmp"));
    CHECK_TRUE(!fs::exists(dir / "panel.png.sage9.tmp"));
    // Открыть — то же самое.
    sp::SpriteDoc back;
    CHECK_TRUE(sp::LoadDoc(image, back, err));
    CHECK_EQ(sp::Fingerprint(back), sp::Fingerprint(doc));
    // Опустошённый лист сохраняется пустым, а не остаётся старым.
    doc.Sheet.Regions.clear();
    CHECK_TRUE(sp::SaveDoc(image, doc, err));
    CHECK_TRUE(ReadAll(dir / "panel.png.sagesheet").find("\"a\"") == std::string::npos);
    // Запись в несуществующую папку — понятная ошибка, а не тишина.
    std::string err2;
    sp::SpriteDoc full;
    full.Sheet.Regions.push_back(SpriteRegion{"b", {0, 0, 4, 4}});
    CHECK_TRUE(!sp::SaveDoc(sage::PathToUtf8(dir / "no" / "such" / "x.png"), full, err2));
    CHECK_TRUE(!err2.empty());
    std::error_code ec;
    fs::remove_all(dir, ec);
}
