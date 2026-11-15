#include "sage/render/MeshData.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>

// CPU-генераторы примитивов. Перенесены из Mesh.cpp, чтобы геометрию можно
// было получить БЕЗ GL-контекста (бейкер GI, физика, юнит-тесты); Mesh::Create*
// — тонкие обёртки над этими функциями.
//
// ОБЩИЕ ПРАВИЛА ВСЕХ ФОРМ (их проверяет tests/test_primitives.cpp):
//   * обход треугольников — против часовой СНАРУЖИ (CCW, как ждёт отсечение
//     задних граней); раньше сфера, плоскость и конус шли по часовой, и их
//     исправлял Mesh (FixWinding) — генератор обязан выдавать правильное сам;
//   * ни одного вырожденного треугольника: у полюса сферы и капсулы, у
//     вершины конуса пояс сходится в точку — там один треугольник на сектор,
//     а не «квадрат» из двух, один из которых нулевой площади;
//   * нормали единичные; UV в [0, 1]; касательные посчитаны;
//   * размер не зависит от детализации (размер — Transform → Scale).
namespace sage::render {

namespace {

constexpr float kPi = glm::pi<float>();

// Плоские нормали: у каждого треугольника — свои три вершины с нормалью
// грани. Число треугольников то же — меняется только освещение.
MeshData Flatten(const MeshData& s) {
    MeshData d;
    d.Vertices.reserve(s.Indices.size());
    d.Indices.reserve(s.Indices.size());
    for (size_t i = 0; i + 2 < s.Indices.size(); i += 3) {
        Vertex a = s.Vertices[s.Indices[i]], b = s.Vertices[s.Indices[i + 1]], c = s.Vertices[s.Indices[i + 2]];
        const glm::vec3 g = glm::cross(b.Position - a.Position, c.Position - a.Position);
        const float len = glm::length(g);
        if (len < 1e-12f) continue;   // вырожденных у генераторов нет, но и плодить их нельзя
        a.Normal = b.Normal = c.Normal = g / len;
        const unsigned int o = (unsigned int)d.Vertices.size();
        d.Vertices.insert(d.Vertices.end(), {a, b, c});
        d.Indices.insert(d.Indices.end(), {o, o + 1, o + 2});
    }
    return d;
}

void Append(MeshData& dst, const MeshData& src) {
    const unsigned int o = (unsigned int)dst.Vertices.size();
    dst.Vertices.insert(dst.Vertices.end(), src.Vertices.begin(), src.Vertices.end());
    for (unsigned int i : src.Indices) dst.Indices.push_back(i + o);
}

MeshData Finish(MeshData d) {
    ComputeTangents(d.Vertices, d.Indices);
    return d;
}

// Сетка грани: origin + U*u + V*v, u и v — 0..1 по nu и nv клеткам.
// cross(U, V) обязан совпадать с n — тогда обход CCW снаружи.
void AddGrid(MeshData& d, glm::vec3 origin, glm::vec3 U, glm::vec3 V, int nu, int nv, glm::vec3 n, bool flipV) {
    const unsigned int base = (unsigned int)d.Vertices.size();
    for (int j = 0; j <= nv; ++j) {
        for (int i = 0; i <= nu; ++i) {
            const float u = (float)i / (float)nu, v = (float)j / (float)nv;
            d.Vertices.push_back({origin + U * u + V * v, n, {u, flipV ? 1.0f - v : v}});
        }
    }
    const unsigned int stride = (unsigned int)nu + 1;
    for (int j = 0; j < nv; ++j) {
        for (int i = 0; i < nu; ++i) {
            const unsigned int a = base + (unsigned int)j * stride + (unsigned int)i;
            const unsigned int b = a + 1, c = a + stride, e = c + 1;
            d.Indices.insert(d.Indices.end(), {a, b, e, a, e, c});
        }
    }
}

// Кольцо крышки (центр + веер): up — смотрит ли крышка вверх.
void AddCap(MeshData& d, float y, float radius, int sectors, bool up) {
    const glm::vec3 n(0.0f, up ? 1.0f : -1.0f, 0.0f);
    const unsigned int center = (unsigned int)d.Vertices.size();
    d.Vertices.push_back({{0.0f, y, 0.0f}, n, {0.5f, 0.5f}});
    const unsigned int first = (unsigned int)d.Vertices.size();
    for (int s = 0; s <= sectors; ++s) {
        const float theta = (float)s / (float)sectors * 2.0f * kPi;
        const float cx = std::cos(theta), cz = std::sin(theta);
        d.Vertices.push_back({{cx * radius, y, cz * radius}, n, {cx * 0.5f + 0.5f, cz * 0.5f + 0.5f}});
    }
    for (int s = 0; s < sectors; ++s) {
        const unsigned int a = first + (unsigned int)s, b = a + 1;
        if (up) d.Indices.insert(d.Indices.end(), {center, b, a});
        else d.Indices.insert(d.Indices.end(), {center, a, b});
    }
}

// Тело вращения по профилю: ряды сверху вниз, у каждого — высота, радиус
// кольца и вертикальная доля нормали (горизонтальная — радиус нормали).
// Полюс (радиус 0) — один треугольник на сектор, без вырожденного.
struct ProfileRow {
    float Y, Radius, NormalY, NormalR, V;
};
MeshData Revolve(const std::vector<ProfileRow>& rows, int sectors) {
    MeshData d;
    const unsigned int stride = (unsigned int)sectors + 1;
    for (const ProfileRow& r : rows) {
        const bool pole = r.Radius <= 1e-6f;
        for (int s = 0; s <= sectors; ++s) {
            // У полюса U — середина сектора: текстура сходится веером, а не
            // тянется в одну точку от края сектора.
            const float u = pole ? std::min(1.0f, ((float)s + 0.5f) / (float)sectors) : (float)s / (float)sectors;
            const float theta = (pole ? ((float)s + 0.5f) : (float)s) / (float)sectors * 2.0f * kPi;
            const float c = std::cos(theta), sn = std::sin(theta);
            const glm::vec3 n = glm::normalize(glm::vec3(c * r.NormalR, r.NormalY, sn * r.NormalR));
            d.Vertices.push_back({{c * r.Radius, r.Y, sn * r.Radius}, n, {u, r.V}});
        }
    }
    for (size_t i = 0; i + 1 < rows.size(); ++i) {
        const bool topPole = rows[i].Radius <= 1e-6f;
        const bool bottomPole = rows[i + 1].Radius <= 1e-6f;
        for (int s = 0; s < sectors; ++s) {
            // a — верхний ряд, b — нижний; (a, a+1, b) и (a+1, b+1, b) — CCW снаружи.
            const unsigned int a = (unsigned int)i * stride + (unsigned int)s;
            const unsigned int b = a + stride;
            if (!topPole) d.Indices.insert(d.Indices.end(), {a, a + 1, b});
            if (!bottomPole) d.Indices.insert(d.Indices.end(), {a + 1, b + 1, b});
        }
    }
    return d;
}

int Clampi(int v, int lo, int hi) { return std::clamp(v, lo, hi); }

} // namespace

void ComputeTangents(std::vector<Vertex>& v, const std::vector<unsigned int>& idx) {
    // Касательная из позиций и UV каждого треугольника; накапливаются и
    // касательная, и бинормаль, затем касательная ортогонализуется к нормали
    // (Грам — Шмидт), а знак w = sign(dot(cross(N,T), B)) пишется в Tangent.w:
    // шейдер строит бинормаль cross(N,T)*w, и грань с зеркальной развёрткой не
    // показывает вывернутый рельеф нормал-карты.
    std::vector<glm::vec3> tanAcc(v.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> bitAcc(v.size(), glm::vec3(0.0f));
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        const unsigned a = idx[i], b = idx[i + 1], c = idx[i + 2];
        const glm::vec3 e1 = v[b].Position - v[a].Position;
        const glm::vec3 e2 = v[c].Position - v[a].Position;
        const glm::vec2 d1 = v[b].TexCoords - v[a].TexCoords;
        const glm::vec2 d2 = v[c].TexCoords - v[a].TexCoords;
        const float det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-8f) continue;
        const float f = 1.0f / det;
        const glm::vec3 t = f * (d2.y * e1 - d1.y * e2);
        const glm::vec3 bt = f * (d1.x * e2 - d2.x * e1);
        tanAcc[a] += t; tanAcc[b] += t; tanAcc[c] += t;
        bitAcc[a] += bt; bitAcc[b] += bt; bitAcc[c] += bt;
    }
    for (size_t i = 0; i < v.size(); ++i) {
        const glm::vec3 n = v[i].Normal;
        glm::vec3 t = tanAcc[i] - n * glm::dot(n, tanAcc[i]);
        if (glm::dot(t, t) < 1e-10f) {
            // Запасная касательная, перпендикулярная нормали.
            t = std::abs(n.y) < 0.99f ? glm::cross(glm::vec3(0, 1, 0), n) : glm::vec3(1, 0, 0);
        }
        t = glm::normalize(t);
        const float w = (glm::dot(glm::cross(n, t), bitAcc[i]) < 0.0f) ? -1.0f : 1.0f;
        v[i].Tangent = glm::vec4(t, w);
    }
}

MeshData BuildCube(int subX, int subY, int subZ) {
    const int sx = Clampi(subX, 1, PrimitiveLimits::kSubdivMax);
    const int sy = Clampi(subY, 1, PrimitiveLimits::kSubdivMax);
    const int sz = Clampi(subZ, 1, PrimitiveLimits::kSubdivMax);
    const glm::vec3 X(1, 0, 0), Y(0, 1, 0), Z(0, 0, 1);
    MeshData d;
    // Грани — отдельными сетками: рёбра куба острые (у соседних граней свои
    // вершины и свои нормали). U каждой грани — по горизонтали, V — вверх (у
    // верхней и нижней — вдоль Z), как было у куба без подразделения.
    AddGrid(d, {-0.5f, -0.5f, 0.5f}, X, Y, sx, sy, {0, 0, 1}, false);    // +Z
    AddGrid(d, {0.5f, -0.5f, -0.5f}, -X, Y, sx, sy, {0, 0, -1}, false);  // -Z
    AddGrid(d, {0.5f, -0.5f, 0.5f}, -Z, Y, sz, sy, {1, 0, 0}, false);    // +X
    AddGrid(d, {-0.5f, -0.5f, -0.5f}, Z, Y, sz, sy, {-1, 0, 0}, false);  // -X
    AddGrid(d, {-0.5f, 0.5f, 0.5f}, X, -Z, sx, sz, {0, 1, 0}, true);     // +Y
    AddGrid(d, {-0.5f, -0.5f, -0.5f}, X, Z, sx, sz, {0, -1, 0}, true);   // -Y
    return Finish(std::move(d));
}

MeshData BuildSphere(int rings, int sectors, bool smooth) {
    rings = Clampi(rings, PrimitiveLimits::kRingsMin, PrimitiveLimits::kRingsMax);
    sectors = Clampi(sectors, PrimitiveLimits::kSegmentsMin, PrimitiveLimits::kSegmentsMax);
    const float radius = PrimitiveLimits::kCapsuleRadius;
    std::vector<ProfileRow> rows;
    for (int r = 0; r <= rings; ++r) {
        const float v = (float)r / (float)rings;   // 0..1 сверху вниз
        const float phi = v * kPi;
        const float y = std::cos(phi), ringR = (r == 0 || r == rings) ? 0.0f : std::sin(phi);
        rows.push_back({y * radius, ringR * radius, y, ringR, 1.0f - v});
    }
    MeshData d = Revolve(rows, sectors);
    return Finish(smooth ? std::move(d) : Flatten(d));
}

MeshData BuildCapsule(int rings, int sectors, float height, int bodySegments, bool smooth) {
    // Кольца КАЖДОЙ полусферы — от одного (полюс сразу к экватору).
    rings = Clampi(rings, 1, PrimitiveLimits::kRingsMax / 2);
    sectors = Clampi(sectors, PrimitiveLimits::kSegmentsMin, PrimitiveLimits::kSegmentsMax);
    bodySegments = Clampi(bodySegments, 1, PrimitiveLimits::kHeightSegMax);
    // РАЗМЕР — ТОТ ЖЕ, ЧТО У КОЛЛАЙДЕРА: радиус 0.5, общая высота 2 по
    // умолчанию (полувысота цилиндрической части 0.5 — ровно ColliderComponent
    // по умолчанию). Высота меняет только длину цилиндрической части:
    // полусферы остаются полусферами радиуса 0.5. Ниже диаметра капсула не
    // бывает — там полусферы пересеклись бы; ровно на диаметре она — сфера.
    const float radius = PrimitiveLimits::kCapsuleRadius;
    height = std::clamp(std::isfinite(height) ? height : 2.0f, PrimitiveLimits::kCapsuleHeightMin,
                        PrimitiveLimits::kCapsuleHeightMax);
    const float half = std::max(0.0f, height * 0.5f - radius);
    const bool body = half > 1e-4f;

    // Профиль сверху вниз: верхняя полусфера, пояса тела, нижняя полусфера.
    // Нормаль — от центра СВОЕЙ полусферы: у стенки она горизонтальна.
    std::vector<ProfileRow> rows;
    for (int k = 0; k <= rings; ++k) {
        const float phi = (float)k / (float)rings * kPi * 0.5f;
        const float s = (k == 0) ? 0.0f : std::sin(phi);
        rows.push_back({half + std::cos(phi) * radius, s * radius, std::cos(phi), s, 0.0f});
    }
    if (body)
        for (int k = 1; k < bodySegments; ++k)
            rows.push_back({half - 2.0f * half * (float)k / (float)bodySegments, radius, 0.0f, 1.0f, 0.0f});
    // Нижний экватор — отдельный ряд, только если между экваторами есть
    // стенка: иначе два ряда совпали бы и дали пояс нулевой высоты.
    for (int k = body ? 0 : 1; k <= rings; ++k) {
        const float phi = kPi * 0.5f + (float)k / (float)rings * kPi * 0.5f;
        const float s = (k == rings) ? 0.0f : std::sin(phi);
        rows.push_back({-half + std::cos(phi) * radius, s * radius, std::cos(phi), s, 0.0f});
    }
    // V — по длине профиля: текстура не сжимается на шапках и не
    // растягивается на длинной стенке.
    float total = 0.0f;
    std::vector<float> along(rows.size(), 0.0f);
    for (size_t i = 1; i < rows.size(); ++i) {
        total += glm::length(glm::vec2(rows[i].Radius - rows[i - 1].Radius, rows[i].Y - rows[i - 1].Y));
        along[i] = total;
    }
    for (size_t i = 0; i < rows.size(); ++i) rows[i].V = 1.0f - (total > 0.0f ? along[i] / total : 0.0f);
    MeshData d = Revolve(rows, sectors);
    return Finish(smooth ? std::move(d) : Flatten(d));
}

MeshData BuildPlane(int subX, int subZ) {
    const int sx = Clampi(subX, 1, PrimitiveLimits::kSubdivMax);
    const int sz = Clampi(subZ < 0 ? subX : subZ, 1, PrimitiveLimits::kSubdivMax);
    MeshData d;
    for (int z = 0; z <= sz; ++z) {
        for (int x = 0; x <= sx; ++x) {
            const float u = (float)x / (float)sx, v = (float)z / (float)sz;
            d.Vertices.push_back({{u - 0.5f, 0.0f, v - 0.5f}, {0, 1, 0}, {u, v}});
        }
    }
    const unsigned int stride = (unsigned int)sx + 1;
    for (int z = 0; z < sz; ++z) {
        for (int x = 0; x < sx; ++x) {
            const unsigned int a = (unsigned int)z * stride + (unsigned int)x;
            const unsigned int b = a + stride;
            d.Indices.insert(d.Indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
    return Finish(std::move(d));
}

MeshData BuildCylinder(int sectors, int heightSegments, bool smoothSide) {
    sectors = Clampi(sectors, PrimitiveLimits::kSegmentsMin, PrimitiveLimits::kSegmentsMax);
    heightSegments = Clampi(heightSegments, 1, PrimitiveLimits::kHeightSegMax);
    const float radius = 0.5f, halfH = 0.5f;
    // Боковая поверхность сверху вниз; нормали радиальные. Торцы — плоские
    // всегда: сглаживание касается только стенки.
    std::vector<ProfileRow> rows;
    for (int k = 0; k <= heightSegments; ++k) {
        const float t = (float)k / (float)heightSegments;
        rows.push_back({halfH - 2.0f * halfH * t, radius, 0.0f, 1.0f, 1.0f - t});
    }
    MeshData side = Revolve(rows, sectors);
    MeshData d = smoothSide ? std::move(side) : Flatten(side);
    AddCap(d, halfH, radius, sectors, true);
    AddCap(d, -halfH, radius, sectors, false);
    return Finish(std::move(d));
}

MeshData BuildCone(int sectors, int heightSegments, bool smoothSide) {
    sectors = Clampi(sectors, PrimitiveLimits::kSegmentsMin, PrimitiveLimits::kSegmentsMax);
    heightSegments = Clampi(heightSegments, 1, PrimitiveLimits::kHeightSegMax);
    const float radius = 0.5f, halfH = 0.5f;
    // Наклон нормали стенки: normalize(cos, r/h, sin). У вершины — ряд из
    // точки (по вершине на сектор, с нормалью середины сектора): один
    // треугольник на сектор, без вырожденного.
    const float slope = radius / (2.0f * halfH);
    std::vector<ProfileRow> rows;
    for (int k = 0; k <= heightSegments; ++k) {
        const float t = 1.0f - (float)k / (float)heightSegments;   // 1 у вершины, 0 у основания
        rows.push_back({-halfH + 2.0f * halfH * t, k == 0 ? 0.0f : radius * (1.0f - t), slope, 1.0f, t});
    }
    MeshData side = Revolve(rows, sectors);
    MeshData d = smoothSide ? std::move(side) : Flatten(side);
    AddCap(d, -halfH, radius, sectors, false);
    return Finish(std::move(d));
}

MeshData BuildPrimitive(PrimitiveShape shape, const PrimitiveParams& params) {
    const PrimitiveParams p = params.Resolved(shape);
    switch (shape) {
        case PrimitiveShape::Cube: return BuildCube(p.SubX, p.SubY, p.SubZ);
        case PrimitiveShape::Sphere: return BuildSphere(p.Rings, p.Segments, !p.Flat);
        case PrimitiveShape::Plane: return BuildPlane(p.SubX, p.SubZ);
        case PrimitiveShape::Cylinder: return BuildCylinder(p.Segments, p.HeightSegments, !p.Flat);
        case PrimitiveShape::Cone: return BuildCone(p.Segments, p.HeightSegments, !p.Flat);
        case PrimitiveShape::Capsule: return BuildCapsule(p.Rings, p.Segments, p.Height, p.HeightSegments, !p.Flat);
    }
    return {};
}

} // namespace sage::render
