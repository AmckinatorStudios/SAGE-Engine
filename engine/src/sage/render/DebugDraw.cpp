#include "sage/render/DebugDraw.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
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

// Заливка форм: цвет затеняется по тому, насколько грань смотрит на камеру.
// Не освещение сцены — гизмо обязан одинаково читаться в тёмной и в светлой
// сцене, поэтому «свет» у него свой и всегда из глаза.
const char* kSolidVertexSrc = R"GLSL(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec4 aColor;
uniform mat4 uView;
uniform mat4 uProjection;
out vec3 vWorld;
out vec3 vNormal;
out vec4 vColor;
void main() {
    vWorld = aPos;
    vNormal = aNormal;
    vColor = aColor;
    gl_Position = uProjection * uView * vec4(aPos, 1.0);
}
)GLSL";

const char* kSolidFragmentSrc = R"GLSL(
#version 330 core
in vec3 vWorld;
in vec3 vNormal;
in vec4 vColor;
uniform vec3 uCameraPos;
out vec4 FragColor;
void main() {
    vec3 n = normalize(vNormal);
    vec3 v = normalize(uCameraPos - vWorld);
    // Грань к камере — светлая, по касательной — тёмная: так читается объём.
    float facing = clamp(dot(n, v), 0.0, 1.0);
    // Лёгкий верхний свет: без него у коробки, повёрнутой к камере ребром,
    // две видимые грани одного тона и сливаются в плоский шестиугольник.
    float top = 0.85 + 0.15 * clamp(dot(n, normalize(vec3(0.35, 1.0, 0.25))), 0.0, 1.0);
    float shade = (0.38 + 0.62 * facing) * top;
    // Край силуэта чуть плотнее: форма не растворяется на пёстром фоне.
    float alpha = vColor.a * (1.0 + 0.6 * (1.0 - facing));
    FragColor = vec4(vColor.rgb * shade, clamp(alpha, 0.0, 1.0));
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

    m_solidShader = device.CreateShaderProgram(kSolidVertexSrc, kSolidFragmentSrc);
    sage::rhi::VertexLayout solid;
    solid.Stride = sizeof(SolidVertex);
    solid.Attributes = {
        {0, 3, sage::rhi::AttribType::Float, 0},
        {1, 3, sage::rhi::AttribType::Float, (int)offsetof(SolidVertex, Normal)},
        {2, 4, sage::rhi::AttribType::Float, (int)offsetof(SolidVertex, Color)},
    };
    m_solidGeometry = device.CreateGeometry(solid);
}

void DebugDraw::SolidTri(const SolidVertex& a, SolidVertex b, SolidVertex c) {
    const glm::vec3 face = glm::cross(b.Pos - a.Pos, c.Pos - a.Pos);
    if (glm::dot(face, a.Normal + b.Normal + c.Normal) < 0.0f) std::swap(b, c);
    m_solid.push_back(a);
    m_solid.push_back(b);
    m_solid.push_back(c);
}

void DebugDraw::SolidBox(const glm::mat4& transform, glm::vec4 color) {
    // Нормали — обратной транспонированной: у неравномерно растянутого ящика
    // нормаль грани не совпадает с преобразованным локальным вектором.
    const glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(transform)));
    auto P = [&](float x, float y, float z) { return glm::vec3(transform * glm::vec4(x, y, z, 1.0f)); };
    for (int axis = 0; axis < 3; ++axis) {
        for (float sign : {-1.0f, 1.0f}) {
            glm::vec3 n(0.0f);
            n[axis] = sign;
            const glm::vec3 wn = glm::normalize(normalMat * n);
            const int u = (axis + 1) % 3, w = (axis + 2) % 3;
            glm::vec3 corner[4];
            const float us[4] = {-0.5f, 0.5f, 0.5f, -0.5f}, ws[4] = {-0.5f, -0.5f, 0.5f, 0.5f};
            for (int k = 0; k < 4; ++k) {
                glm::vec3 l(0.0f);
                l[axis] = 0.5f * sign;
                l[u] = us[k];
                l[w] = ws[k];
                corner[k] = P(l.x, l.y, l.z);
            }
            SolidTri({corner[0], wn, color}, {corner[1], wn, color}, {corner[2], wn, color});
            SolidTri({corner[0], wn, color}, {corner[2], wn, color}, {corner[3], wn, color});
        }
    }
}

void DebugDraw::SolidCapsule(const glm::mat4& transform, float radius, float halfHeight,
                             glm::vec4 color, int segments) {
    if (segments < 6) segments = 6;
    const int half = std::max(segments / 4, 2);   // колец на полусферу
    const glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(transform)));
    // Кольца снизу вверх: нижняя полусфера, затем верхняя. Два кольца на
    // экваторе (со сдвигом -h и +h) и дают цилиндр между полусферами.
    struct Ring { float Y, R, Ny, Nr; };
    std::vector<Ring> rings;
    for (int i = 0; i <= half; ++i) {
        const float phi = -glm::half_pi<float>() + glm::half_pi<float>() * i / half;
        rings.push_back({std::sin(phi) * radius - halfHeight, std::cos(phi) * radius, std::sin(phi), std::cos(phi)});
    }
    for (int i = 0; i <= half; ++i) {
        const float phi = glm::half_pi<float>() * i / half;
        rings.push_back({std::sin(phi) * radius + halfHeight, std::cos(phi) * radius, std::sin(phi), std::cos(phi)});
    }
    auto vert = [&](const Ring& r, int k) {
        const float a = glm::two_pi<float>() * k / segments;
        const glm::vec3 lp(std::cos(a) * r.R, r.Y, std::sin(a) * r.R);
        const glm::vec3 ln(std::cos(a) * r.Nr, r.Ny, std::sin(a) * r.Nr);
        return SolidVertex{glm::vec3(transform * glm::vec4(lp, 1.0f)),
                           glm::normalize(normalMat * ln), color};
    };
    for (size_t i = 0; i + 1 < rings.size(); ++i) {
        for (int k = 0; k < segments; ++k) {
            const SolidVertex a = vert(rings[i], k), b = vert(rings[i], k + 1);
            const SolidVertex c = vert(rings[i + 1], k + 1), d = vert(rings[i + 1], k);
            // У полюса кольцо вырождается в точку — один треугольник вместо двух.
            if (rings[i].R > 1e-6f) SolidTri(a, b, c);
            if (rings[i + 1].R > 1e-6f) SolidTri(a, c, d);
        }
    }
}

void DebugDraw::SolidSphere(glm::vec3 center, float radius, glm::vec4 color, int segments) {
    SolidCapsule(glm::translate(glm::mat4(1.0f), center), radius, 0.0f, color, segments);
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
        glm::vec3 cx = (i == 0) ? glm::vec3(0.85f, 0.28f, 0.28f) : color;
        glm::vec3 cz = (i == 0) ? glm::vec3(0.28f, 0.42f, 0.88f) : color;
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
    // ЦВЕТА ЧИСТЫЕ. Ось — это ответ на вопрос «куда поедет», и читается он
    // только цветом: приглушённый красный рядом с приглушённым зелёным на
    // пёстрой сцене сливается в «две тёмные палочки». Те же три цвета носят
    // гизмо осей в углу вьюпорта и манипулятор — «красное это X» запоминают
    // один раз и дальше полагаются на это всюду.
    Line(origin, origin + axis(0) * size, {1.0f, 0.22f, 0.22f});
    Line(origin, origin + axis(1) * size, {0.25f, 0.95f, 0.25f});
    Line(origin, origin + axis(2) * size, {0.22f, 0.46f, 1.0f});
}

void DebugDraw::Flush(const glm::mat4& view, const glm::mat4& proj) {
    if (m_vertices.empty() && m_solid.empty()) return;

    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();

    // Заливка — первой и без записи глубины: каркас, нарисованный следом,
    // остаётся виден сквозь неё, а заливки разных форм не выгрызают друг друга.
    // Задние грани отсекаются: иначе дальняя стенка удваивала бы плотность, и
    // форма выглядела бы темнее посередине, чем у краёв, — наоборот объёму.
    if (!m_solid.empty()) {
        m_solidShader->Use();
        m_solidShader->SetMat4("uView", view);
        m_solidShader->SetMat4("uProjection", proj);
        m_solidShader->SetVec3("uCameraPos", glm::vec3(glm::inverse(view)[3]));
        m_solidGeometry->SetVertexData(m_solid.data(), m_solid.size() * sizeof(SolidVertex), true);
        device.SetBlend(true);
        device.SetDepthWrite(false);
        device.SetCullMode(sage::rhi::CullMode::Back);
        m_solidGeometry->DrawArrays(m_solid.size());
        device.SetDepthWrite(true);
        device.SetBlend(false);
        m_solid.clear();
    }
    if (m_vertices.empty()) return;
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
