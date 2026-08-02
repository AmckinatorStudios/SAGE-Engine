#include "sage/render/DebugDraw.h"

#include <cmath>
#include <glm/gtc/constants.hpp>

namespace {

// Шейдеры встроены строками: DebugDraw — часть ядра движка и не может
// полагаться на файлы ассетов конкретного приложения (у каждого приложения
// свой каталог assets рядом с бинарником).
const char* kVertexSrc = R"GLSL(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aColor;
uniform mat4 uView;
uniform mat4 uProjection;
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = uProjection * uView * vec4(aPos, 1.0);
}
)GLSL";

const char* kFragmentSrc = R"GLSL(
#version 330 core
in vec3 vColor;
out vec4 FragColor;
void main() {
    FragColor = vec4(vColor, 1.0);
}
)GLSL";

} // namespace

DebugDraw::DebugDraw() {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    m_shader = device.CreateShaderProgram(kVertexSrc, kFragmentSrc);

    sage::rhi::VertexLayout layout;
    layout.Stride = sizeof(LineVertex);
    layout.Attributes = {
        {0, 3, sage::rhi::AttribType::Float, 0},
        {1, 3, sage::rhi::AttribType::Float, (int)offsetof(LineVertex, Color)},
    };
    m_geometry = device.CreateGeometry(layout);
}

void DebugDraw::Line(glm::vec3 a, glm::vec3 b, glm::vec3 color) {
    m_vertices.push_back({a, color});
    m_vertices.push_back({b, color});
}

void DebugDraw::Grid(glm::vec3 center, float halfExtent, float step, glm::vec3 color) {
    if (step <= 0.0f || halfExtent <= 0.0f) return;
    int lines = (int)(halfExtent / step);
    for (int i = -lines; i <= lines; ++i) {
        float offset = i * step;
        // Линии, проходящие через центр, — это оси мира: X красным, Z синим.
        glm::vec3 cx = (i == 0) ? glm::vec3(0.75f, 0.35f, 0.35f) : color;
        glm::vec3 cz = (i == 0) ? glm::vec3(0.35f, 0.45f, 0.80f) : color;
        Line(center + glm::vec3(-halfExtent, 0, offset), center + glm::vec3(halfExtent, 0, offset), cx);
        Line(center + glm::vec3(offset, 0, -halfExtent), center + glm::vec3(offset, 0, halfExtent), cz);
    }
}

void DebugDraw::WireBox(const glm::mat4& transform, glm::vec3 color) {
    // 8 углов единичного куба -0.5..+0.5 в мировых координатах.
    glm::vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        glm::vec3 local((i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f);
        c[i] = glm::vec3(transform * glm::vec4(local, 1.0f));
    }
    // 12 рёбер: по 4 вдоль каждой оси (индексные пары отличаются одним битом).
    static const int edges[12][2] = {
        {0,1},{2,3},{4,5},{6,7}, // вдоль X
        {0,2},{1,3},{4,6},{5,7}, // вдоль Y
        {0,4},{1,5},{2,6},{3,7}, // вдоль Z
    };
    for (const auto& e : edges) Line(c[e[0]], c[e[1]], color);
}

void DebugDraw::WireBox(glm::vec3 center, glm::vec3 halfExtents, glm::vec3 color) {
    glm::mat4 m(1.0f);
    m[0][0] = halfExtents.x * 2.0f;
    m[1][1] = halfExtents.y * 2.0f;
    m[2][2] = halfExtents.z * 2.0f;
    m[3] = glm::vec4(center, 1.0f);
    WireBox(m, color);
}

void DebugDraw::WireCapsule(const glm::mat4& transform, float radius, float halfHeight,
                            glm::vec3 color, int segments) {
    if (segments < 6) segments = 6;
    const float twoPi = glm::two_pi<float>();
    auto at = [&](const glm::vec3& local) {
        return glm::vec3(transform * glm::vec4(local, 1.0f));
    };

    // Два кольца по торцам цилиндра.
    for (int i = 0; i < segments; ++i) {
        const float a0 = twoPi * i / segments;
        const float a1 = twoPi * (i + 1) / segments;
        const glm::vec2 p0(std::cos(a0) * radius, std::sin(a0) * radius);
        const glm::vec2 p1(std::cos(a1) * radius, std::sin(a1) * radius);
        for (float y : {-halfHeight, halfHeight}) {
            Line(at({p0.x, y, p0.y}), at({p1.x, y, p1.y}), color);
        }
    }

    // Четыре образующие цилиндра — по осям X и Z.
    for (int i = 0; i < 4; ++i) {
        const float a = twoPi * i / 4.0f;
        const glm::vec3 off(std::cos(a) * radius, 0.0f, std::sin(a) * radius);
        Line(at(off + glm::vec3(0, -halfHeight, 0)), at(off + glm::vec3(0, halfHeight, 0)), color);
    }

    // Полусферы: две дуги на каждом торце (в плоскостях XY и ZY).
    const int arc = segments / 2 + 1;
    for (int i = 0; i < arc; ++i) {
        const float t0 = glm::half_pi<float>() * i / arc;
        const float t1 = glm::half_pi<float>() * (i + 1) / arc;
        const float c0 = std::cos(t0) * radius, s0 = std::sin(t0) * radius;
        const float c1 = std::cos(t1) * radius, s1 = std::sin(t1) * radius;
        for (float sign : {1.0f, -1.0f}) {
            const float y = sign * halfHeight;
            Line(at({c0, y + sign * s0, 0}), at({c1, y + sign * s1, 0}), color);
            Line(at({-c0, y + sign * s0, 0}), at({-c1, y + sign * s1, 0}), color);
            Line(at({0, y + sign * s0, c0}), at({0, y + sign * s1, c1}), color);
            Line(at({0, y + sign * s0, -c0}), at({0, y + sign * s1, -c1}), color);
        }
    }
}

void DebugDraw::WireSphere(glm::vec3 center, float radius, glm::vec3 color, int segments) {
    if (segments < 4) segments = 4;
    float twoPi = glm::two_pi<float>();
    for (int i = 0; i < segments; ++i) {
        float a0 = twoPi * i / segments;
        float a1 = twoPi * (i + 1) / segments;
        glm::vec2 p0(std::cos(a0) * radius, std::sin(a0) * radius);
        glm::vec2 p1(std::cos(a1) * radius, std::sin(a1) * radius);
        Line(center + glm::vec3(p0.x, p0.y, 0), center + glm::vec3(p1.x, p1.y, 0), color); // XY
        Line(center + glm::vec3(p0.x, 0, p0.y), center + glm::vec3(p1.x, 0, p1.y), color); // XZ
        Line(center + glm::vec3(0, p0.x, p0.y), center + glm::vec3(0, p1.x, p1.y), color); // YZ
    }
}

void DebugDraw::WireCone(glm::vec3 apex, glm::vec3 dir, float length, float halfAngleDeg,
                         glm::vec3 color, int segments) {
    if (segments < 3) segments = 3;
    dir = glm::normalize(dir);
    if (glm::length(dir) < 0.0001f) dir = glm::vec3(0.0f, -1.0f, 0.0f);

    // Ортонормальный базис вокруг оси конуса: любой не-параллельный вектор.
    glm::vec3 up = (glm::abs(dir.y) > 0.99f) ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 right = glm::normalize(glm::cross(dir, up));
    up = glm::normalize(glm::cross(right, dir));

    glm::vec3 center = apex + dir * length;
    float radius = length * std::tan(glm::radians(halfAngleDeg));
    float twoPi = glm::two_pi<float>();

    glm::vec3 prev;
    for (int i = 0; i <= segments; ++i) {
        float a = twoPi * i / segments;
        glm::vec3 p = center + (right * std::cos(a) + up * std::sin(a)) * radius;
        if (i > 0) Line(prev, p, color);   // окружность основания
        if (i < segments) Line(apex, p, color); // боковые рёбра от вершины
        prev = p;
    }
}

void DebugDraw::WireFrustum(glm::vec3 apex, glm::vec3 dir, float fovDeg, float aspect,
                            float nearDist, float farDist, glm::vec3 color) {
    dir = glm::normalize(dir);
    if (glm::length(dir) < 0.0001f) dir = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 upHint = (glm::abs(dir.y) > 0.99f) ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 right = glm::normalize(glm::cross(dir, upHint));
    glm::vec3 up = glm::normalize(glm::cross(right, dir));

    auto rect = [&](float dist, glm::vec3 out[4]) {
        float h = std::tan(glm::radians(fovDeg) * 0.5f) * dist;
        float w = h * aspect;
        glm::vec3 c = apex + dir * dist;
        out[0] = c + up * h - right * w; // TL
        out[1] = c + up * h + right * w; // TR
        out[2] = c - up * h + right * w; // BR
        out[3] = c - up * h - right * w; // BL
    };
    glm::vec3 n[4], f[4];
    rect(nearDist, n);
    rect(farDist, f);
    for (int i = 0; i < 4; ++i) {
        Line(n[i], n[(i + 1) % 4], color); // ближняя рамка
        Line(f[i], f[(i + 1) % 4], color); // дальняя рамка
        Line(n[i], f[i], color);           // боковые рёбра
    }
    // Хвостик от вершины к ближней рамке — читается как «камера».
    for (int i = 0; i < 4; ++i) Line(apex, n[i], color * 0.7f);
}

void DebugDraw::Axes(const glm::mat4& transform, float size) {
    glm::vec3 origin = glm::vec3(transform[3]);
    // Нормализуем базисные векторы: оси показывают ОРИЕНТАЦИЮ, длина всегда
    // size — иначе у сущности с масштабом 6 оси улетают за экран.
    auto axis = [&](int col) {
        glm::vec3 v = glm::vec3(transform[col]);
        float len = glm::length(v);
        return len > 0.0001f ? v / len : glm::vec3(0.0f);
    };
    Line(origin, origin + axis(0) * size, {0.9f, 0.2f, 0.2f});
    Line(origin, origin + axis(1) * size, {0.2f, 0.85f, 0.25f});
    Line(origin, origin + axis(2) * size, {0.25f, 0.4f, 0.95f});
}

void DebugDraw::Flush(const glm::mat4& view, const glm::mat4& proj) {
    if (m_vertices.empty()) return;

    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    m_shader->Use();
    m_shader->SetMat4("uView", view);
    m_shader->SetMat4("uProjection", proj);

    m_geometry->SetVertexData(m_vertices.data(), m_vertices.size() * sizeof(LineVertex), /*dynamic=*/true);

    // Тест глубины оставляем ВКЛЮЧЁННЫМ (объекты сцены заслоняют линии — в
    // этом весь смысл), но запись глубины выключаем: гизмо не должны
    // заслонять то, что рисуется после них.
    device.SetDepthWrite(false);
    m_geometry->DrawLines(m_vertices.size());
    device.SetDepthWrite(true);

    m_vertices.clear();
}
