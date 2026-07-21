#include "sage/physics/PhysicsScene.h"

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>

#include "sage/core/Log.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"

using namespace sage::physics;

namespace {

// Transform хранит поворот углами Эйлера (градусы, порядок XYZ — как в
// Transform::GetMatrix). Физика работает с кватернионами — конвертируем через
// матрицу, чтобы порядок совпадал везде в движке.
glm::quat EulerToQuat(const glm::vec3& eulerDeg) {
    glm::mat4 m = glm::eulerAngleXYZ(glm::radians(eulerDeg.x),
                                     glm::radians(eulerDeg.y),
                                     glm::radians(eulerDeg.z));
    return glm::quat_cast(m);
}

glm::vec3 QuatToEuler(const glm::quat& q) {
    glm::mat4 m = glm::mat4_cast(q);
    float x, y, z;
    glm::extractEulerAngleXYZ(m, x, y, z);
    return glm::degrees(glm::vec3(x, y, z));
}

BodyDesc DescFromEntity(const RigidBodyComponent& rb, const ColliderComponent* col,
                        const Transform& tr) {
    BodyDesc d;
    d.Type = rb.Type;
    d.Mass = rb.Mass;
    d.Friction = rb.Friction;
    d.Restitution = rb.Restitution;
    d.Position = tr.Position;
    d.Rotation = EulerToQuat(tr.Rotation);

    glm::vec3 scale = glm::abs(tr.Scale);
    float uniform = glm::max(scale.x, glm::max(scale.y, scale.z));
    if (col && !col->Parts.empty()) {
        // Составная форма: каждая дочерняя часть масштабируется как одиночная.
        for (const ColliderComponent::Part& p : col->Parts) {
            ChildShape cs;
            cs.Shape = p.Shape;
            cs.HalfExtents = p.HalfExtents * scale;
            cs.Radius = p.Radius * uniform;
            cs.HalfHeight = p.HalfHeight * scale.y;
            cs.Position = p.Offset * scale;
            cs.Rotation = EulerToQuat(p.EulerDeg);
            d.Children.push_back(cs);
        }
    } else if (col) {
        d.Shape = col->Shape;
        d.HalfExtents = col->HalfExtents * scale;              // Box масштабируется по осям
        d.Radius = col->Radius * uniform;                     // равномерно
        d.HalfHeight = col->HalfHeight * scale.y;
    } else {
        // Нет коллайдера — единичный бокс по масштабу сущности.
        d.Shape = ShapeType::Box;
        d.HalfExtents = 0.5f * scale;
    }
    return d;
}

} // namespace

PhysicsScene::PhysicsScene(Backend backend, Scene& scene) {
    m_world = PhysicsWorld::Create(backend);
    m_world->SetGravity({0.0f, -9.81f, 0.0f});

    auto view = scene.Registry().view<RigidBodyComponent, Transform>();
    for (auto e : view) {
        RigidBodyComponent& rb = view.get<RigidBodyComponent>(e);
        const Transform& tr = view.get<Transform>(e);
        const ColliderComponent* col = scene.Registry().try_get<ColliderComponent>(e);
        rb.RuntimeBody = m_world->CreateBody(DescFromEntity(rb, col, tr));
        ++m_bodyCount;
    }

    // Второй проход — соединения (все тела уже созданы, партнёров можно
    // резолвить по id). Требует RigidBodyComponent у самой сущности (BodyA).
    auto joints = scene.Registry().view<JointComponent, RigidBodyComponent, Transform>();
    for (auto e : joints) {
        JointComponent& jc = joints.get<JointComponent>(e);
        const RigidBodyComponent& rb = joints.get<RigidBodyComponent>(e);
        const Transform& tr = joints.get<Transform>(e);
        if (rb.RuntimeBody == kInvalidBody) continue;

        JointDesc jd;
        jd.Type = jc.Type;
        jd.BodyA = rb.RuntimeBody;
        jd.BodyB = kInvalidBody;
        if (jc.TargetId >= 0) {
            GameObject target = scene.Get(jc.TargetId);
            if (target.Valid()) {
                const RigidBodyComponent* trb =
                    scene.Registry().try_get<RigidBodyComponent>(target.Entity());
                if (trb) jd.BodyB = trb->RuntimeBody;
            }
        }
        jd.Anchor = tr.Position + jc.Anchor; // мировая точка крепления
        jd.Axis = jc.Axis;
        jd.UseLimits = jc.UseLimits;
        jd.MinLimit = jc.MinLimit;
        jd.MaxLimit = jc.MaxLimit;
        jd.MinDistance = jc.MinDistance;
        jd.MaxDistance = jc.MaxDistance;
        jd.ConeHalfAngle = jc.ConeHalfAngle;

        jc.RuntimeJoint = m_world->CreateJoint(jd);
        if (jc.RuntimeJoint != kInvalidJoint) ++m_jointCount;
    }

    LOG_INFO("Physics") << "PhysicsScene: бэкенд " << m_world->BackendName()
                        << ", тел " << m_bodyCount << ", соединений " << m_jointCount
                        << (m_world->IsAvailable() ? "" : " (симуляция отключена)");
}

void PhysicsScene::Step(Scene& scene, float dt) {
    if (!m_world || !m_world->IsAvailable()) return;

    // Кинематика: до шага толкаем тела за Transform сущности (её ведёт скрипт).
    auto view = scene.Registry().view<RigidBodyComponent, Transform>();
    for (auto e : view) {
        RigidBodyComponent& rb = view.get<RigidBodyComponent>(e);
        if (rb.Type != BodyType::Kinematic || rb.RuntimeBody == kInvalidBody) continue;
        const Transform& tr = view.get<Transform>(e);
        m_world->SetBodyTransform(rb.RuntimeBody, tr.Position, EulerToQuat(tr.Rotation));
    }

    m_world->Step(dt);

    // События контактов бэкенда -> пары id сущностей (для скриптов).
    m_collisions.clear();
    for (const ContactEvent& ev : m_world->DrainContactEvents()) {
        int a = EntityForBody(scene, ev.A);
        int b = EntityForBody(scene, ev.B);
        if (a < 0 || b < 0) continue; // тело без сущности (уже удалена и т.п.)
        m_collisions.push_back({a, b, ev.Began});
    }

    // После шага: позиции динамических тел -> обратно в Transform сущностей.
    for (auto e : view) {
        RigidBodyComponent& rb = view.get<RigidBodyComponent>(e);
        if (rb.Type != BodyType::Dynamic || rb.RuntimeBody == kInvalidBody) continue;
        Transform& tr = view.get<Transform>(e);
        glm::vec3 pos;
        glm::quat rot;
        m_world->GetBodyTransform(rb.RuntimeBody, pos, rot);
        tr.Position = pos;
        tr.Rotation = QuatToEuler(rot);
    }
}

int PhysicsScene::EntityForBody(Scene& scene, BodyHandle body) const {
    if (body == kInvalidBody) return -1;
    auto view = scene.Registry().view<RigidBodyComponent, IdComponent>();
    for (auto e : view) {
        if (view.get<RigidBodyComponent>(e).RuntimeBody == body)
            return view.get<IdComponent>(e).Id;
    }
    return -1;
}

SceneRayHit PhysicsScene::Raycast(Scene& scene, const glm::vec3& origin, const glm::vec3& dir,
                                  float maxDist) const {
    SceneRayHit out;
    if (!m_world || !m_world->IsAvailable()) return out;
    RayHitInfo hit = m_world->Raycast(origin, dir, maxDist);
    if (!hit.Hit) return out;
    out.Hit = true;
    out.Position = hit.Position;
    out.Normal = hit.Normal;
    out.Distance = hit.Distance;
    out.EntityId = EntityForBody(scene, hit.Body);
    return out;
}
