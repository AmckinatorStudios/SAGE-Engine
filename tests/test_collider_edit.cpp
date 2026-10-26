#include "TestFramework.h"

#include "gizmo/ColliderEdit.h"

// Ручки формы коллайдера (как в Unity): тянется грань — противоположная стоит,
// с Alt — симметрично; всё в локальных единицах коллайдера при любом масштабе.

using namespace sage::editor::collideredit;
using sage::physics::ShapeType;

TEST(ColliderEdit_box_face_moves_while_the_opposite_face_stays) {
    ColliderComponent box;   // 1×1×1 вокруг нуля
    const glm::vec3 scale(2.0f, 1.0f, 1.0f);
    // +X грань в мире на 1 м дальше: в осях объекта это 0.5 (масштаб 2).
    const ColliderComponent c = DragHandle(box, scale, {0, 1}, 1.0f, false);
    CHECK_NEAR(c.HalfExtents.x, 0.75f, 1e-5f);
    CHECK_NEAR(c.Center.x, 0.25f, 1e-5f);
    CHECK_NEAR(c.Center.x - c.HalfExtents.x, -0.5f, 1e-5f);   // −X грань на месте
    CHECK_NEAR(c.HalfExtents.y, 0.5f, 1e-6f);                  // другие оси не тронуты
    // Ручка стоит там, где грань: в мире +X грань — на (0.25 + 0.75) * 2 = 2.
    CHECK_NEAR(HandleLocal(c, scale, {0, 1}).x * scale.x, 2.0f, 1e-5f);
}

TEST(ColliderEdit_box_symmetric_drag_keeps_the_center) {
    ColliderComponent box;
    box.Center = {0.0f, 1.0f, 0.0f};
    const ColliderComponent c = DragHandle(box, glm::vec3(1.0f), {1, -1}, 0.5f, true);
    CHECK_NEAR(c.HalfExtents.y, 1.0f, 1e-5f);
    CHECK_NEAR(c.Center.y, 1.0f, 1e-6f);
}

TEST(ColliderEdit_box_cannot_turn_inside_out) {
    ColliderComponent box;
    const ColliderComponent c = DragHandle(box, glm::vec3(1.0f), {2, 1}, -5.0f, false);
    CHECK_TRUE(c.HalfExtents.z >= kMinSize);
    // Противоположная грань по-прежнему стоит.
    CHECK_NEAR(c.Center.z - c.HalfExtents.z, -0.5f, 1e-5f);
}

TEST(ColliderEdit_sphere_and_capsule_handles) {
    ColliderComponent sphere;
    sphere.Shape = ShapeType::Sphere;
    // Радиус шара домножается на наибольший масштаб (3): ход 1.5 м — это 0.5 радиуса.
    const ColliderComponent s = DragHandle(sphere, {1.0f, 3.0f, 1.0f}, {0, 1}, 1.5f, false);
    CHECK_NEAR(s.Radius, 1.0f, 1e-5f);
    CHECK_NEAR(s.Center.x, 0.0f, 1e-6f);   // шар растёт от центра

    ColliderComponent cap;
    cap.Shape = ShapeType::Capsule;
    const ColliderComponent h = DragHandle(cap, glm::vec3(1.0f), {1, 1}, 0.25f, false);
    CHECK_NEAR(h.HalfHeight, 0.75f, 1e-5f);
    CHECK_NEAR(h.Radius, 0.5f, 1e-6f);
    const ColliderComponent r = DragHandle(cap, glm::vec3(1.0f), {2, -1}, 0.1f, false);
    CHECK_NEAR(r.Radius, 0.6f, 1e-5f);
    // Верхняя ручка капсулы — на её макушке: высота цилиндра + радиус.
    CHECK_NEAR(HandleLocal(h, glm::vec3(1.0f), {1, 1}).y, 1.25f, 1e-5f);
}

TEST(ColliderEdit_center_moves_in_local_units_and_mesh_shapes_have_no_handles) {
    ColliderComponent box;
    const ColliderComponent c = MoveCenter(box, {2.0f, 2.0f, 4.0f}, {1.0f, 0.0f, 2.0f});
    CHECK_NEAR(c.Center.x, 0.5f, 1e-6f);
    CHECK_NEAR(c.Center.z, 0.5f, 1e-6f);
    ColliderComponent mesh;
    mesh.Shape = ShapeType::Mesh;
    CHECK_FALSE(Editable(mesh));
    CHECK_TRUE(Editable(box));
}

#include <glm/gtc/matrix_transform.hpp>

#include "imgui.h"
#include "gizmo/ColliderHandles.h"
#include "gizmo/LightGizmo.h"

namespace {
// Кадры ImGui без окна — как у тестов ColorPicker: нужна только логика мыши.
struct HandleHarness {
    ImGuiContext* Ctx = ImGui::CreateContext();
    sage::editor::handles::View View;
    HandleHarness() {
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(800, 600);
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        View.ViewM = glm::lookAt(glm::vec3(3.0f, 2.0f, 6.0f), glm::vec3(0.0f), glm::vec3(0, 1, 0));
        View.Proj = glm::perspective(glm::radians(60.0f), 800.0f / 600.0f, 0.1f, 100.0f);
        View.Min = ImVec2(0, 0);
        View.Size = ImVec2(800, 600);
        View.Eye = glm::vec3(3.0f, 2.0f, 6.0f);
    }
    ~HandleHarness() { ImGui::DestroyContext(Ctx); }
    template <class F> void Frame(F&& body) {
        ImGui::NewFrame();
        ImGui::Begin("HandlesTest");
        View.Draw = ImGui::GetWindowDrawList();
        body();
        ImGui::End();
        ImGui::Render();
    }
    ImVec2 Screen(const glm::vec3& w) {
        ImVec2 s;
        sage::editor::handles::ToScreen(View, w, s);
        return s;
    }
};
} // namespace

// Мышь тянет ручку +X грани: грань едет вслед за курсором, противоположная
// стоит, объект (его матрица) не трогается, жест — одно «начали» и одно «кончили».
TEST(ColliderHandles_mouse_drags_the_face_under_the_cursor) {
    HandleHarness h;
    ColliderComponent col;
    const glm::mat4 world = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 1.0f));
    sage::editor::ColliderHandles handles;
    int began = 0, ended = 0;
    auto step = [&](ImVec2 mouse, bool down) {
        ImGui::GetIO().AddMousePosEvent(mouse.x, mouse.y);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        h.Frame([&] {
            const auto r = handles.Update(col, world, h.View, true, glm::vec3(0.0f));
            began += r.Began;
            ended += r.Ended;
        });
    };
    const ImVec2 at = h.Screen({1.0f, 0.0f, 0.0f});        // +X грань в мире: 0.5 * 2
    const ImVec2 to = h.Screen({1.6f, 0.0f, 0.0f});        // на 0.6 м дальше по X
    step(at, false);                                        // навели
    step(at, true);                                         // нажали
    for (int k = 1; k <= 5; ++k)
        step(ImVec2(at.x + (to.x - at.x) * k / 5.0f, at.y + (to.y - at.y) * k / 5.0f), true);
    step(to, false);                                        // отпустили
    std::printf("       грань +X: половина %.3f, центр %.3f\n", col.HalfExtents.x, col.Center.x);
    CHECK_EQ(began, 1);
    CHECK_EQ(ended, 1);
    // Ход 0.6 м в мире = 0.3 в осях объекта (масштаб 2): половина +0.15, центр +0.15.
    CHECK_NEAR(col.HalfExtents.x, 0.65f, 0.01f);
    CHECK_NEAR(col.Center.x - col.HalfExtents.x, -0.5f, 1e-4f);
}

// Ручка дальности прожектора тянется вдоль его луча.
TEST(LightHandles_spot_range_follows_the_mouse) {
    HandleHarness h;
    LightComponent spot;
    spot.Kind = LightComponent::Type::Spot;
    spot.Range = 2.0f;
    // Прожектор светит вниз по −Z объекта; объект повёрнут так, что светит вдоль +X мира.
    const glm::mat4 world = glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(0, 1, 0));
    sage::editor::LightHandles handles;
    auto step = [&](ImVec2 mouse, bool down) {
        ImGui::GetIO().AddMousePosEvent(mouse.x, mouse.y);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        h.Frame([&] { handles.Update(spot, world, h.View, true); });
    };
    const ImVec2 at = h.Screen({2.0f, 0.0f, 0.0f}), to = h.Screen({3.0f, 0.0f, 0.0f});
    step(at, false);
    step(at, true);
    step(ImVec2((at.x + to.x) * 0.5f, (at.y + to.y) * 0.5f), true);
    step(to, true);
    step(to, false);
    CHECK_NEAR(spot.Range, 3.0f, 0.02f);
}

#include "EditorTools.h"

TEST(LightEdit_spot_angle_keeps_its_soft_edge_and_area_grows_both_ways) {
    LightComponent spot;
    spot.Kind = LightComponent::Type::Spot;
    spot.Range = 4.0f;
    spot.OuterConeDeg = 30.0f;
    spot.InnerConeDeg = 15.0f;
    LightComponent out = spot;
    // Радиус основания 4 на дальности 4 — внешний угол 45°.
    sage::editor::lightedit::SpotAngle(spot, 4.0f, out);
    CHECK_NEAR(out.OuterConeDeg, 45.0f, 0.01f);
    CHECK_NEAR(out.InnerConeDeg, 22.5f, 0.01f);   // та же доля, что была (1/2)
    sage::editor::lightedit::SpotAngle(spot, -1.0f, out);
    CHECK_TRUE(out.OuterConeDeg >= 1.0f);          // конус не выворачивается
    CHECK_TRUE(out.InnerConeDeg <= out.OuterConeDeg);
    // Ручка на краю панели: её ход — половина изменения размера.
    CHECK_NEAR(sage::editor::lightedit::AreaSide(2.0f, 0.5f), 3.0f, 1e-5f);
    CHECK_TRUE(sage::editor::lightedit::Range(1.0f, -5.0f) > 0.0f);
}

TEST(Snap_steps_survive_a_restart_and_ctrl_inverts_while_dragging) {
    EditorTools t;
    t.Snap = true;
    t.SnapMove = {2.0f, 1.0f, 0.25f};
    t.SnapRotate = 45.0f;
    t.SnapScale = 0.5f;
    t.SaveSnap();
    EditorTools again;
    again.LoadSnap();
    CHECK_TRUE(again.Snap);
    CHECK_NEAR(again.SnapMove.z, 0.25f, 1e-6f);
    CHECK_NEAR(again.SnapMove.x, 2.0f, 1e-6f);
    CHECK_NEAR(again.SnapRotate, 45.0f, 1e-6f);
    CHECK_NEAR(again.SnapScale, 0.5f, 1e-6f);
    // Ctrl — привязка наоборот на время жеста.
    CHECK_TRUE(again.SnapActive(false));
    CHECK_FALSE(again.SnapActive(true));
    again.Snap = false;
    CHECK_TRUE(again.SnapActive(true));
    // Вернуть как было, чтобы не оставить следа другим тестам.
    EditorTools def;
    def.SaveSnap();
}
