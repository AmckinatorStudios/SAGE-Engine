// ===========================================================================
//  ПРОЦЕДУРНЫЕ ПРИМИТИВЫ (engine/src/sage/render/MeshData.cpp, PrimitiveParams)
//  — headless, без GL.
//
//  Для каждой формы и разной детализации: число вершин и индексов, обход CCW
//  снаружи и ни одного вырожденного треугольника, единичные нормали, UV в
//  [0, 1], касательные перпендикулярны нормалям, границы формы не зависят от
//  детализации (размер — Transform → Scale). Отдельно: плоские/гладкие нормали,
//  высота капсулы, умолчания и ключи вариантов.
// ===========================================================================
#include "TestFramework.h"

#include <cmath>

#include <glm/glm.hpp>

#include "sage/render/MeshData.h"
#include "sage/render/PrimitiveParams.h"

using namespace sage::render;

namespace {

struct Report {
    int Outward = 0, Inward = 0, Degenerate = 0;
    bool UnitNormals = true, UvInRange = true, TangentsOk = true;
    glm::vec3 Min{1e9f}, Max{-1e9f};
};

Report Inspect(const MeshData& d) {
    Report r;
    for (const Vertex& v : d.Vertices) {
        r.Min = glm::min(r.Min, v.Position);
        r.Max = glm::max(r.Max, v.Position);
        if (std::abs(glm::length(v.Normal) - 1.0f) > 1e-3f) r.UnitNormals = false;
        if (v.TexCoords.x < -1e-5f || v.TexCoords.x > 1.0f + 1e-5f || v.TexCoords.y < -1e-5f ||
            v.TexCoords.y > 1.0f + 1e-5f)
            r.UvInRange = false;
        const glm::vec3 t(v.Tangent);
        if (std::abs(glm::length(t) - 1.0f) > 1e-3f || std::abs(glm::dot(t, v.Normal)) > 1e-3f) r.TangentsOk = false;
    }
    for (size_t i = 0; i + 2 < d.Indices.size(); i += 3) {
        const Vertex& a = d.Vertices[d.Indices[i]];
        const Vertex& b = d.Vertices[d.Indices[i + 1]];
        const Vertex& c = d.Vertices[d.Indices[i + 2]];
        const glm::vec3 g = glm::cross(b.Position - a.Position, c.Position - a.Position);
        if (glm::length(g) < 1e-9f) {
            ++r.Degenerate;
            continue;
        }
        (glm::dot(g, a.Normal + b.Normal + c.Normal) > 0.0f ? r.Outward : r.Inward)++;
    }
    return r;
}

// Общие свойства любой правильной сетки.
void CheckClean(const MeshData& d) {
    CHECK_TRUE(!d.Empty());
    CHECK_EQ((int)(d.Indices.size() % 3), 0);
    bool inRange = true;
    for (unsigned int i : d.Indices) inRange &= i < d.Vertices.size();
    CHECK_TRUE(inRange);
    const Report r = Inspect(d);
    CHECK_EQ(r.Inward, 0);
    CHECK_EQ(r.Degenerate, 0);
    CHECK_TRUE(r.Outward > 0);
    CHECK_TRUE(r.UnitNormals);
    CHECK_TRUE(r.UvInRange);
    CHECK_TRUE(r.TangentsOk);
}

void CheckBounds(const MeshData& d, glm::vec3 lo, glm::vec3 hi) {
    const Report r = Inspect(d);
    CHECK_NEAR(r.Min.x, lo.x, 1e-4f);
    CHECK_NEAR(r.Min.y, lo.y, 1e-4f);
    CHECK_NEAR(r.Min.z, lo.z, 1e-4f);
    CHECK_NEAR(r.Max.x, hi.x, 1e-4f);
    CHECK_NEAR(r.Max.y, hi.y, 1e-4f);
    CHECK_NEAR(r.Max.z, hi.z, 1e-4f);
}

// Тела вращения: по высоте — ровно [lo, hi], по радиусу — не дальше 0.5
// (крайние X/Z зависят от числа секторов).
void CheckRevolved(const MeshData& d, float lo, float hi) {
    const Report r = Inspect(d);
    CHECK_NEAR(r.Min.y, lo, 1e-4f);
    CHECK_NEAR(r.Max.y, hi, 1e-4f);
    float radius = 0.0f;
    for (const Vertex& v : d.Vertices) radius = std::max(radius, glm::length(glm::vec2(v.Position.x, v.Position.z)));
    CHECK_NEAR(radius, 0.5f, 1e-4f);
}

int Triangles(const MeshData& d) { return (int)d.Indices.size() / 3; }

} // namespace

TEST(Primitives_cube_subdivides_each_face) {
    const MeshData one = BuildCube();
    CHECK_EQ((int)one.Vertices.size(), 24);
    CHECK_EQ((int)one.Indices.size(), 36);
    CheckClean(one);
    CheckBounds(one, glm::vec3(-0.5f), glm::vec3(0.5f));

    const MeshData sub = BuildCube(2, 3, 4);
    // Грани: ±Z — 2×3, ±X — 4×3, ±Y — 2×4 клеток, по два треугольника.
    CHECK_EQ(Triangles(sub), 2 * 2 * (2 * 3 + 4 * 3 + 2 * 4));
    CHECK_EQ((int)sub.Vertices.size(), 2 * (3 * 4 + 5 * 4 + 3 * 5));
    CheckClean(sub);
    CheckBounds(sub, glm::vec3(-0.5f), glm::vec3(0.5f));
    // Грани плоские: нормали — только шесть осей.
    bool axisNormals = true;
    for (const Vertex& v : sub.Vertices)
        axisNormals &= std::abs(std::abs(v.Normal.x) + std::abs(v.Normal.y) + std::abs(v.Normal.z) - 1.0f) < 1e-5f;
    CHECK_TRUE(axisNormals);
}

TEST(Primitives_plane_subdivides_x_and_z_independently) {
    const MeshData p = BuildPlane(3, 5);
    CHECK_EQ((int)p.Vertices.size(), 4 * 6);
    CHECK_EQ(Triangles(p), 3 * 5 * 2);
    CheckClean(p);
    CheckBounds(p, glm::vec3(-0.5f, 0.0f, -0.5f), glm::vec3(0.5f, 0.0f, 0.5f));
    bool up = true;
    for (const Vertex& v : p.Vertices) up &= v.Normal == glm::vec3(0, 1, 0);
    CHECK_TRUE(up);
    // Старый вызов с одним числом — квадратная сетка.
    CHECK_EQ(Triangles(BuildPlane(4)), 4 * 4 * 2);
}

TEST(Primitives_sphere_rings_segments_and_shading) {
    for (int rings : {2, 8, 24}) {
        for (int seg : {3, 12, 32}) {
            const MeshData s = BuildSphere(rings, seg);
            // У полюсов — по одному треугольнику на сектор.
            CHECK_EQ(Triangles(s), seg * (2 * rings - 2));
            CheckClean(s);
            CheckRevolved(s, -0.5f, 0.5f);
        }
    }
    // Гладкая: нормаль — направление от центра.
    const MeshData smooth = BuildSphere(12, 16, true);
    bool radial = true;
    for (const Vertex& v : smooth.Vertices) radial &= glm::length(v.Normal - glm::normalize(v.Position)) < 1e-4f;
    CHECK_TRUE(radial);
    // Плоская: то же число треугольников, нормаль вершины = нормаль её грани.
    const MeshData flat = BuildSphere(12, 16, false);
    CHECK_EQ(Triangles(flat), Triangles(smooth));
    CheckClean(flat);
    bool faceted = true;
    for (size_t i = 0; i + 2 < flat.Indices.size(); i += 3) {
        const Vertex& a = flat.Vertices[flat.Indices[i]];
        const Vertex& b = flat.Vertices[flat.Indices[i + 1]];
        const Vertex& c = flat.Vertices[flat.Indices[i + 2]];
        const glm::vec3 g = glm::normalize(glm::cross(b.Position - a.Position, c.Position - a.Position));
        faceted &= glm::length(a.Normal - g) < 1e-4f && a.Normal == b.Normal && b.Normal == c.Normal;
    }
    CHECK_TRUE(faceted);
}

TEST(Primitives_cylinder_and_cone_keep_flat_caps) {
    for (const bool smooth : {true, false}) {
        const MeshData cyl = BuildCylinder(10, 3, smooth);
        CHECK_EQ(Triangles(cyl), 10 * 3 * 2 + 10 * 2);   // стенка + две крышки
        CheckClean(cyl);
        CheckRevolved(cyl, -0.5f, 0.5f);
        const MeshData cone = BuildCone(10, 3, smooth);
        CHECK_EQ(Triangles(cone), 10 * (3 * 2 - 1) + 10);   // у вершины — по треугольнику на сектор
        CheckClean(cone);
        CheckRevolved(cone, -0.5f, 0.5f);
        // Торцы плоские при любом сглаживании: у вершин на высоте торца,
        // смотрящих вдоль оси, нормаль строго ±Y.
        int capNormals = 0;
        for (const Vertex& v : cyl.Vertices)
            if (std::abs(v.Normal.y) > 0.999f) ++capNormals;
        CHECK_EQ(capNormals, 2 * (10 + 2));
    }
    // Гладкая стенка цилиндра — радиальные нормали без наклона.
    bool radial = true;
    for (const Vertex& v : BuildCylinder(12, 2, true).Vertices)
        if (std::abs(v.Normal.y) < 0.5f) radial &= std::abs(v.Normal.y) < 1e-5f;
    CHECK_TRUE(radial);
}

TEST(Primitives_capsule_height_stretches_only_the_body) {
    for (const float h : {1.0f, 2.0f, 3.5f, 8.0f}) {
        const MeshData c = BuildCapsule(6, 12, h, 3);
        CheckClean(c);
        // Ширина — всегда диаметр 1 (её задаёт Scale), высота — заданная.
        CheckBounds(c, glm::vec3(-0.5f, -h * 0.5f, -0.5f), glm::vec3(0.5f, h * 0.5f, 0.5f));
        // Полусферы — круглые: каждая вершина шапки на расстоянии 0.5 от
        // центра своей полусферы.
        const float half = std::max(0.0f, h * 0.5f - 0.5f);
        bool round = true;
        for (const Vertex& v : c.Vertices) {
            if (v.Position.y > half + 1e-4f)
                round &= std::abs(glm::length(v.Position - glm::vec3(0, half, 0)) - 0.5f) < 1e-4f;
            if (v.Position.y < -half - 1e-4f)
                round &= std::abs(glm::length(v.Position - glm::vec3(0, -half, 0)) - 0.5f) < 1e-4f;
        }
        CHECK_TRUE(round);
    }
    // Ниже диаметра не бывает: полусферы не пересекаются.
    CheckBounds(BuildCapsule(6, 12, 0.3f), glm::vec3(-0.5f), glm::vec3(0.5f));
    // По умолчанию — прежняя капсула коллайдера: диаметр 1, высота 2.
    CheckBounds(BuildCapsule(), glm::vec3(-0.5f, -1.0f, -0.5f), glm::vec3(0.5f, 1.0f, 0.5f));
    // Пояса тела добавляют треугольники только стенке.
    CHECK_EQ(Triangles(BuildCapsule(6, 12, 3.0f, 4)) - Triangles(BuildCapsule(6, 12, 3.0f, 1)), 3 * 12 * 2);
    // Плоская капсула — тот же счёт треугольников.
    CHECK_EQ(Triangles(BuildCapsule(6, 12, 3.0f, 2, false)), Triangles(BuildCapsule(6, 12, 3.0f, 2, true)));
}

TEST(Primitives_params_defaults_keys_and_limits) {
    // Нули — умолчания формы, и это та же геометрия, что у примитива без настроек.
    const PrimitiveParams none;
    CHECK_TRUE(none.SameAsDefault(PrimitiveShape::Sphere));
    const MeshData a = BuildPrimitive(PrimitiveShape::Sphere, none);
    const MeshData b = BuildSphere();
    CHECK_EQ(a.Vertices.size(), b.Vertices.size());
    CHECK_EQ(a.Indices.size(), b.Indices.size());
    CHECK_EQ(DefaultPrimitiveParams(PrimitiveShape::Sphere).Segments, 32);
    CHECK_EQ(DefaultPrimitiveParams(PrimitiveShape::Sphere).Rings, 24);
    CHECK_EQ(DefaultPrimitiveParams(PrimitiveShape::Capsule).Segments, 24);
    CHECK_EQ(DefaultPrimitiveParams(PrimitiveShape::Capsule).Rings, 12);
    CHECK_NEAR(DefaultPrimitiveParams(PrimitiveShape::Capsule).Height, 2.0f, 1e-6f);

    // Явно заданные умолчания — тот же вариант (тот же ключ кэша).
    PrimitiveParams explicitDefault;
    explicitDefault.Segments = 32;
    explicitDefault.Rings = 24;
    CHECK_TRUE(explicitDefault.SameAsDefault(PrimitiveShape::Sphere));
    CHECK_TRUE(explicitDefault.Key(PrimitiveShape::Sphere) == none.Key(PrimitiveShape::Sphere));

    // Поля чужой формы на вариант не влияют: высота у сферы не читается.
    PrimitiveParams stray;
    stray.Height = 7.0f;
    stray.SubX = 9;
    CHECK_TRUE(stray.SameAsDefault(PrimitiveShape::Sphere));

    // Другая детализация — другой вариант.
    PrimitiveParams dense;
    dense.Segments = 64;
    CHECK_TRUE(!dense.SameAsDefault(PrimitiveShape::Sphere));
    CHECK_TRUE(dense.Key(PrimitiveShape::Sphere) != none.Key(PrimitiveShape::Sphere));

    // Пределы: слишком мало и слишком много зажимаются.
    PrimitiveParams wild;
    wild.Segments = 100000;
    wild.Rings = 1;
    wild.Height = 0.1f;
    const PrimitiveParams r = wild.Resolved(PrimitiveShape::Capsule);
    CHECK_EQ(r.Segments, PrimitiveLimits::kSegmentsMax);
    CHECK_EQ(r.Rings, 1);
    CHECK_NEAR(r.Height, PrimitiveLimits::kCapsuleHeightMin, 1e-6f);
    CHECK_EQ(wild.Resolved(PrimitiveShape::Sphere).Rings, PrimitiveLimits::kRingsMin);
    CheckClean(BuildPrimitive(PrimitiveShape::Capsule, wild));
}
