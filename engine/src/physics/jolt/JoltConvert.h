#pragma once
// Jolt требует, чтобы его зонтичный заголовок включался ПЕРВЫМ.
#include <Jolt/Jolt.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Перевод векторов и поворотов между glm и Jolt — общий для мира
// (JoltWorld.cpp) и тел (JoltWorld_Bodies.cpp).
namespace sage::physics::jolt_detail {

inline JPH::RVec3 ToJolt(const glm::vec3& v) { return JPH::RVec3(v.x, v.y, v.z); }

inline JPH::Quat ToJoltQuat(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }

// В одинарной точности RVec3 == Vec3 — один перегруз покрывает оба (GetPosition
// возвращает RVec3, GetLinearVelocity — Vec3).
inline glm::vec3 FromJolt(const JPH::Vec3& v) { return glm::vec3(v.GetX(), v.GetY(), v.GetZ()); }

inline glm::quat FromJoltQuat(const JPH::Quat& q) { return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); }

// Перпендикуляр к оси (для рамок Hinge/Slider, которым нужна нормаль ⟂ оси).
inline JPH::Vec3 PerpendicularTo(const JPH::Vec3& axis) {
    JPH::Vec3 a = axis.NormalizedOr(JPH::Vec3::sAxisY());
    JPH::Vec3 ref = std::abs(a.GetX()) < 0.9f ? JPH::Vec3::sAxisX() : JPH::Vec3::sAxisY();
    return a.Cross(ref).NormalizedOr(JPH::Vec3::sAxisZ());
}

} // namespace sage::physics::jolt_detail
