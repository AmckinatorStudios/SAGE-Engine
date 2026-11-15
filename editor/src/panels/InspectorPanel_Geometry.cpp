// ---------------------------------------------------------------------------
// ИНСПЕКТОР: GEOMETRY — настройки геометрии процедурного примитива.
//
// Только детализация и сглаживание: размер объекта — Transform → Scale, и
// второго поля размера (ширины, радиуса, длины) здесь нет. Исключение —
// высота капсулы: Scale по Y сплющил бы и её круглые торцы, а Height удлиняет
// только цилиндрическую часть.
//
// Настройки — СВОИ у объекта (MeshRef::prim): правка одного шара строит ему
// свой меш (ResourceManager::GetPrimitive с настройками), не трогая общий меш
// формы и соседей. История правок и сброс — как у всех полей инспектора.
// ---------------------------------------------------------------------------
#include "panels/InspectorPanel.h"

#include "imgui.h"

#include "EditorHost.h"
#include "EditorTheme.h"
#include "InspectorControls.h"
#include "InspectorFields.h"
#include "Localization.h"

#include "sage/render/ResourceManager.h"
#include "sage/scene/Components.h"

namespace sage::editor {

namespace ictl = sage::editor::ictl;
namespace ifield = sage::editor::ifield;
using sage::render::PrimitiveLimits;
using sage::render::PrimitiveParams;
using sage::render::PrimitiveShape;

namespace {

// Подразделения граней — одним вектором kit: «Подразделения: [X|Y|Z]».
bool Subdivisions(EditorHost& host, int* v, int n, const char* const* tags) {
    float f[3] = {(float)v[0], (float)v[1], n > 2 ? (float)v[2] : 1.0f};
    ictl::NumOpts o;
    o.Min = 1.0f;
    o.Max = (float)PrimitiveLimits::kSubdivMax;
    o.Step = 1.0f;
    o.Format = "%.0f";
    o.HasReset = true;
    o.Reset = 1.0f;
    ictl::VecOpts vo;
    vo.Tags = tags;
    if (!ictl::VectorN(host, T("Subdivisions"), f, n, o, vo)) return false;
    for (int i = 0; i < n; ++i) v[i] = std::clamp((int)std::lround(f[i]), 1, PrimitiveLimits::kSubdivMax);
    return true;
}

} // namespace

void DrawPrimitiveGeometry(EditorHost& host, MeshRendererComponent& mr) {
    PrimitiveShape shape;
    if (!MeshRef::ShapeOf(mr.Ref.type, shape)) return;   // модель и «нет меша» — без раздела
    bool reset = false;
    if (!EditorTheme::SectionHeader("wire", T("Geometry" "###Geometry"), ImGuiTreeNodeFlags_DefaultOpen, nullptr,
                                    T("Detail and shading of the shape. Its size is Transform → Scale"), &reset))
        return;
    if (reset) {
        if (!mr.Ref.prim.IsDefault()) {
            host.PushUndoSnapshot();
            mr.Ref.prim = PrimitiveParams{};
            mr.MeshPtr = ResourceManager::Instance().GetPrimitive(mr.Ref.type, mr.Ref.prim);
        }
        return;
    }

    // Поля показывают ДЕЙСТВУЮЩИЕ значения (нули в настройках — умолчания
    // формы); правка записывает весь набор формы явно.
    PrimitiveParams p = mr.Ref.prim.Resolved(shape);
    bool changed = false;
    bool smooth = !p.Flat;
    static const char* const kXYZ[3] = {"X", "Y", "Z"};
    static const char* const kXZ[2] = {"X", "Z"};

    switch (shape) {
        case PrimitiveShape::Cube: {
            int sub[3] = {p.SubX, p.SubY, p.SubZ};
            if (Subdivisions(host, sub, 3, kXYZ)) {
                p.SubX = sub[0];
                p.SubY = sub[1];
                p.SubZ = sub[2];
                changed = true;
            }
            EditorTheme::Hint(T("Each face is split into a grid; faces stay flat and edges sharp"));
            break;
        }
        case PrimitiveShape::Plane: {
            int sub[2] = {p.SubX, p.SubZ};
            if (Subdivisions(host, sub, 2, kXZ)) {
                p.SubX = sub[0];
                p.SubZ = sub[1];
                changed = true;
            }
            break;
        }
        case PrimitiveShape::Sphere:
            changed |= ifield::Int(host, T("Segments"), &p.Segments, PrimitiveLimits::kSegmentsMin,
                                   PrimitiveLimits::kSegmentsMax);
            changed |= ifield::Int(host, T("Rings"), &p.Rings, PrimitiveLimits::kRingsMin, PrimitiveLimits::kRingsMax);
            changed |= ifield::Check(host, T("Smooth Shading"), &smooth);
            break;
        case PrimitiveShape::Cylinder:
        case PrimitiveShape::Cone:
            changed |= ifield::Int(host, T("Radial Segments"), &p.Segments, PrimitiveLimits::kSegmentsMin,
                                   PrimitiveLimits::kSegmentsMax);
            changed |= ifield::Int(host, T("Height Segments"), &p.HeightSegments, 1, PrimitiveLimits::kHeightSegMax);
            changed |= ifield::Check(host, T("Smooth Side"), &smooth);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", T("Smooths only the side: the caps stay flat"));
            break;
        case PrimitiveShape::Capsule:
            changed |= ifield::Float(host, T("Height"), &p.Height, 0.02f, PrimitiveLimits::kCapsuleHeightMin,
                                     PrimitiveLimits::kCapsuleHeightMax, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", T("Total height of the capsule: only the straight middle grows, the round "
                                          "ends keep their shape. Not lower than the diameter (1)"));
            changed |= ifield::Int(host, T("Radial Segments"), &p.Segments, PrimitiveLimits::kSegmentsMin,
                                   PrimitiveLimits::kSegmentsMax);
            changed |= ifield::Int(host, T("Hemisphere Rings"), &p.Rings, 1, PrimitiveLimits::kRingsMax / 2);
            changed |= ifield::Int(host, T("Body Segments"), &p.HeightSegments, 1, PrimitiveLimits::kHeightSegMax);
            changed |= ifield::Check(host, T("Smooth Shading"), &smooth);
            break;
    }
    if (!changed) return;
    // Снимок для отмены уже сделан полем; меш — свой у этого объекта (или
    // общий, если вернулись к умолчаниям). Transform, материал и цвет не
    // трогаются: меняется только MeshPtr.
    p.Flat = !smooth;
    mr.Ref.prim = p.Resolved(shape);
    mr.MeshPtr = ResourceManager::Instance().GetPrimitive(mr.Ref.type, mr.Ref.prim);
}

} // namespace sage::editor
