#include "physics/jolt/JoltWorld.h"

#include <algorithm>

// Jolt требует, чтобы его собственный зонтичный заголовок включался ПЕРВЫМ.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <mutex>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>

#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <thread>

#include "sage/core/Log.h"

using namespace sage::physics;

#include "physics/jolt/JoltConvert.h"

namespace sage::physics {

using namespace jolt_detail;

void JoltWorld::GetBodyTransform(BodyHandle body, glm::vec3& position, glm::quat& rotation) const {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    JPH::BodyID id(it->second);
    const JPH::BodyInterface& bi = m_system->GetBodyInterface();
    position = FromJolt(bi.GetPosition(id));
    rotation = FromJoltQuat(bi.GetRotation(id));
}

void JoltWorld::SetBodyTransform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    JPH::BodyID id(it->second);
    JPH::BodyInterface& bi = m_system->GetBodyInterface();
    // Кинематику ведём «мягко»: цель запоминается и проходится по подшагам
    // следующего Step со скоростью, которую Jolt передаёт тем, кого она
    // толкает. Телепорт кинематики сквозь ящик выбил бы его без всякого
    // трения. Прочие тела переставляем жёстко.
    if (bi.GetMotionType(id) == JPH::EMotionType::Kinematic) {
        m_kinTargets[body] = KinematicTarget{position, rotation};
    } else {
        bi.SetPositionAndRotation(id, ToJolt(position), ToJoltQuat(rotation), JPH::EActivation::Activate);
    }
}

void JoltWorld::SetLinearVelocity(BodyHandle body, const glm::vec3& velocity) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    JPH::BodyID id(it->second);
    m_system->GetBodyInterface().SetLinearVelocity(id, JPH::Vec3(velocity.x, velocity.y, velocity.z));
}

glm::vec3 JoltWorld::GetLinearVelocity(BodyHandle body) const {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return glm::vec3(0.0f);
    JPH::BodyID id(it->second);
    return FromJolt(m_system->GetBodyInterface().GetLinearVelocity(id));
}

void JoltWorld::AddImpulse(BodyHandle body, const glm::vec3& impulse) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    JPH::BodyID id(it->second);
    m_system->GetBodyInterface().AddImpulse(id, JPH::Vec3(impulse.x, impulse.y, impulse.z));
}

void JoltWorld::SetAngularVelocity(BodyHandle body, const glm::vec3& w) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    JPH::BodyInterface& bi = m_system->GetBodyInterface();
    const JPH::BodyID id(it->second);
    if (bi.GetMotionType(id) == JPH::EMotionType::Static) return;
    bi.SetAngularVelocity(id, JPH::Vec3(w.x, w.y, w.z));
    // Спящее тело скорость хранит, но не движется — будим, как и при толчке.
    bi.ActivateBody(id);
}

glm::vec3 JoltWorld::GetAngularVelocity(BodyHandle body) const {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return glm::vec3(0.0f);
    return FromJolt(m_system->GetBodyInterface().GetAngularVelocity(JPH::BodyID(it->second)));
}

void JoltWorld::AddImpulseAtPoint(BodyHandle body, const glm::vec3& impulse, const glm::vec3& point) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    m_system->GetBodyInterface().AddImpulse(JPH::BodyID(it->second), JPH::Vec3(impulse.x, impulse.y, impulse.z),
                                            ToJolt(point));
}

void JoltWorld::AddAngularImpulse(BodyHandle body, const glm::vec3& impulse) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    m_system->GetBodyInterface().AddAngularImpulse(JPH::BodyID(it->second),
                                                   JPH::Vec3(impulse.x, impulse.y, impulse.z));
}

void JoltWorld::AddForce(BodyHandle body, const glm::vec3& force) {
    if (!m_bodies.count(body)) return;
    m_forces[body].Force += force;
}

void JoltWorld::AddForceAtPoint(BodyHandle body, const glm::vec3& force, const glm::vec3& point) {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return;
    // Сила в точке = та же сила в центр масс + момент плеча. Плечо снимаем
    // сейчас: за один кадр тело поворачивается на доли градуса.
    const glm::vec3 com =
        FromJolt(m_system->GetBodyInterface().GetCenterOfMassPosition(JPH::BodyID(it->second)));
    PendingForce& f = m_forces[body];
    f.Force += force;
    f.Torque += glm::cross(point - com, force);
}

void JoltWorld::AddTorque(BodyHandle body, const glm::vec3& torque) {
    if (!m_bodies.count(body)) return;
    m_forces[body].Torque += torque;
}

bool JoltWorld::IsSleeping(BodyHandle body) const {
    auto it = m_bodies.find(body);
    if (it == m_bodies.end() || !m_system) return false;
    const JPH::BodyInterface& bi = m_system->GetBodyInterface();
    const JPH::BodyID id(it->second);
    return bi.GetMotionType(id) == JPH::EMotionType::Dynamic && !bi.IsActive(id);
}

JointHandle JoltWorld::CreateJoint(const JointDesc& desc) {
    if (!m_system) return kInvalidJoint;

    // Тело A обязательно; тело B == invalid -> крепим к неподвижному «миру».
    auto ia = m_bodies.find(desc.BodyA);
    if (ia == m_bodies.end()) return kInvalidJoint;

    const JPH::BodyLockInterface& lock = m_system->GetBodyLockInterfaceNoLock();
    JPH::Body* a = lock.TryGetBody(JPH::BodyID(ia->second));
    if (!a) return kInvalidJoint;

    JPH::Body* b = &JPH::Body::sFixedToWorld;
    if (desc.BodyB != kInvalidBody) {
        auto ib = m_bodies.find(desc.BodyB);
        if (ib == m_bodies.end()) return kInvalidJoint;
        b = lock.TryGetBody(JPH::BodyID(ib->second));
        if (!b) return kInvalidJoint;
    }

    JPH::RVec3 anchor = ToJolt(desc.Anchor);
    JPH::Vec3 axis = JPH::Vec3(desc.Axis.x, desc.Axis.y, desc.Axis.z).NormalizedOr(JPH::Vec3::sAxisY());

    JPH::Constraint* constraint = nullptr;
    switch (desc.Type) {
        case JointType::Fixed: {
            JPH::FixedConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mAutoDetectPoint = true; // фиксируем текущее взаимное положение тел
            constraint = s.Create(*a, *b);
            break;
        }
        case JointType::Point: {
            JPH::PointConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = s.mPoint2 = anchor;
            constraint = s.Create(*a, *b);
            break;
        }
        case JointType::Hinge: {
            JPH::HingeConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = s.mPoint2 = anchor;
            s.mHingeAxis1 = s.mHingeAxis2 = axis;
            JPH::Vec3 normal = PerpendicularTo(axis);
            s.mNormalAxis1 = s.mNormalAxis2 = normal;
            if (desc.UseLimits) {
                s.mLimitsMin = glm::radians(desc.MinLimit);
                s.mLimitsMax = glm::radians(desc.MaxLimit);
            }
            constraint = s.Create(*a, *b);
            break;
        }
        case JointType::Slider: {
            JPH::SliderConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = s.mPoint2 = anchor;
            s.SetSliderAxis(axis); // выставляет ось скольжения + нормали
            if (desc.UseLimits) {
                s.mLimitsMin = desc.MinLimit;
                s.mLimitsMax = desc.MaxLimit;
            }
            constraint = s.Create(*a, *b);
            break;
        }
        case JointType::Distance: {
            // Трос: точка крепления — на ВТОРОМ теле (крюк, столб, мир), а
            // первое тело держится за свой ЦЕНТР. Крепить оба конца к одной
            // точке, как у остальных соединений, здесь нельзя: длина между
            // ними тогда равна нулю, ограничение расталкивает тела в
            // произвольную сторону, а тело уезжает на длину троса плюс вылет
            // своего крепления — маятник ложится на пол вместо того, чтобы
            // качаться.
            JPH::DistanceConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = a->GetCenterOfMassPosition();
            s.mPoint2 = anchor;
            s.mMinDistance = desc.MinDistance;
            s.mMaxDistance = desc.MaxDistance;
            constraint = s.Create(*a, *b);
            break;
        }
        case JointType::Cone: {
            JPH::ConeConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = s.mPoint2 = anchor;
            s.mTwistAxis1 = s.mTwistAxis2 = axis;
            s.mHalfConeAngle = glm::radians(glm::clamp(desc.ConeHalfAngle, 0.0f, 180.0f));
            constraint = s.Create(*a, *b);
            break;
        }
    }
    if (!constraint) return kInvalidJoint;

    constraint->AddRef(); // держим ссылку сами (карта m_joints) — иначе удалится
    m_system->AddConstraint(constraint);
    JointHandle h = m_nextJoint++;
    m_joints[h] = constraint;
    return h;
}

void JoltWorld::ReleaseConstraint(JPH::Constraint* constraint) {
    // Тела соединения будим: улёгшийся маятник спит, и без этого груз, у
    // которого перерезали трос, так и висел бы в воздухе — снятие соединения
    // Jolt сном не считает.
    const auto* c = static_cast<const JPH::TwoBodyConstraint*>(constraint);
    JPH::BodyInterface& bi = m_system->GetBodyInterfaceNoLock();
    for (const JPH::Body* b : {c->GetBody1(), c->GetBody2()})
        if (b && !b->IsStatic() && b->IsInBroadPhase()) bi.ActivateBody(b->GetID());
    m_system->RemoveConstraint(constraint);
    constraint->Release();
}

void JoltWorld::RemoveJoint(JointHandle joint) {
    auto it = m_joints.find(joint);
    if (it == m_joints.end() || !m_system) return;
    ReleaseConstraint(it->second);
    m_joints.erase(it);
}

} // namespace sage::physics
