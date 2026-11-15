#include "sage/render/PrimitiveParams.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sage::render {

PrimitiveParams DefaultPrimitiveParams(PrimitiveShape shape) {
    // То, чем примитив был до настроек: геометрия старых сцен не меняется.
    PrimitiveParams p;
    switch (shape) {
        case PrimitiveShape::Cube: p.SubX = p.SubY = p.SubZ = 1; break;
        case PrimitiveShape::Plane: p.SubX = p.SubZ = 1; break;
        case PrimitiveShape::Sphere: p.Segments = 32; p.Rings = 24; break;
        case PrimitiveShape::Cylinder:
        case PrimitiveShape::Cone: p.Segments = 32; p.HeightSegments = 1; break;
        case PrimitiveShape::Capsule:
            p.Segments = 24;
            p.Rings = 12;
            p.HeightSegments = 1;
            p.Height = 2.0f;
            break;
    }
    return p;
}

PrimitiveParams PrimitiveParams::Resolved(PrimitiveShape shape) const {
    using L = PrimitiveLimits;
    const PrimitiveParams def = DefaultPrimitiveParams(shape);
    auto pick = [](int v, int d, int lo, int hi) { return std::clamp(v > 0 ? v : d, lo, hi); };
    // Поля, которых у формы нет, обнуляются: «тот же вариант» и ключ кэша не
    // зависят от того, что осталось в них от прошлой формы.
    PrimitiveParams r;
    switch (shape) {
        case PrimitiveShape::Cube:
            r.SubX = pick(SubX, def.SubX, 1, L::kSubdivMax);
            r.SubY = pick(SubY, def.SubY, 1, L::kSubdivMax);
            r.SubZ = pick(SubZ, def.SubZ, 1, L::kSubdivMax);
            break;
        case PrimitiveShape::Plane:
            r.SubX = pick(SubX, def.SubX, 1, L::kSubdivMax);
            r.SubZ = pick(SubZ, def.SubZ, 1, L::kSubdivMax);
            break;
        case PrimitiveShape::Sphere:
            r.Segments = pick(Segments, def.Segments, L::kSegmentsMin, L::kSegmentsMax);
            r.Rings = pick(Rings, def.Rings, L::kRingsMin, L::kRingsMax);
            r.Flat = Flat;
            break;
        case PrimitiveShape::Cylinder:
        case PrimitiveShape::Cone:
            r.Segments = pick(Segments, def.Segments, L::kSegmentsMin, L::kSegmentsMax);
            r.HeightSegments = pick(HeightSegments, def.HeightSegments, 1, L::kHeightSegMax);
            r.Flat = Flat;
            break;
        case PrimitiveShape::Capsule:
            r.Segments = pick(Segments, def.Segments, L::kSegmentsMin, L::kSegmentsMax);
            r.Rings = pick(Rings, def.Rings, 1, L::kRingsMax / 2);
            r.HeightSegments = pick(HeightSegments, def.HeightSegments, 1, L::kHeightSegMax);
            r.Height = std::clamp(Height > 0.0f && std::isfinite(Height) ? Height : def.Height, L::kCapsuleHeightMin,
                                  L::kCapsuleHeightMax);
            r.Flat = Flat;
            break;
    }
    return r;
}

bool PrimitiveParams::SameAsDefault(PrimitiveShape shape) const {
    return Resolved(shape) == DefaultPrimitiveParams(shape).Resolved(shape);
}

std::string PrimitiveParams::Key(PrimitiveShape shape) const {
    const PrimitiveParams r = Resolved(shape);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%d|%d,%d,%d|%d|%d|%d|%.4f|%d", (int)shape, r.SubX, r.SubY, r.SubZ, r.Segments,
                  r.Rings, r.HeightSegments, r.Height, r.Flat ? 1 : 0);
    return buf;
}

} // namespace sage::render
