#include "sage/render/MeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "sage/assets/import/MeshNormalize.h"

namespace sage::render {

unsigned int MeshBuilder::AddVertex(const glm::vec3& pos, const glm::vec3& normal, const glm::vec2& uv) {
    Vertex v{};
    v.Position = pos;
    v.Normal = normal;
    v.TexCoords = uv;
    m_vertices.push_back(v);
    return (unsigned int)m_vertices.size() - 1;
}

bool MeshBuilder::AddTriangle(unsigned int a, unsigned int b, unsigned int c) {
    const unsigned int n = (unsigned int)m_vertices.size();
    if (a >= n || b >= n || c >= n) return false;
    std::vector<unsigned int>& slot = m_slots[(size_t)m_current];
    slot.push_back(a);
    slot.push_back(b);
    slot.push_back(c);
    return true;
}

bool MeshBuilder::AddQuad(unsigned int a, unsigned int b, unsigned int c, unsigned int d) {
    return AddTriangle(a, b, c) && AddTriangle(a, c, d);
}

void MeshBuilder::UseMaterial(int slot) {
    m_current = std::clamp(slot, 0, 63);
    if ((size_t)m_current >= m_slots.size()) m_slots.resize((size_t)m_current + 1);
}

size_t MeshBuilder::TriangleCount() const {
    size_t n = 0;
    for (const auto& s : m_slots) n += s.size() / 3;
    return n;
}

void MeshBuilder::RecalculateNormals(bool smooth) {
    if (!smooth) {
        // Плоские: у каждого треугольника свои три вершины — общая вершина не
        // может нести две разные нормали.
        std::vector<Vertex> out;
        for (auto& slot : m_slots) {
            std::vector<unsigned int> idx;
            for (size_t i = 0; i + 2 < slot.size(); i += 3) {
                Vertex a = m_vertices[slot[i]], b = m_vertices[slot[i + 1]], c = m_vertices[slot[i + 2]];
                glm::vec3 n = glm::cross(b.Position - a.Position, c.Position - a.Position);
                n = glm::length(n) > 1e-12f ? glm::normalize(n) : glm::vec3(0, 1, 0);
                a.Normal = b.Normal = c.Normal = n;
                for (const Vertex& v : {a, b, c}) {
                    idx.push_back((unsigned int)out.size());
                    out.push_back(v);
                }
            }
            slot = std::move(idx);
        }
        m_vertices = std::move(out);
        return;
    }
    // Сглаженные: сумма нормалей граней по вершине (с весом площади — длина
    // векторного произведения), затем нормировка.
    for (Vertex& v : m_vertices) v.Normal = glm::vec3(0.0f);
    for (const auto& slot : m_slots)
        for (size_t i = 0; i + 2 < slot.size(); i += 3) {
            Vertex& a = m_vertices[slot[i]];
            Vertex& b = m_vertices[slot[i + 1]];
            Vertex& c = m_vertices[slot[i + 2]];
            const glm::vec3 n = glm::cross(b.Position - a.Position, c.Position - a.Position);
            a.Normal += n;
            b.Normal += n;
            c.Normal += n;
        }
    for (Vertex& v : m_vertices)
        v.Normal = glm::length(v.Normal) > 1e-12f ? glm::normalize(v.Normal) : glm::vec3(0, 1, 0);
}

void MeshBuilder::RecalculateTangents() {
    MeshData d = ToMeshData();
    m_vertices = d.Vertices;
}

void MeshBuilder::Deform(
    const std::function<glm::vec3(const glm::vec3&, const glm::vec3&, const glm::vec2&)>& fn) {
    if (!fn) return;
    for (Vertex& v : m_vertices) v.Position = fn(v.Position, v.Normal, v.TexCoords);
}

void MeshBuilder::Translate(const glm::vec3& d) {
    for (Vertex& v : m_vertices) v.Position += d;
}

void MeshBuilder::Scale(const glm::vec3& s) {
    for (Vertex& v : m_vertices) v.Position *= s;
    // Неравномерный масштаб перекашивает нормали: их преобразуют обратной
    // матрицей (для диагональной — делением).
    for (Vertex& v : m_vertices) {
        const glm::vec3 n = v.Normal / glm::max(glm::abs(s), glm::vec3(1e-6f)) * glm::sign(s);
        if (glm::length(n) > 1e-12f) v.Normal = glm::normalize(n);
    }
}

void MeshBuilder::Append(const MeshBuilder& other) {
    const unsigned int base = (unsigned int)m_vertices.size();
    m_vertices.insert(m_vertices.end(), other.m_vertices.begin(), other.m_vertices.end());
    if (other.m_slots.size() > m_slots.size()) m_slots.resize(other.m_slots.size());
    for (size_t s = 0; s < other.m_slots.size(); ++s)
        for (unsigned int i : other.m_slots[s]) m_slots[s].push_back(base + i);
}

void MeshBuilder::Clear() {
    m_vertices.clear();
    m_slots.assign(1, {});
    m_current = 0;
}

void MeshBuilder::Bounds(glm::vec3& lo, glm::vec3& hi) const {
    lo = glm::vec3(0.0f);
    hi = glm::vec3(0.0f);
    if (m_vertices.empty()) return;
    lo = hi = m_vertices[0].Position;
    for (const Vertex& v : m_vertices) {
        lo = glm::min(lo, v.Position);
        hi = glm::max(hi, v.Position);
    }
}

MeshData MeshBuilder::ToMeshData() const {
    MeshData d;
    d.Vertices = m_vertices;
    for (size_t s = 0; s < m_slots.size(); ++s) {
        if (m_slots[s].empty()) continue;
        Submesh sub;
        sub.Name = "slot " + std::to_string(s + 1);
        sub.Material = (int)s;
        sub.FirstIndex = (unsigned int)d.Indices.size();
        sub.IndexCount = (unsigned int)m_slots[s].size();
        d.Indices.insert(d.Indices.end(), m_slots[s].begin(), m_slots[s].end());
        d.Submeshes.push_back(sub);
    }
    // Один слот — обычный одноматериальный меш, разметка не нужна.
    if (d.Submeshes.size() == 1 && d.Submeshes[0].Material == 0) d.Submeshes.clear();
    if (!sage::assets::HasNormals(d)) sage::assets::GenerateNormals(d);
    sage::assets::GenerateTangents(d);
    return d;
}

// --- Готовые формы -----------------------------------------------------------

MeshBuilder MeshBuilder::Grid(float sizeX, float sizeZ, int resX, int resZ,
                              const std::function<float(float, float)>& height) {
    MeshBuilder b;
    resX = std::clamp(resX, 1, 2048);
    resZ = std::clamp(resZ, 1, 2048);
    for (int z = 0; z <= resZ; ++z)
        for (int x = 0; x <= resX; ++x) {
            const float u = (float)x / (float)resX, v = (float)z / (float)resZ;
            const float px = (u - 0.5f) * sizeX, pz = (v - 0.5f) * sizeZ;
            const float py = height ? height(px, pz) : 0.0f;
            b.AddVertex({px, py, pz}, height ? glm::vec3(0.0f) : glm::vec3(0, 1, 0), {u, v});
        }
    const int row = resX + 1;
    for (int z = 0; z < resZ; ++z)
        for (int x = 0; x < resX; ++x) {
            const unsigned int i = (unsigned int)(z * row + x);
            // Обход против часовой, если смотреть сверху (+Y).
            b.AddQuad(i, i + row, i + row + 1, i + 1);
        }
    if (height) b.RecalculateNormals(true);
    return b;
}

MeshBuilder MeshBuilder::Box(const glm::vec3& size) {
    MeshBuilder b;
    const glm::vec3 h = size * 0.5f;
    // Шесть граней по своим четыре вершины: у куба нормали на рёбрах резкие.
    const glm::vec3 n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const glm::vec3& f : n) {
        const glm::vec3 up = std::fabs(f.y) > 0.5f ? glm::vec3(0, 0, -f.y) : glm::vec3(0, 1, 0);
        const glm::vec3 right = glm::cross(up, f);
        const glm::vec3 c = f * h;
        const glm::vec3 r = right * h, u = up * h;
        const unsigned int a = b.AddVertex(c - r - u, f, {0, 0});
        const unsigned int bb = b.AddVertex(c + r - u, f, {1, 0});
        const unsigned int cc = b.AddVertex(c + r + u, f, {1, 1});
        const unsigned int d = b.AddVertex(c - r + u, f, {0, 1});
        b.AddQuad(a, bb, cc, d);
    }
    return b;
}

MeshBuilder MeshBuilder::Sphere(float radius, int segments, int rings) {
    MeshBuilder b;
    segments = std::clamp(segments, 3, 512);
    rings = std::clamp(rings, 2, 512);
    const float pi = 3.14159265358979f;
    for (int r = 0; r <= rings; ++r) {
        const float v = (float)r / (float)rings, phi = v * pi;
        // У полюсов sin(pi) во float не ноль (~1e-7): вершины «полюса» расходились
        // бы кольцом, и вырожденные треугольники у него получали случайный обход.
        const bool pole = r == 0 || r == rings;
        const float sp = pole ? 0.0f : std::sin(phi), cp = pole ? (r == 0 ? 1.0f : -1.0f) : std::cos(phi);
        for (int s = 0; s <= segments; ++s) {
            const float u = (float)s / (float)segments, th = u * 2.0f * pi;
            const glm::vec3 n(sp * std::cos(th), cp, sp * std::sin(th));
            b.AddVertex(n * radius, n, {u, 1.0f - v});
        }
    }
    const int row = segments + 1;
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < segments; ++s) {
            const unsigned int i = (unsigned int)(r * row + s);
            b.AddQuad(i, i + 1, i + row + 1, i + row);
        }
    return b;
}

MeshBuilder MeshBuilder::Cylinder(float radius, float height, int segments, bool caps) {
    MeshBuilder b;
    segments = std::clamp(segments, 3, 512);
    const float pi = 3.14159265358979f, h = height * 0.5f;
    for (int s = 0; s <= segments; ++s) {
        const float u = (float)s / (float)segments, th = u * 2.0f * pi;
        const glm::vec3 n(std::cos(th), 0.0f, std::sin(th));
        b.AddVertex(n * radius + glm::vec3(0, -h, 0), n, {u, 0});
        b.AddVertex(n * radius + glm::vec3(0, h, 0), n, {u, 1});
    }
    for (int s = 0; s < segments; ++s) {
        const unsigned int i = (unsigned int)(s * 2);
        b.AddQuad(i, i + 1, i + 3, i + 2);
    }
    if (caps) {
        for (int side = 0; side < 2; ++side) {
            const float y = side ? h : -h;
            const glm::vec3 n(0, side ? 1.0f : -1.0f, 0);
            const unsigned int c = b.AddVertex({0, y, 0}, n, {0.5f, 0.5f});
            const unsigned int first = (unsigned int)b.VertexCount();
            for (int s = 0; s <= segments; ++s) {
                const float th = (float)s / (float)segments * 2.0f * pi;
                b.AddVertex({std::cos(th) * radius, y, std::sin(th) * radius}, n,
                            {0.5f + 0.5f * std::cos(th), 0.5f + 0.5f * std::sin(th)});
            }
            for (int s = 0; s < segments; ++s) {
                const unsigned int a = first + (unsigned int)s, d = a + 1;
                if (side) b.AddTriangle(c, d, a);
                else b.AddTriangle(c, a, d);
            }
        }
    }
    return b;
}

} // namespace sage::render
