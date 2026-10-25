#include "sage/render/LightClusters.h"

#include <algorithm>
#include <cmath>

#include "sage/core/Log.h"
#include "sage/render/Shader.h"
#include "sage/rhi/GraphicsDevice.h"

namespace sage::render {

namespace {

constexpr float kPi = 3.14159265358979f;

// Шар влияния источника: дальше Range свет гасится (в шейдере — плавно, на
// последней пятой части), так что шар радиуса Range его покрывает целиком.
struct LightSphere {
    glm::vec3 Center;
    float Radius;
};

// Ячейки, которых касается шар (в пространстве вида), — с запасом: лишняя
// ячейка стоит пикселю одного пустого расчёта, недостающая — ступеньки света.
struct CellRange {
    int X0, X1, Y0, Y1, Z0, Z1;
    bool Empty;
};

CellRange RangeOf(const LightSphere& s, const LightClusterData& d) {
    CellRange r{0, ClusterGrid::TilesX - 1, 0, ClusterGrid::TilesY - 1, 0, ClusterGrid::Slices - 1, false};
    const glm::vec3 c = glm::vec3(d.View * glm::vec4(s.Center, 1.0f));
    const float dMin = -c.z - s.Radius, dMax = -c.z + s.Radius;
    if (dMax < d.Depth.Near || dMin > d.Depth.Far) { r.Empty = true; return r; }
    r.Z0 = d.Depth.Slice(std::max(dMin, d.Depth.Near));
    r.Z1 = d.Depth.Slice(std::min(dMax, d.Depth.Far));
    // Шар, задевший ближнюю плоскость, проецируется во весь экран (точки за
    // камерой проекция выворачивает) — экранные границы не сужаем.
    if (dMin <= d.Depth.Near) return r;
    glm::vec2 lo(1e9f), hi(-1e9f);
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner = c + glm::vec3((i & 1) ? s.Radius : -s.Radius,
                                               (i & 2) ? s.Radius : -s.Radius,
                                               (i & 4) ? s.Radius : -s.Radius);
        const glm::vec4 clip = d.Proj * glm::vec4(corner, 1.0f);
        if (clip.w <= 1e-5f) return r;   // угол за камерой — весь экран
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        lo = glm::min(lo, ndc);
        hi = glm::max(hi, ndc);
    }
    if (hi.x < -1.0f || lo.x > 1.0f || hi.y < -1.0f || lo.y > 1.0f) { r.Empty = true; return r; }
    auto tile = [](float ndc, int n) {
        return std::clamp((int)std::floor((ndc * 0.5f + 0.5f) * (float)n), 0, n - 1);
    };
    r.X0 = tile(lo.x, ClusterGrid::TilesX);
    r.X1 = tile(hi.x, ClusterGrid::TilesX);
    r.Y0 = tile(lo.y, ClusterGrid::TilesY);
    r.Y1 = tile(hi.y, ClusterGrid::TilesY);
    return r;
}

int CellIndex(int x, int y, int z) {
    return x + y * ClusterGrid::TilesX + z * ClusterGrid::TilesX * ClusterGrid::TilesY;
}

} // namespace

ClusterDepth ClusterDepth::FromProjection(const glm::mat4& proj) {
    ClusterDepth d;
    const bool ortho = std::fabs(proj[3][3] - 1.0f) < 1e-4f;
    if (ortho) {
        // z_ndc = proj[2][2]*z + proj[3][2]; ближняя -> -1, дальняя -> +1.
        const float a = proj[2][2], b = proj[3][2];
        d.Near = std::fabs(a) > 1e-8f ? (b + 1.0f) / a : 0.0f;   // -z при z_ndc = -1
        d.Far = std::fabs(a) > 1e-8f ? (b - 1.0f) / a : 1000.0f;
        d.Log = false;
    } else {
        const float a = proj[2][2], b = proj[3][2];
        d.Near = b / (a - 1.0f);
        d.Far = b / (a + 1.0f);
        d.Log = true;
    }
    if (d.Log) {
        d.Near = std::max(d.Near, 1e-3f);
        // Дальние срезы на километры — пустая трата разрешения: ламп там не
        // видно. Всё, что дальше, попадает в последний срез.
        d.Far = std::clamp(d.Far, d.Near + 1.0f, d.Near + 500.0f);
    } else {
        // Ортография редактора смотрит и «из-за спины» (ближняя плоскость
        // бывает отрицательной) — диапазон берётся как есть.
        d.Far = std::max(d.Far, d.Near + 1.0f);
    }
    d.Scale = d.Log ? (float)ClusterGrid::Slices / std::log(d.Far / d.Near)
                    : (float)ClusterGrid::Slices / (d.Far - d.Near);
    return d;
}

int ClusterDepth::Slice(float depth) const {
    const float t = Log ? std::log(std::max(depth, Near) / Near) * Scale : (depth - Near) * Scale;
    return std::clamp((int)std::floor(t), 0, ClusterGrid::Slices - 1);
}

std::vector<int> LightClusterData::CellLights(int cell) const {
    std::vector<int> out;
    if (cell < 0 || cell >= ClusterGrid::Cells || Texels.empty()) return out;
    const glm::vec4 g = Texels[(size_t)(GridBase + cell)];
    const int offset = (int)g.x, count = (int)g.y;
    for (int i = 0; i < count; ++i) {
        const int e = offset + i;
        out.push_back((int)Texels[(size_t)(IndexBase + e / 4)][e % 4]);
    }
    return out;
}

LightClusterData BuildLightClusters(const LightingEnvironment& env, const glm::mat4& view,
                                    const glm::mat4& proj) {
    LightClusterData d;
    d.View = view;
    d.Proj = proj;
    d.Depth = ClusterDepth::FromProjection(proj);

    std::vector<glm::vec4> lights;
    std::vector<LightSphere> spheres;
    auto room = [&] { return (int)spheres.size() < LightingEnvironment::MaxLights; };
    for (const PointLight& p : env.PointLights) {
        if (!room()) break;
        if (p.Intensity <= 0.0f) continue;
        lights.push_back(glm::vec4(p.Position, (float)LightClusterData::Point));
        lights.push_back(glm::vec4(p.Color, p.Intensity));
        lights.push_back(glm::vec4(p.Constant(), p.Linear(), p.Quadratic(), (float)p.ShadowSlot));
        lights.push_back(glm::vec4(0.0f));
        lights.push_back(glm::vec4(0.0f));
        lights.push_back(glm::vec4(0.0f, 0.0f, p.Range, 0.0f));
        spheres.push_back({p.Position, p.Range});
    }
    for (const SpotLight& s : env.SpotLights) {
        if (!room()) break;
        if (s.Intensity <= 0.0f) continue;
        lights.push_back(glm::vec4(s.Position, (float)LightClusterData::Spot));
        lights.push_back(glm::vec4(s.Color, s.Intensity));
        lights.push_back(glm::vec4(s.Constant(), s.Linear(), s.Quadratic(), (float)s.ShadowSlot));
        lights.push_back(glm::vec4(s.Direction, s.CosInner()));
        lights.push_back(glm::vec4(0.0f, 0.0f, 0.0f, s.CosOuter()));
        lights.push_back(glm::vec4(0.0f, 0.0f, s.Range, 0.0f));
        spheres.push_back({s.Position, s.Range});
    }
    for (const AreaLight& a : env.AreaLights) {
        if (!room()) break;
        if (a.Intensity <= 0.0f) continue;
        lights.push_back(glm::vec4(a.Position, (float)LightClusterData::Area));
        lights.push_back(glm::vec4(a.Color, a.Intensity));
        lights.push_back(glm::vec4(0.0f, 0.0f, 0.0f, -1.0f));
        lights.push_back(glm::vec4(glm::normalize(a.Direction), 0.0f));
        lights.push_back(glm::vec4(glm::normalize(a.Right), 0.0f));
        lights.push_back(glm::vec4(a.Width * 0.5f, a.Height * 0.5f, a.Range, 0.0f));
        // Шар — от центра панели на дальность плюс её полудиагональ: угол
        // широкой панели светит дальше центра.
        spheres.push_back({a.Position, a.Range + 0.5f * std::sqrt(a.Width * a.Width + a.Height * a.Height)});
    }
    d.LightCount = (int)spheres.size();

    // Два прохода: сначала число ламп в каждой ячейке, потом сами списки
    // подряд — так каждая ячейка читает свой отрезок одним смещением.
    std::vector<CellRange> ranges(spheres.size());
    std::vector<int> counts(ClusterGrid::Cells, 0);
    for (size_t i = 0; i < spheres.size(); ++i) {
        ranges[i] = RangeOf(spheres[i], d);
        if (ranges[i].Empty) continue;
        for (int z = ranges[i].Z0; z <= ranges[i].Z1; ++z)
            for (int y = ranges[i].Y0; y <= ranges[i].Y1; ++y)
                for (int x = ranges[i].X0; x <= ranges[i].X1; ++x) ++counts[(size_t)CellIndex(x, y, z)];
    }
    std::vector<int> offsets(ClusterGrid::Cells, 0);
    int total = 0;
    for (int c = 0; c < ClusterGrid::Cells; ++c) {
        offsets[(size_t)c] = total;
        total += counts[(size_t)c];
    }
    // Переполнение списка обрезает ячейки целиком с конца, а не случайные
    // ссылки: ячейка с частью своих ламп выглядела бы как свет, мигающий кусками.
    if (total > LightClusterData::kMaxIndices) {
        d.Dropped = total - LightClusterData::kMaxIndices;
        total = LightClusterData::kMaxIndices;
    }
    std::vector<int> fill(ClusterGrid::Cells, 0);
    std::vector<float> indices((size_t)total, 0.0f);
    for (size_t i = 0; i < spheres.size(); ++i) {
        if (ranges[i].Empty) continue;
        for (int z = ranges[i].Z0; z <= ranges[i].Z1; ++z)
            for (int y = ranges[i].Y0; y <= ranges[i].Y1; ++y)
                for (int x = ranges[i].X0; x <= ranges[i].X1; ++x) {
                    const int c = CellIndex(x, y, z);
                    const int e = offsets[(size_t)c] + fill[(size_t)c];
                    if (e >= total) continue;
                    indices[(size_t)e] = (float)i;
                    ++fill[(size_t)c];
                }
    }

    d.GridBase = (int)lights.size();
    d.IndexBase = d.GridBase + ClusterGrid::Cells;
    d.IndexCount = total;
    const int texels = d.IndexBase + (total + 3) / 4;
    const int rows = std::max(1, (texels + LightClusterData::kWidth - 1) / LightClusterData::kWidth);
    d.Texels.assign((size_t)rows * LightClusterData::kWidth, glm::vec4(0.0f));
    std::copy(lights.begin(), lights.end(), d.Texels.begin());
    for (int c = 0; c < ClusterGrid::Cells; ++c) {
        const int count = std::max(0, std::min(fill[(size_t)c], total - offsets[(size_t)c]));
        d.Texels[(size_t)(d.GridBase + c)] = glm::vec4((float)offsets[(size_t)c], (float)count, 0.0f, 0.0f);
    }
    for (int e = 0; e < total; ++e) d.Texels[(size_t)(d.IndexBase + e / 4)][e % 4] = indices[(size_t)e];
    return d;
}

int ClusterCellOf(const LightClusterData& d, const glm::vec3& worldPos) {
    const glm::vec4 v = d.View * glm::vec4(worldPos, 1.0f);
    const glm::vec4 clip = d.Proj * v;
    const glm::vec2 ndc = glm::vec2(clip) / std::max(std::fabs(clip.w), 1e-6f);
    const int x = std::clamp((int)std::floor((ndc.x * 0.5f + 0.5f) * ClusterGrid::TilesX), 0, ClusterGrid::TilesX - 1);
    const int y = std::clamp((int)std::floor((ndc.y * 0.5f + 0.5f) * ClusterGrid::TilesY), 0, ClusterGrid::TilesY - 1);
    return CellIndex(x, y, d.Depth.Slice(-v.z));
}

float AreaFormFactor(const glm::vec3& P, const glm::vec3& N, const AreaLight& a) {
    const glm::vec3 dir = glm::normalize(a.Direction), right = glm::normalize(a.Right);
    const glm::vec3 up = glm::normalize(glm::cross(dir, right));
    const float hw = a.Width * 0.5f, hh = a.Height * 0.5f;
    if (glm::dot(P - a.Position, dir) <= 0.0f) return 0.0f;   // за панелью
    const glm::vec3 c[4] = {a.Position + right * hw + up * hh, a.Position - right * hw + up * hh,
                            a.Position - right * hw - up * hh, a.Position + right * hw - up * hh};
    float sum = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const glm::vec3 v0 = glm::normalize(c[i] - P), v1 = glm::normalize(c[(i + 1) % 4] - P);
        const float theta = std::acos(std::clamp(glm::dot(v0, v1), -1.0f, 1.0f));
        const glm::vec3 g = glm::cross(v0, v1);
        const float len = glm::length(g);
        if (len > 1e-6f) sum += theta * glm::dot(g / len, N);
    }
    // Знак — от того, в какую сторону обход углов виден из P (см. шейдер).
    if (glm::dot(glm::cross(c[1] - c[0], c[2] - c[0]), P - c[0]) > 0.0f) sum = -sum;
    return std::max(sum / (2.0f * kPi), 0.0f);
}

LightClusters::LightClusters() = default;
LightClusters::~LightClusters() = default;

void LightClusters::Build(const LightingEnvironment& env, const glm::mat4& view, const glm::mat4& proj) {
    m_data = BuildLightClusters(env, view, proj);
    if (m_data.Dropped > 0) {
        static bool warned = false;
        if (!warned) {
            LOG_WARN("Render") << "Кластерный свет: ссылок «ячейка -> лампа» больше "
                               << LightClusterData::kMaxIndices << ", дальние ячейки обрезаны";
            warned = true;
        }
    }
    if (m_data.LightCount == 0) return;
    const int rows = m_data.Rows();
    if (!m_texture || rows > m_rows) {
        // Запас по строкам вдвое: сцена, где число ламп ходит туда-сюда, не
        // должна пересоздавать текстуру каждый кадр.
        m_rows = std::max(rows, m_rows * 2);
        sage::rhi::Texture2DDesc desc;
        desc.Width = LightClusterData::kWidth;
        desc.Height = m_rows;
        desc.Channels = 4;
        desc.Float32 = true;
        desc.GenerateMipmaps = false;
        desc.FilterMode = sage::rhi::Filter::Nearest;
        desc.WrapMode = sage::rhi::Wrap::ClampEdge;
        m_data.Texels.resize((size_t)m_rows * LightClusterData::kWidth, glm::vec4(0.0f));
        m_texture = sage::rhi::GraphicsDevice::Get().CreateTexture2D(desc, m_data.Texels.data());
        return;
    }
    m_data.Texels.resize((size_t)m_rows * LightClusterData::kWidth, glm::vec4(0.0f));
    m_texture->Update(m_data.Texels.data());
}

void DisableLightClusters(Shader& shader) {
    shader.SetInt("uLightGrid", kLightClusterUnit);
    shader.SetInt("uClustered", 0);
}

void LightClusters::Upload(Shader& shader) const {
    shader.SetInt("uLightGrid", kLightClusterUnit);
    if (!Ready()) {
        shader.SetInt("uClustered", 0);
        return;
    }
    m_texture->Bind(kLightClusterUnit);
    shader.SetInt("uClustered", 1);
    shader.SetInt("uClusterGridBase", m_data.GridBase);
    shader.SetInt("uClusterIndexBase", m_data.IndexBase);
    shader.SetMat4("uClusterView", m_data.View);
    shader.SetMat4("uClusterProj", m_data.Proj);
    shader.SetVec4("uClusterDepth", glm::vec4(m_data.Depth.Near, m_data.Depth.Far, m_data.Depth.Scale,
                                              m_data.Depth.Log ? 1.0f : 0.0f));
}

} // namespace sage::render
