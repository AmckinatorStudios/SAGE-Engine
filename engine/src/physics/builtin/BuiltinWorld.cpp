#include "physics/builtin/BuiltinWorld.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "sage/core/Log.h"

namespace sage::physics {

namespace {
// Итераций решателя скоростей на под-шаг: больше — устойчивее высокие стопки.
constexpr int kSolverIterations = 8;

// AABB-полуразмеры одиночной формы (сфера/капсула приближаются боксом).
glm::vec3 HalfOfShape(ShapeType shape, const glm::vec3& halfExtents, float radius, float halfHeight) {
    switch (shape) {
        case ShapeType::Sphere:  return glm::vec3(radius);
        case ShapeType::Capsule: return glm::vec3(radius, halfHeight + radius, radius);
        default:                 return halfExtents; // Box
    }
}

// AABB-полуразмеры по описанию тела. Составное тело (Children) сводим к
// объемлющему AABB — аркадное приближение (вращение дочерних форм игнорируем).
glm::vec3 HalfFromDesc(const BodyDesc& d) {
    if (d.Children.empty())
        return HalfOfShape(d.Shape, d.HalfExtents, d.Radius, d.HalfHeight);

    glm::vec3 lo(1e9f), hi(-1e9f);
    for (const ChildShape& c : d.Children) {
        glm::vec3 h = HalfOfShape(c.Shape, c.HalfExtents, c.Radius, c.HalfHeight);
        lo = glm::min(lo, c.Position - h);
        hi = glm::max(hi, c.Position + h);
    }
    // Половина габарита + сдвиг центра учитываем как симметричный AABB вокруг
    // центра тела (простое, но устойчивое приближение для аркадной физики).
    return glm::max(glm::abs(lo), glm::abs(hi));
}
} // namespace

// --- Генерация контакта (нормаль всегда направлена от A к B) ----------------
namespace {

// Сфера A против сферы B.
bool SphereSphere(const glm::vec3& pa, float ra, const glm::vec3& pb, float rb,
                  glm::vec3& normal, float& depth) {
    glm::vec3 d = pb - pa;
    float dist2 = glm::dot(d, d);
    float r = ra + rb;
    if (dist2 >= r * r) return false;
    float dist = std::sqrt(dist2);
    normal = dist > 1e-6f ? d / dist : glm::vec3(0, 1, 0);
    depth = r - dist;
    return true;
}

// Сфера (центр ps, радиус rs) против AABB-бокса (центр pb, полуразмер hb).
// Нормаль направлена от сферы к боксу.
bool SphereBox(const glm::vec3& ps, float rs, const glm::vec3& pb, const glm::vec3& hb,
               glm::vec3& normal, float& depth) {
    glm::vec3 closest = glm::clamp(ps, pb - hb, pb + hb);
    glm::vec3 d = closest - ps; // от центра сферы к ближайшей точке бокса
    float dist2 = glm::dot(d, d);
    if (dist2 > 1e-12f) {
        float dist = std::sqrt(dist2);
        if (dist >= rs) return false; // центр снаружи и дальше радиуса
        normal = d / dist;
        depth = rs - dist;
        return true;
    }
    // Центр сферы внутри бокса — выталкиваем по оси ближайшей грани.
    glm::vec3 delta = ps - pb;
    glm::vec3 overlap = hb - glm::abs(delta);
    int axis = 0;
    if (overlap.y < overlap.x) axis = 1;
    if (overlap.z < overlap[axis]) axis = 2;
    float sign = delta[axis] >= 0.0f ? 1.0f : -1.0f;
    normal = glm::vec3(0.0f);
    normal[axis] = -sign; // от сферы к центру бокса
    depth = overlap[axis] + rs;
    return true;
}

// Ближайшая точка отрезка [a,b] к точке p.
glm::vec3 ClosestOnSegment(const glm::vec3& a, const glm::vec3& b, const glm::vec3& p) {
    glm::vec3 ab = b - a;
    float len2 = glm::dot(ab, ab);
    if (len2 < 1e-12f) return a;
    float t = glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
    return a + t * ab;
}

// Ближайшие точки между двумя отрезками [p1,q1] и [p2,q2] (Ericson, RTCD).
void ClosestSegSeg(const glm::vec3& p1, const glm::vec3& q1,
                   const glm::vec3& p2, const glm::vec3& q2,
                   glm::vec3& c1, glm::vec3& c2) {
    glm::vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    float a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
    float s, t;
    if (a <= 1e-12f && e <= 1e-12f) { c1 = p1; c2 = p2; return; }
    if (a <= 1e-12f) { s = 0.0f; t = glm::clamp(f / e, 0.0f, 1.0f); }
    else {
        float c = glm::dot(d1, r);
        if (e <= 1e-12f) { t = 0.0f; s = glm::clamp(-c / a, 0.0f, 1.0f); }
        else {
            float b = glm::dot(d1, d2), denom = a * e - b * b;
            s = denom > 1e-12f ? glm::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) { t = 0.0f; s = glm::clamp(-c / a, 0.0f, 1.0f); }
            else if (t > 1.0f) { t = 1.0f; s = glm::clamp((b - c) / a, 0.0f, 1.0f); }
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
}

// AABB-бокс A против AABB-бокса B — ось минимального проникновения.
bool BoxBox(const glm::vec3& pa, const glm::vec3& ha, const glm::vec3& pb, const glm::vec3& hb,
            glm::vec3& normal, float& depth) {
    glm::vec3 delta = pb - pa;
    glm::vec3 overlap = (ha + hb) - glm::abs(delta);
    if (overlap.x <= 0.0f || overlap.y <= 0.0f || overlap.z <= 0.0f) return false;
    int axis = 0;
    if (overlap.y < overlap.x) axis = 1;
    if (overlap.z < overlap[axis]) axis = 2;
    float sign = delta[axis] >= 0.0f ? 1.0f : -1.0f;
    normal = glm::vec3(0.0f);
    normal[axis] = sign; // от A к B
    depth = overlap[axis];
    return true;
}

} // namespace

// Диспетчер по формам. Нормаль всегда от A к B. Сфера и капсула — «круглые»
// формы (осевой отрезок вдоль Y + радиус; сфера — отрезок нулевой длины); бокс
// — AABB.
bool BuiltinWorld::Collide(const Body& A, const Body& B, glm::vec3& normal, float& depth) {
    // Осевой отрезок круглой формы (для сферы hh=0 -> вырожденная точка).
    auto seg = [](const Body& b) -> std::pair<glm::vec3, glm::vec3> {
        float hh = (b.Kind == Shape::Capsule) ? b.HalfHeight : 0.0f;
        glm::vec3 up(0.0f, hh, 0.0f);
        return {b.Position - up, b.Position + up};
    };
    bool roundA = A.Kind != Shape::Box;
    bool roundB = B.Kind != Shape::Box;

    if (roundA && roundB) {
        // Ближайшие точки осей -> задача двух сфер в этих точках.
        auto [a1, a2] = seg(A);
        auto [b1, b2] = seg(B);
        glm::vec3 ca, cb;
        ClosestSegSeg(a1, a2, b1, b2, ca, cb);
        return SphereSphere(ca, A.Radius, cb, B.Radius, normal, depth);
    }
    if (roundA && !roundB) {
        auto [a1, a2] = seg(A);
        glm::vec3 q = ClosestOnSegment(a1, a2, B.Position); // точка оси, ближайшая к боксу
        return SphereBox(q, A.Radius, B.Position, B.Half, normal, depth);
    }
    if (!roundA && roundB) {
        auto [b1, b2] = seg(B);
        glm::vec3 q = ClosestOnSegment(b1, b2, A.Position);
        bool hit = SphereBox(q, B.Radius, A.Position, A.Half, normal, depth); // B->A
        normal = -normal;                                                     // -> A->B
        return hit;
    }
    return BoxBox(A.Position, A.Half, B.Position, B.Half, normal, depth);
}

// Нормальный импульс (с упругостью) + кулоновское трение по касательной.
void BuiltinWorld::SolveVelocity(const Contact& c) {
    Body& A = *c.A;
    Body& B = *c.B;
    float invSum = A.InvMass + B.InvMass;
    if (invSum <= 0.0f) return;

    glm::vec3 rv = B.Velocity - A.Velocity;
    float vn = glm::dot(rv, c.Normal);
    if (vn > 0.0f) return; // тела уже расходятся

    float e = std::min(A.Restitution, B.Restitution);
    float jn = -(1.0f + e) * vn / invSum;
    glm::vec3 impulse = jn * c.Normal;
    A.Velocity -= impulse * A.InvMass;
    B.Velocity += impulse * B.InvMass;

    // Трение: импульс по касательной, ограниченный μ·jn (конус трения).
    rv = B.Velocity - A.Velocity;
    glm::vec3 tangent = rv - glm::dot(rv, c.Normal) * c.Normal;
    float tlen = glm::length(tangent);
    if (tlen > 1e-6f) {
        tangent /= tlen;
        float jt = -glm::dot(rv, tangent) / invSum;
        float mu = std::sqrt(A.Friction * B.Friction);
        jt = std::clamp(jt, -jn * mu, jn * mu);
        glm::vec3 fimp = jt * tangent;
        A.Velocity -= fimp * A.InvMass;
        B.Velocity += fimp * B.InvMass;
    }
}

// Позиционная коррекция (нелинейная проекция) с распределением по обратной массе
// и допуском slop — чтобы стопки не «тонули» и не дрожали на контакте покоя.
void BuiltinWorld::CorrectPosition(const Contact& c) {
    Body& A = *c.A;
    Body& B = *c.B;
    float invSum = A.InvMass + B.InvMass;
    if (invSum <= 0.0f) return;
    const float slop = 0.005f;    // допустимое проникновение (не выталкиваем)
    const float percent = 0.8f;   // доля коррекции за проход
    float corr = std::max(c.Depth - slop, 0.0f) / invSum * percent;
    glm::vec3 push = corr * c.Normal;
    A.Position -= push * A.InvMass;
    B.Position += push * B.InvMass;
}

BodyHandle BuiltinWorld::CreateBody(const BodyDesc& desc) {
    Body b;
    b.Type = desc.Type;
    // Одиночные Sphere/Capsule — настоящие формы; составное тело приближается
    // AABB (объемлющий бокс дочерних форм), бокс — по HalfExtents.
    if (desc.Children.empty() && desc.Shape == ShapeType::Sphere) {
        b.Kind = Shape::Sphere;
        b.Radius = desc.Radius;
        b.Half = glm::vec3(desc.Radius);
    } else if (desc.Children.empty() && desc.Shape == ShapeType::Capsule) {
        b.Kind = Shape::Capsule;
        b.Radius = desc.Radius;
        b.HalfHeight = desc.HalfHeight;
        b.Half = glm::vec3(desc.Radius, desc.HalfHeight + desc.Radius, desc.Radius);
    } else {
        b.Kind = Shape::Box;
        b.Half = HalfFromDesc(desc);
    }
    b.Position = desc.Position;
    b.Rotation = desc.Rotation;
    b.InvMass = (desc.Type == BodyType::Dynamic && desc.Mass > 0.0f) ? 1.0f / desc.Mass : 0.0f;
    b.Friction = desc.Friction;
    b.Restitution = desc.Restitution;
    BodyHandle h = m_next++;
    m_bodies[h] = b;
    return h;
}

void BuiltinWorld::RemoveBody(BodyHandle body) { m_bodies.erase(body); }

void BuiltinWorld::AddImpulse(BodyHandle body, const glm::vec3& impulse) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end()) return;
    Body& b = it->second;
    if (b.InvMass <= 0.0f) return;           // static/kinematic не толкаются
    b.Velocity += impulse * b.InvMass;       // Δv = J / m
}

JointHandle BuiltinWorld::CreateJoint(const JointDesc&) {
    // Встроенный аркадный бэкенд не решает соединений — для суставов/тросов/
    // ragdoll выбирают Backend::Jolt. Предупреждаем один раз, не заваливаем лог.
    if (!m_warnedNoJoints) {
        LOG_WARN("Physics") << "Встроенный бэкенд не поддерживает соединения (joints) — "
                               "для суставов/ragdoll используйте Backend::Jolt";
        m_warnedNoJoints = true;
    }
    return kInvalidJoint;
}

void BuiltinWorld::RemoveJoint(JointHandle) {}

void BuiltinWorld::Step(float dt) {
    // Фиксированный шаг 1/120 c аккумулятором — стабильнее переменного dt.
    const float fixed = 1.0f / 120.0f;
    m_accum += std::min(dt, 0.1f); // защита от «спирали смерти» при лагах
    int guard = 0;
    while (m_accum >= fixed && guard++ < 16) {
        SubStep(fixed);
        m_accum -= fixed;
    }
}

void BuiltinWorld::SubStep(float dt) {
    // 1. Интегрируем скорость/позицию динамических тел (полу-неявно).
    for (auto& [h, b] : m_bodies) {
        if (b.Type != BodyType::Dynamic) continue;
        b.Velocity += m_gravity * dt;
        b.Position += b.Velocity * dt;
    }

    // 2. Широкая фаза: собираем контакты всех пар, где есть хотя бы одно
    //    динамическое тело (static-static/kinematic не сталкиваем). O(n²) —
    //    достаточно для аркадных сцен (десятки тел). Указатели в m_bodies
    //    стабильны на время под-шага (нет вставок/удалений).
    std::vector<Body*> list;
    list.reserve(m_bodies.size());
    for (auto& [h, b] : m_bodies) list.push_back(&b);

    std::vector<Contact> contacts;
    for (size_t i = 0; i < list.size(); ++i) {
        for (size_t j = i + 1; j < list.size(); ++j) {
            Body* A = list[i];
            Body* B = list[j];
            if (A->Type != BodyType::Dynamic && B->Type != BodyType::Dynamic) continue;
            glm::vec3 normal;
            float depth;
            if (Collide(*A, *B, normal, depth))
                contacts.push_back({A, B, normal, depth});
        }
    }

    // 3. Решатель скоростей: несколько итераций последовательных импульсов —
    //    так стопки/кучи сходятся к устойчивому покою (одной итерации мало).
    for (int it = 0; it < kSolverIterations; ++it)
        for (const Contact& c : contacts) SolveVelocity(c);

    // 4. Позиционная коррекция проникновений (после решателя скоростей).
    for (const Contact& c : contacts) CorrectPosition(c);
}

void BuiltinWorld::GetBodyTransform(BodyHandle body, glm::vec3& position, glm::quat& rotation) const {
    auto it = m_bodies.find(body);
    if (it != m_bodies.end()) { position = it->second.Position; rotation = it->second.Rotation; }
}

void BuiltinWorld::SetBodyTransform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end()) return;
    it->second.Position = position;
    it->second.Rotation = rotation;
    if (it->second.Type != BodyType::Dynamic) it->second.Velocity = glm::vec3(0.0f);
}

void BuiltinWorld::SetLinearVelocity(BodyHandle body, const glm::vec3& velocity) {
    auto it = m_bodies.find(body);
    if (it != m_bodies.end()) it->second.Velocity = velocity;
}

glm::vec3 BuiltinWorld::GetLinearVelocity(BodyHandle body) const {
    auto it = m_bodies.find(body);
    return it != m_bodies.end() ? it->second.Velocity : glm::vec3(0.0f);
}

} // namespace sage::physics
