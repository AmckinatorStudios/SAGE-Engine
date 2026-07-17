#include "physics/simple/SimpleWorld.h"

#include <algorithm>
#include <cmath>

namespace sage::physics {

namespace {
// AABB-полуразмеры по форме (сфера/капсула приближаются боксом).
glm::vec3 HalfFromDesc(const BodyDesc& d) {
    switch (d.Shape) {
        case ShapeType::Sphere:  return glm::vec3(d.Radius);
        case ShapeType::Capsule: return glm::vec3(d.Radius, d.HalfHeight + d.Radius, d.Radius);
        default:                 return d.HalfExtents; // Box
    }
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
    BodyHandle h = m_next++;
    m_bodies[h] = b;
    return h;
}

void SimpleWorld::RemoveBody(BodyHandle body) { m_bodies.erase(body); }

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
            if (overlap.x <= 0.0f || overlap.y <= 0.0f || overlap.z <= 0.0f) continue;

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
