#include "EditorTools.h"
#include <imgui.h>

#include <ImGuizmo.h>

float EditorTools::SnapStepForCurrentOp() const {
    switch ((ImGuizmo::OPERATION)GizmoOp) {
        case ImGuizmo::ROTATE: return SnapRotate;
        case ImGuizmo::SCALE:  return SnapScale;
        default:               return SnapMove.x;
    }
}

#include <algorithm>

#include "EditorPrefs.h"

namespace {
constexpr const char* kColliderMode = "gizmo.collider.mode";
constexpr const char* kColliderOpacity = "gizmo.collider.opacity";
constexpr const char* kColliderSelected = "gizmo.collider.onlySelected";
}

void ColliderGizmoStyle::Load() {
    namespace prefs = sage::editor::prefs;
    // Число из файла не доверяется: испорченный файл не должен давать режим,
    // которого нет в списке (и пустой вьюпорт без единой формы).
    const int mode = prefs::GetInt(kColliderMode, (int)Mode::Lines);
    Draw = (mode >= 0 && mode <= 2) ? (Mode)mode : Mode::Lines;
    FillOpacity = std::clamp(prefs::GetFloat(kColliderOpacity, 0.28f), 0.05f, 0.9f);
    OnlySelected = prefs::GetBool(kColliderSelected, false);
}

void ColliderGizmoStyle::Save() const {
    namespace prefs = sage::editor::prefs;
    prefs::SetInt(kColliderMode, (int)Draw);
    prefs::SetFloat(kColliderOpacity, FillOpacity);
    prefs::SetBool(kColliderSelected, OnlySelected);
}

namespace {
constexpr const char* kSnapOn = "gizmo.snap.on";
constexpr const char* kSnapMove[3] = {"gizmo.snap.moveX", "gizmo.snap.moveY", "gizmo.snap.moveZ"};
constexpr const char* kSnapRotate = "gizmo.snap.rotate";
constexpr const char* kSnapScale = "gizmo.snap.scale";
// Шаг из файла — в разумных пределах: ноль или минус означал бы «привязка
// включена, но ничего не двигается».
float Step(float v, float def, float lo, float hi) { return (v >= lo && v <= hi) ? v : def; }
} // namespace

void EditorTools::LoadSnap() {
    namespace prefs = sage::editor::prefs;
    Snap = prefs::GetBool(kSnapOn, false);
    for (int k = 0; k < 3; ++k) SnapMove[k] = Step(prefs::GetFloat(kSnapMove[k], 1.0f), 1.0f, 0.001f, 1000.0f);
    SnapRotate = Step(prefs::GetFloat(kSnapRotate, 15.0f), 15.0f, 0.1f, 180.0f);
    SnapScale = Step(prefs::GetFloat(kSnapScale, 0.1f), 0.1f, 0.001f, 10.0f);
}

void EditorTools::SaveSnap() const {
    namespace prefs = sage::editor::prefs;
    prefs::SetBool(kSnapOn, Snap);
    for (int k = 0; k < 3; ++k) prefs::SetFloat(kSnapMove[k], SnapMove[k]);
    prefs::SetFloat(kSnapRotate, SnapRotate);
    prefs::SetFloat(kSnapScale, SnapScale);
}
