#include "Mesh.h"
#include "sage/rhi/GraphicsDevice.h"

#include <cmath>
#include <glm/gtc/constants.hpp>

using namespace sage::rhi;

// Вычисляет касательные (tangent) для normal mapping из позиций и UV каждого
// треугольника; аккумулирует касательную И бинормаль по вершинам, затем
// ортонормирует касательную относительно нормали (Gram–Schmidt) и определяет
// ЗНАК ориентации (handedness) w = sign(dot(cross(N,T), B_accum)). Он пишется в
// Tangent.w и в шейдере даёт бинормаль cross(N,T)*w — без него грани с
// зеркальной UV-развёрткой показывали бы ИНВЕРТИРОВАННЫЙ рельеф нормал-карты.
static void ComputeTangents(std::vector<Vertex>& v, const std::vector<unsigned int>& idx) {
    std::vector<glm::vec3> tanAcc(v.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> bitAcc(v.size(), glm::vec3(0.0f));
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        unsigned a = idx[i], b = idx[i + 1], c = idx[i + 2];
        glm::vec3 e1 = v[b].Position - v[a].Position;
        glm::vec3 e2 = v[c].Position - v[a].Position;
        glm::vec2 d1 = v[b].TexCoords - v[a].TexCoords;
        glm::vec2 d2 = v[c].TexCoords - v[a].TexCoords;
        float det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-8f) continue;
        float f = 1.0f / det;
        glm::vec3 t = f * (d2.y * e1 - d1.y * e2);
        glm::vec3 bt = f * (d1.x * e2 - d2.x * e1);
        tanAcc[a] += t; tanAcc[b] += t; tanAcc[c] += t;
        bitAcc[a] += bt; bitAcc[b] += bt; bitAcc[c] += bt;
    }
    for (size_t i = 0; i < v.size(); ++i) {
        glm::vec3 n = v[i].Normal;
        glm::vec3 t = tanAcc[i];
        t = t - n * glm::dot(n, t); // ортогонализация к нормали
        if (glm::dot(t, t) < 1e-10f) {
            // Запасная касательная, перпендикулярная нормали.
            t = glm::abs(n.y) < 0.99f ? glm::cross(glm::vec3(0, 1, 0), n) : glm::vec3(1, 0, 0);
        }
        t = glm::normalize(t);
        // Знак: если накопленная бинормаль смотрит противоположно cross(N,T) —
        // UV зеркальны на этой грани, w = -1 (иначе +1).
        float w = (glm::dot(glm::cross(n, t), bitAcc[i]) < 0.0f) ? -1.0f : 1.0f;
        v[i].Tangent = glm::vec4(t, w);
    }
}

Mesh::Mesh(const std::vector<Vertex>& verticesIn, const std::vector<unsigned int>& indices) {
    m_indexCount = indices.size();

    std::vector<Vertex> vertices = verticesIn;
    ComputeTangents(vertices, indices); // касательные для normal mapping

    VertexLayout layout;
    layout.Stride = sizeof(Vertex);
    layout.Attributes = {
        {0, 3, AttribType::Float, (int)offsetof(Vertex, Position)},
        {1, 3, AttribType::Float, (int)offsetof(Vertex, Normal)},
        {2, 2, AttribType::Float, (int)offsetof(Vertex, TexCoords)},
        {3, 4, AttribType::Float, (int)offsetof(Vertex, Tangent)},
    };
    // Per-instance поток (батчинг): модельная матрица (loc 4..7 — 4 строки) +
    // цвет (loc 8) + metallic (loc 9) + roughness (loc 10). Divisor 1.
    // Не-инстансная отрисовка эти локации не читает.
    layout.InstanceStride = sizeof(MeshInstance);
    layout.InstanceAttributes = {
        {4, 4, AttribType::Float, (int)offsetof(MeshInstance, Model) + 0},
        {5, 4, AttribType::Float, (int)offsetof(MeshInstance, Model) + 16},
        {6, 4, AttribType::Float, (int)offsetof(MeshInstance, Model) + 32},
        {7, 4, AttribType::Float, (int)offsetof(MeshInstance, Model) + 48},
        {8, 3, AttribType::Float, (int)offsetof(MeshInstance, Color)},
        {9, 1, AttribType::Float, (int)offsetof(MeshInstance, Metallic)},
        {10, 1, AttribType::Float, (int)offsetof(MeshInstance, Roughness)},
    };

    m_geometry = GraphicsDevice::Get().CreateGeometry(layout);
    m_geometry->SetVertexData(vertices.data(), vertices.size() * sizeof(Vertex), /*dynamic=*/false);
    m_geometry->SetIndexData(indices.data(), indices.size(), /*dynamic=*/false);

    // Затравка инстанс-буфера одним элементом — чтобы enabled instance-атрибуты
    // не читали из пустого буфера при обычной (не инстансной) отрисовке.
    MeshInstance seed{};
    m_geometry->SetInstanceData(&seed, sizeof(seed));

    // Ограничивающая сфера в локальных координатах: центр — середина AABB,
    // радиус — макс. расстояние вершины от центра. Для отсечения по фрустуму.
    if (!vertices.empty()) {
        glm::vec3 lo = vertices[0].Position, hi = vertices[0].Position;
        for (const Vertex& v : vertices) { lo = glm::min(lo, v.Position); hi = glm::max(hi, v.Position); }
        m_boundsCenter = (lo + hi) * 0.5f;
        float r2 = 0.0f;
        for (const Vertex& v : vertices) r2 = glm::max(r2, glm::dot(v.Position - m_boundsCenter, v.Position - m_boundsCenter));
        m_boundsRadius = std::sqrt(r2);
    }
}

void Mesh::Draw() const {
    m_geometry->DrawIndexed(m_indexCount);
}

void Mesh::SetInstances(const MeshInstance* data, size_t count) const {
    m_geometry->SetInstanceData(data, count * sizeof(MeshInstance));
}

void Mesh::DrawInstances(size_t count) const {
    if (count == 0) return;
    m_geometry->DrawIndexedInstanced(m_indexCount, count);
}

Mesh Mesh::CreateCube() {
    std::vector<Vertex> vertices = {
        // задняя грань (-Z)
        {{0.5f,-0.5f,-0.5f},{0,0,-1},{0,0}}, {{-0.5f,-0.5f,-0.5f},{0,0,-1},{1,0}},
        {{-0.5f, 0.5f,-0.5f},{0,0,-1},{1,1}}, {{0.5f, 0.5f,-0.5f},{0,0,-1},{0,1}},
        // передняя грань (+Z)
        {{-0.5f,-0.5f, 0.5f},{0,0,1},{0,0}}, {{0.5f,-0.5f, 0.5f},{0,0,1},{1,0}},
        {{0.5f, 0.5f, 0.5f},{0,0,1},{1,1}}, {{-0.5f, 0.5f, 0.5f},{0,0,1},{0,1}},
        // левая грань (-X)
        {{-0.5f, 0.5f, 0.5f},{-1,0,0},{1,0}}, {{-0.5f, 0.5f,-0.5f},{-1,0,0},{1,1}},
        {{-0.5f,-0.5f,-0.5f},{-1,0,0},{0,1}}, {{-0.5f,-0.5f, 0.5f},{-1,0,0},{0,0}},
        // правая грань (+X)
        {{0.5f, 0.5f,-0.5f},{1,0,0},{1,0}}, {{0.5f, 0.5f, 0.5f},{1,0,0},{1,1}},
        {{0.5f,-0.5f, 0.5f},{1,0,0},{0,1}}, {{0.5f,-0.5f,-0.5f},{1,0,0},{0,0}},
        // нижняя грань (-Y)
        {{-0.5f,-0.5f,-0.5f},{0,-1,0},{0,1}}, {{0.5f,-0.5f,-0.5f},{0,-1,0},{1,1}},
        {{0.5f,-0.5f, 0.5f},{0,-1,0},{1,0}}, {{-0.5f,-0.5f, 0.5f},{0,-1,0},{0,0}},
        // верхняя грань (+Y)
        {{-0.5f, 0.5f, 0.5f},{0,1,0},{0,1}}, {{0.5f, 0.5f, 0.5f},{0,1,0},{1,1}},
        {{0.5f, 0.5f,-0.5f},{0,1,0},{1,0}}, {{-0.5f, 0.5f,-0.5f},{0,1,0},{0,0}},
    };

    std::vector<unsigned int> indices;
    for (unsigned int face = 0; face < 6; ++face) {
        unsigned int o = face * 4;
        indices.insert(indices.end(), { o, o+1, o+2, o+2, o+3, o });
    }

    return Mesh(vertices, indices);
}

Mesh Mesh::CreateSphere(int rings, int sectors) {
    if (rings < 2) rings = 2;
    if (sectors < 3) sectors = 3;
    const float pi = glm::pi<float>();
    const float radius = 0.5f;

    std::vector<Vertex> vertices;
    for (int r = 0; r <= rings; ++r) {
        float v = (float)r / rings;          // 0..1 сверху вниз
        float phi = v * pi;                  // 0..pi полярный угол
        float y = std::cos(phi);
        float ringR = std::sin(phi);
        for (int s = 0; s <= sectors; ++s) {
            float u = (float)s / sectors;    // 0..1 по окружности
            float theta = u * 2.0f * pi;
            glm::vec3 n(ringR * std::cos(theta), y, ringR * std::sin(theta));
            vertices.push_back({ n * radius, n, {u, 1.0f - v} });
        }
    }

    std::vector<unsigned int> indices;
    int stride = sectors + 1;
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < sectors; ++s) {
            unsigned int a = r * stride + s;
            unsigned int b = a + stride;
            indices.insert(indices.end(), { a, b, a + 1, a + 1, b, b + 1 });
        }
    }
    return Mesh(vertices, indices);
}

Mesh Mesh::CreatePlane(int subdivisions) {
    if (subdivisions < 1) subdivisions = 1;
    std::vector<Vertex> vertices;
    for (int z = 0; z <= subdivisions; ++z) {
        for (int x = 0; x <= subdivisions; ++x) {
            float fx = (float)x / subdivisions - 0.5f; // -0.5..0.5
            float fz = (float)z / subdivisions - 0.5f;
            vertices.push_back({ {fx, 0.0f, fz}, {0, 1, 0},
                                 {(float)x / subdivisions, (float)z / subdivisions} });
        }
    }
    std::vector<unsigned int> indices;
    int stride = subdivisions + 1;
    for (int z = 0; z < subdivisions; ++z) {
        for (int x = 0; x < subdivisions; ++x) {
            unsigned int a = z * stride + x;
            unsigned int b = a + stride;
            indices.insert(indices.end(), { a, a + 1, b, b, a + 1, b + 1 });
        }
    }
    return Mesh(vertices, indices);
}

Mesh Mesh::CreateCylinder(int sectors) {
    if (sectors < 3) sectors = 3;
    const float pi = glm::pi<float>();
    const float radius = 0.5f;
    const float halfH = 0.5f;

    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;

    // Боковая поверхность: два кольца (низ/верх), нормали радиальные.
    for (int ring = 0; ring < 2; ++ring) {
        float y = ring == 0 ? -halfH : halfH;
        for (int s = 0; s <= sectors; ++s) {
            float u = (float)s / sectors;
            float theta = u * 2.0f * pi;
            glm::vec3 n(std::cos(theta), 0.0f, std::sin(theta));
            vertices.push_back({ {n.x * radius, y, n.z * radius}, n, {u, (float)ring} });
        }
    }
    int stride = sectors + 1;
    for (int s = 0; s < sectors; ++s) {
        unsigned int a = s;             // низ
        unsigned int b = stride + s;    // верх
        indices.insert(indices.end(), { a, b, a + 1, a + 1, b, b + 1 });
    }

    // Крышки: центр + веер, нормали по ±Y.
    auto addCap = [&](float y, glm::vec3 normal, bool flip) {
        unsigned int center = (unsigned int)vertices.size();
        vertices.push_back({ {0.0f, y, 0.0f}, normal, {0.5f, 0.5f} });
        unsigned int first = (unsigned int)vertices.size();
        for (int s = 0; s <= sectors; ++s) {
            float theta = (float)s / sectors * 2.0f * pi;
            float cx = std::cos(theta), cz = std::sin(theta);
            vertices.push_back({ {cx * radius, y, cz * radius}, normal, {cx * 0.5f + 0.5f, cz * 0.5f + 0.5f} });
        }
        for (int s = 0; s < sectors; ++s) {
            unsigned int a = first + s, b = first + s + 1;
            if (flip) indices.insert(indices.end(), { center, b, a });
            else      indices.insert(indices.end(), { center, a, b });
        }
    };
    addCap(halfH, {0, 1, 0}, false);
    addCap(-halfH, {0, -1, 0}, true);
    return Mesh(vertices, indices);
}

Mesh Mesh::CreateCone(int sectors) {
    if (sectors < 3) sectors = 3;
    const float pi = glm::pi<float>();
    const float radius = 0.5f;
    const float halfH = 0.5f;

    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;

    // Боковая поверхность: вершина продублирована на каждый сектор, чтобы у
    // конуса были корректные (не усреднённые) боковые нормали. Наклон нормали
    // учитывает высоту — normal = normalize(cos, r/h, sin) в наклонной системе.
    float slope = radius / (2.0f * halfH);
    for (int s = 0; s < sectors; ++s) {
        float t0 = (float)s / sectors * 2.0f * pi;
        float t1 = (float)(s + 1) / sectors * 2.0f * pi;
        float tm = (t0 + t1) * 0.5f;
        glm::vec3 apexN = glm::normalize(glm::vec3(std::cos(tm), slope, std::sin(tm)));
        glm::vec3 n0 = glm::normalize(glm::vec3(std::cos(t0), slope, std::sin(t0)));
        glm::vec3 n1 = glm::normalize(glm::vec3(std::cos(t1), slope, std::sin(t1)));
        unsigned int base = (unsigned int)vertices.size();
        vertices.push_back({ {0.0f, halfH, 0.0f}, apexN, {0.5f, 1.0f} });
        vertices.push_back({ {std::cos(t0) * radius, -halfH, std::sin(t0) * radius}, n0, {(float)s / sectors, 0.0f} });
        vertices.push_back({ {std::cos(t1) * radius, -halfH, std::sin(t1) * radius}, n1, {(float)(s + 1) / sectors, 0.0f} });
        indices.insert(indices.end(), { base, base + 1, base + 2 });
    }

    // Основание (диск, нормаль -Y).
    unsigned int center = (unsigned int)vertices.size();
    vertices.push_back({ {0.0f, -halfH, 0.0f}, {0, -1, 0}, {0.5f, 0.5f} });
    unsigned int first = (unsigned int)vertices.size();
    for (int s = 0; s <= sectors; ++s) {
        float theta = (float)s / sectors * 2.0f * pi;
        float cx = std::cos(theta), cz = std::sin(theta);
        vertices.push_back({ {cx * radius, -halfH, cz * radius}, {0, -1, 0}, {cx * 0.5f + 0.5f, cz * 0.5f + 0.5f} });
    }
    for (int s = 0; s < sectors; ++s) {
        indices.insert(indices.end(), { center, first + s + 1, first + s });
    }
    return Mesh(vertices, indices);
}
