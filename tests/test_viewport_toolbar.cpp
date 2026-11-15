// ===========================================================================
//  ПАНЕЛЬ ИНСТРУМЕНТОВ И ИНФОРМАЦИЯ ВЬЮПОРТА (editor/src/panels/ViewportToolbarRules.h)
//  — чистая логика: перенос групп в узком виде, состав строк
//  информационного слоя по настройкам, размещение слоя без пересечений,
//  доступность правки коллайдера, подпись шагов привязки, сохранение
//  настроек слоя.
// ===========================================================================
#include "TestFramework.h"

#include <string>

#include "../editor/src/panels/ViewportToolbarRules.h"
#include "sage/physics/PhysicsComponents.h"

namespace vp = sage::editor::vptools;

namespace {

bool Has(const std::vector<std::string>& lines, const std::string& part) {
    for (const std::string& l : lines)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

vp::OverlayInput Sample() {
    vp::OverlayInput in;
    in.ViewName = "Perspective";
    in.CameraPos = {1.5f, 2.25f, -3.0f};
    in.Fov = 60.0f;
    in.SelectedCount = 1;
    in.SelectedName = "Red Cube";
    in.Fps = 59.6f;
    in.DrawCalls = 12;
    in.Triangles = 89442;
    in.Objects = 8;
    in.MemoryGb = 0.41;
    return in;
}

} // namespace

TEST(ViewportToolbar_groups_wrap_in_a_narrow_view) {
    const std::vector<float> w = {96, 110, 120, 52, 24};
    // Широко — всё в одну строку.
    for (int r : vp::FlowRows(w, 12.0f, 2000.0f)) CHECK_EQ(r, 0);
    // Узко — переносы, но основные инструменты всегда в первой строке, и ни
    // одна строка не шире вида (кроме группы, которая шире сама по себе).
    const float maxW = 260.0f;
    const std::vector<int> rows = vp::FlowRows(w, 12.0f, maxW);
    CHECK_EQ(rows[0], 0);
    CHECK_TRUE(rows.back() > 0);
    float x = 0.0f;
    int row = 0;
    for (size_t i = 0; i < w.size(); ++i) {
        if (rows[i] != row) { row = rows[i]; x = 0.0f; }
        x += (x > 0.0f ? 12.0f : 0.0f) + w[i];
        CHECK_TRUE(x <= maxW || x == w[i]);
    }
    // Порядок групп сохраняется: строки не убывают.
    for (size_t i = 1; i < rows.size(); ++i) CHECK_TRUE(rows[i] >= rows[i - 1]);
}

TEST(ViewportToolbar_overlay_parts_switch_independently) {
    const vp::OverlayInput in = Sample();
    vp::OverlayPrefs p;
    p.Camera = false;
    p.Stats = false;
    std::vector<std::string> l = vp::OverlayLines(p, in);
    CHECK_TRUE(Has(l, "Perspective"));
    CHECK_TRUE(Has(l, "Red Cube"));
    CHECK_TRUE(!Has(l, "FOV"));
    CHECK_TRUE(!Has(l, "FPS"));
    // По одной части.
    p.View = false;
    CHECK_TRUE(!Has(vp::OverlayLines(p, in), "Perspective"));
    p.Selection = false;
    p.Camera = true;
    l = vp::OverlayLines(p, in);
    CHECK_TRUE(Has(l, "1.50") && Has(l, "2.25") && Has(l, "-3.00") && Has(l, "FOV 60"));
    CHECK_TRUE(!Has(l, "Red Cube"));
    p.Camera = false;
    p.Stats = true;
    l = vp::OverlayLines(p, in);
    CHECK_TRUE(Has(l, "FPS 60") && Has(l, "89 442") && Has(l, "12") && Has(l, "0.41"));
    // Главный выключатель — пусто, что бы ни было включено.
    p.View = p.Selection = p.Camera = p.Stats = true;
    p.Enabled = false;
    CHECK_TRUE(vp::OverlayLines(p, in).empty());
}

TEST(ViewportToolbar_overlay_follows_camera_selection_and_views) {
    vp::OverlayPrefs p;
    p.Camera = true;
    vp::OverlayInput in = Sample();
    // Ортогональный вид — масштаб, а не положение и угол обзора.
    in.Perspective = false;
    in.ViewName = "Top";
    in.OrthoHeight = 12.5f;
    std::vector<std::string> l = vp::OverlayLines(p, in);
    CHECK_TRUE(Has(l, "12.50"));
    CHECK_TRUE(!Has(l, "FOV"));
    // Выделение меняется — строка меняется.
    in.SelectedCount = 0;
    CHECK_TRUE(Has(vp::OverlayLines(p, in), "Nothing selected"));
    in.SelectedCount = 3;
    in.SelectedName = "Cone";
    l = vp::OverlayLines(p, in);
    CHECK_TRUE(Has(l, "3") && Has(l, "Cone"));
    // Камера сдвинулась — числа другие.
    in.Perspective = true;
    in.CameraPos = {7.0f, 0.0f, 0.0f};
    CHECK_TRUE(Has(vp::OverlayLines(p, in), "7.00"));
    // Четыре вида: у каждого — его вид и номер; выделение и камера — только у
    // активного.
    in.SlotCount = 4;
    in.Slot = 2;
    in.Active = false;
    l = vp::OverlayLines(p, in);
    CHECK_EQ((int)l.size(), 1);
    CHECK_TRUE(Has(l, "3") && Has(l, "4"));
}

TEST(ViewportToolbar_overlay_avoids_other_elements) {
    const vp::Box cell{0, 0, 800, 500};
    // Свободно — левый нижний угол.
    vp::Box b = vp::PlaceBottomLeft(cell, 200, 50, 8, {});
    CHECK_NEAR(b.X0, 8.0f, 1e-4f);
    CHECK_NEAR(b.Y1, 492.0f, 1e-4f);
    // Превью камеры в левом нижнем углу — слой над ним, без пересечения.
    const vp::Box preview{8, 300, 330, 492};
    b = vp::PlaceBottomLeft(cell, 200, 50, 8, {preview});
    CHECK_TRUE(!b.Empty());
    CHECK_TRUE(!b.Overlaps(preview));
    CHECK_TRUE(b.Y1 <= preview.Y0);
    // Мест нет — слой не рисуется, а не ложится поверх.
    const vp::Box tall{0, 0, 800, 492};
    CHECK_TRUE(vp::PlaceBottomLeft(cell, 200, 50, 8, {tall}).Empty());
    // Шире вида — тоже не рисуется.
    CHECK_TRUE(vp::PlaceBottomLeft({0, 0, 120, 500}, 200, 50, 8, {}).Empty());
}

TEST(ViewportToolbar_collider_edit_only_for_shapes_with_handles) {
    entt::registry reg;
    const entt::entity none = reg.create();
    CHECK_TRUE(!vp::ColliderEditAvailable(reg, none));   // коллайдера нет
    CHECK_TRUE(!vp::ColliderEditAvailable(reg, entt::null));
    const entt::entity box = reg.create();
    reg.emplace<ColliderComponent>(box);
    CHECK_TRUE(vp::ColliderEditAvailable(reg, box));
    for (auto shape : {sage::physics::ShapeType::Sphere, sage::physics::ShapeType::Capsule}) {
        const entt::entity e = reg.create();
        reg.emplace<ColliderComponent>(e).Shape = shape;
        CHECK_TRUE(vp::ColliderEditAvailable(reg, e));
    }
    // Форма по мешу и составная — без ручек.
    const entt::entity mesh = reg.create();
    reg.emplace<ColliderComponent>(mesh).Shape = sage::physics::ShapeType::Mesh;
    CHECK_TRUE(!vp::ColliderEditAvailable(reg, mesh));
    const entt::entity parts = reg.create();
    reg.emplace<ColliderComponent>(parts).Parts.emplace_back();
    CHECK_TRUE(!vp::ColliderEditAvailable(reg, parts));
}

TEST(ViewportToolbar_snap_label_keeps_per_axis_steps) {
    CHECK_EQ(vp::SnapLabel({1, 1, 1}, 15.0f, 0.1f), std::string("1 · 15° · 0.1"));
    // Шаг по осям не сливается в одно число.
    CHECK_EQ(vp::SnapLabel({2, 1, 0.25f}, 45.0f, 0.5f), std::string("2/1/0.25 · 45° · 0.5"));
}

TEST(ViewportToolbar_overlay_prefs_survive_a_restart) {
    vp::OverlayPrefs saved;
    saved.Load();
    const vp::OverlayPrefs before = saved;
    vp::OverlayPrefs p;
    p.Enabled = false;
    p.View = false;
    p.Selection = true;
    p.Camera = true;
    p.Stats = true;
    p.Save();
    vp::OverlayPrefs back;
    back.Load();
    CHECK_TRUE(!back.Enabled && !back.View && back.Selection && back.Camera && back.Stats);
    before.Save();   // настройки человека — как были
}
