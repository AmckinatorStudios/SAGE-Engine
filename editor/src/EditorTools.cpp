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
