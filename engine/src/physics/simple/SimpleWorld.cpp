#include "physics/simple/SimpleWorld.h"

#include <algorithm>
#include <cmath>

#include "sage/core/Log.h"

namespace sage::physics {

namespace {
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

BodyHandle SimpleWorld::CreateBody(const BodyDesc& desc) {
    Body b;
    b.Type = desc.Type;
    b.Half = HalfFromDesc(desc);
    b.Position = desc.Position;
    b.Rotation = desc.Rotation;
    b.InvMass = (desc.Type == BodyType::Dynamic && desc.Mass > 0.0f) ? 1.0f / desc.Mass : 0.0f;
    b.Friction = desc.Friction;
    b.Restitution = desc.Restitution;
    b.Layer = desc.Layer;
    b.Sensor = desc.Sensor;
    BodyHandle h = m_next++;
    m_bodies[h] = b;
    return h;
}

void SimpleWorld::RemoveBody(BodyHandle body) { m_bodies.erase(body); }

void SimpleWorld::AddImpulse(BodyHandle body, const glm::vec3& impulse) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end()) return;
    Body& b = it->second;
    if (b.InvMass <= 0.0f) return;           // static/kinematic не толкаются
    b.Velocity += impulse * b.InvMass;       // Δv = J / m
}

JointHandle SimpleWorld::CreateJoint(const JointDesc&) {
    // Встроенный аркадный бэкенд не решает соединений — для суставов/тросов/
    // ragdoll выбирают Backend::Jolt. Предупреждаем один раз, не заваливаем лог.
    if (!m_warnedNoJoints) {
        LOG_WARN("Physics") << "Simple-бэкенд не поддерживает соединения (joints) — "
                               "для суставов/ragdoll используйте Backend::Jolt";
        m_warnedNoJoints = true;
    }
    return kInvalidJoint;
}

void SimpleWorld::RemoveJoint(JointHandle) {}

void SimpleWorld::Step(float dt) {
    // Фиксированный шаг 1/120 c аккумулятором — стабильнее переменного dt.
    const float fixed = 1.0f / 120.0f;
    m_accum += std::min(dt, 0.1f); // защита от «спирали смерти» при лагах
    int guard = 0;
    while (m_accum >= fixed && guard++ < 16) {
        SubStep(fixed);
        m_accum -= fixed;
    }
}

void SimpleWorld::SubStep(float dt) {
    // 1. Интегрируем скорость/позицию динамических тел.
    for (auto& [h, b] : m_bodies) {
        if (b.Type != BodyType::Dynamic) continue;
        b.Velocity += m_gravity * dt;
        b.Position += b.Velocity * dt;
    }

    // 2. Разрешаем столкновения динамики со статикой/кинематикой (AABB).
    for (auto& [hd, d] : m_bodies) {
        if (d.Type != BodyType::Dynamic) continue;
        for (auto& [hs, s] : m_bodies) {
            if (&s == &d || s.Type == BodyType::Dynamic) continue; // только vs static/kinematic

            glm::vec3 delta = d.Position - s.Position;
            glm::vec3 sum = d.Half + s.Half;
            glm::vec3 overlap = sum - glm::abs(delta);
            if (overlap.x <= 0.0f || overlap.y <= 0.0f || overlap.z <= 0.0f) {
                // Разошлись — если пара касалась, это событие «вышел».
                NoteSeparated(hd, hs);
                continue;
            }
            NoteTouching(hd, hs, d, s);
            // Сенсор обнаруживает касание, но не отталкивает: он зона, а не стена.
            if (s.Sensor || d.Sensor) continue;

            // Ось минимального проникновения — выталкиваем по ней.
            int axis = 0;
            if (overlap.y < overlap.x) axis = 1;
            if (overlap.z < overlap[axis]) axis = 2;
            float sign = delta[axis] >= 0.0f ? 1.0f : -1.0f;

            d.Position[axis] += overlap[axis] * sign; // вытолкнуть из препятствия

            // Гасим скорость вдоль оси контакта (с упругостью), только если
            // тело двигалось В препятствие.
            if (d.Velocity[axis] * sign < 0.0f) {
                d.Velocity[axis] = -d.Velocity[axis] * d.Restitution;
                // Трение по касательным осям.
                float keep = std::clamp(1.0f - d.Friction, 0.0f, 1.0f);
                for (int t = 0; t < 3; ++t) if (t != axis) d.Velocity[t] *= keep;
            }
        }
    }
}


// --- Запросы и события --------------------------------------------------------

// Луч против AABB (slab-метод). Форма в этом бэкенде и так приближается
// коробкой, так что луч ровно настолько же точен, насколько и вся его физика —
// и это честнее, чем аккуратный луч по коробке, которая всё равно неточна.
static bool RayVsAabb(const glm::vec3& o, const glm::vec3& d, const glm::vec3& mn,
                      const glm::vec3& mx, float maxT, float& outT, glm::vec3& outN) {
    float tmin = 0.0f, tmax = maxT;
    int axis = 0;
    float sign = 1.0f;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-8f) {
            // Луч параллелен паре плоскостей: либо внутри полосы, либо мимо.
            if (o[i] < mn[i] || o[i] > mx[i]) return false;
            continue;
        }
        const float inv = 1.0f / d[i];
        float t1 = (mn[i] - o[i]) * inv;
        float t2 = (mx[i] - o[i]) * inv;
        float s = -1.0f;
        if (t1 > t2) { std::swap(t1, t2); s = 1.0f; }
        if (t1 > tmin) { tmin = t1; axis = i; sign = s; }
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return false;
    }
    outT = tmin;
    outN = glm::vec3(0.0f);
    outN[axis] = sign;
    return true;
}

bool SimpleWorld::Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                          RayHit& out, LayerMask mask) const {
    out = RayHit{};
    const float len = glm::length(direction);
    if (len < 1e-6f || maxDistance <= 0.0f) return false;
    const glm::vec3 dir = direction / len;

    float best = maxDistance;
    for (const auto& [h, b] : m_bodies) {
        if (!b.Alive || (b.Layer & mask) == 0) continue;
        float t = 0.0f;
        glm::vec3 n(0.0f);
        if (!RayVsAabb(origin, dir, b.Position - b.Half, b.Position + b.Half, best, t, n)) continue;
        if (t >= best) continue;
        best = t;
        out.Hit = true;
        out.Body = h;
        out.Point = origin + dir * t;
        out.Normal = n;
        out.Distance = t;
    }
    return out.Hit;
}

int SimpleWorld::OverlapSphere(const glm::vec3& center, float radius, std::vector<BodyHandle>& out,
                               LayerMask mask) const {
    out.clear();
    if (radius <= 0.0f) return 0;
    for (const auto& [h, b] : m_bodies) {
        if (!b.Alive || (b.Layer & mask) == 0) continue;
        // Ближайшая точка коробки к центру сферы — классическая проверка
        // сфера-AABB без корней до самого конца.
        const glm::vec3 closest = glm::clamp(center, b.Position - b.Half, b.Position + b.Half);
        const glm::vec3 d2 = closest - center;
        if (glm::dot(d2, d2) <= radius * radius) out.push_back(h);
    }
    return (int)out.size();
}

void SimpleWorld::NoteTouching(BodyHandle a, BodyHandle b, const Body& ba, const Body& bb) {
    const auto key = std::minmax(a, b);
    if (!m_touching.insert(key).second) return;   // уже касались — не событие
    ContactEvent e;
    e.When = ContactEvent::Phase::Begin;
    e.A = a;
    e.B = b;
    e.Point = (ba.Position + bb.Position) * 0.5f;
    e.Normal = glm::normalize(bb.Position - ba.Position + glm::vec3(1e-6f));
    e.Sensor = ba.Sensor || bb.Sensor;
    e.Impulse = e.Sensor ? 0.0f : glm::length(ba.Velocity - bb.Velocity);
    m_events.push_back(e);
}

void SimpleWorld::NoteSeparated(BodyHandle a, BodyHandle b) {
    const auto key = std::minmax(a, b);
    if (m_touching.erase(key) == 0) return;
    ContactEvent e;
    e.When = ContactEvent::Phase::End;
    e.A = a;
    e.B = b;
    m_events.push_back(e);
}

void SimpleWorld::PollContacts(std::vector<ContactEvent>& out) {
    out.swap(m_events);
    m_events.clear();
}

void SimpleWorld::GetBodyTransform(BodyHandle body, glm::vec3& position, glm::quat& rotation) const {
    auto it = m_bodies.find(body);
    if (it != m_bodies.end()) { position = it->second.Position; rotation = it->second.Rotation; }
}

void SimpleWorld::SetBodyTransform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end()) return;
    it->second.Position = position;
    it->second.Rotation = rotation;
    if (it->second.Type != BodyType::Dynamic) it->second.Velocity = glm::vec3(0.0f);
}

void SimpleWorld::SetLinearVelocity(BodyHandle body, const glm::vec3& velocity) {
    auto it = m_bodies.find(body);
    if (it != m_bodies.end()) it->second.Velocity = velocity;
}

glm::vec3 SimpleWorld::GetLinearVelocity(BodyHandle body) const {
    auto it = m_bodies.find(body);
    return it != m_bodies.end() ? it->second.Velocity : glm::vec3(0.0f);
}

} // namespace sage::physics
