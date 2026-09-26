#include "EditorTools.h"
#include <imgui.h>

#include <ImGuizmo.h>

float EditorTools::SnapStepForCurrentOp() const {
    switch ((ImGuizmo::OPERATION)GizmoOp) {
        case ImGuizmo::ROTATE: return SnapRotate;
        case ImGuizmo::SCALE:  return SnapScale;
        default:               return SnapMove;
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
