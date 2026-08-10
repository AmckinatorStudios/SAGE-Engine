#include "sage/physics/PhysicsScene.h"

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtc/matrix_transform.hpp>

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

// Мировой Transform сущности: позиция, поворот и масштаб, УЖЕ учитывающие
// родителей.
//
// Физика раньше брала ЛОКАЛЬНЫЙ Transform, и это работало ровно до первой
// сущности с родителем. У корневой локальное и мировое совпадают, поэтому
// ошибка была невидима во всех тестах и во всех сценах, где тела висят на
// корне. А стоило прицепить объект к другому — и тело оказывалось не там, где
// объект: на экране всё правильно (рендер-то мировую матрицу учитывает), а
// столкновения происходят в стороне, будто коллизия «не поворачивается вместе
// с объектом». Найти это по симптому почти невозможно.
struct WorldTransform {
    glm::vec3 Position{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 Scale{1.0f};
};

WorldTransform DecomposeWorld(const glm::mat4& m) {
    WorldTransform out;
    out.Position = glm::vec3(m[3]);
    // Масштаб — длины столбцов; поворот — те же столбцы, нормированные. Полный
    // glm::decompose здесь избыточен: скос физике всё равно не передать, у формы
    // столкновения его нет по определению.
    glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    out.Scale = {glm::length(c0), glm::length(c1), glm::length(c2)};
    if (out.Scale.x > 1e-6f) c0 /= out.Scale.x;
    if (out.Scale.y > 1e-6f) c1 /= out.Scale.y;
    if (out.Scale.z > 1e-6f) c2 /= out.Scale.z;
    glm::mat3 rot(c0, c1, c2);
    out.Rotation = glm::quat_cast(rot);
    return out;
}

// Мировая матрица РОДИТЕЛЯ (единичная, если родителя нет). Нужна для обратного
// перевода: физика отвечает в мировых координатах, Transform хранит локальные.
glm::mat4 ParentWorldMatrix(const Scene& scene, entt::entity e) {
    const HierarchyComponent* h = scene.Registry().try_get<HierarchyComponent>(e);
    if (!h || h->Parent == entt::null || !scene.Registry().valid(h->Parent)) return glm::mat4(1.0f);
    return scene.WorldMatrix(h->Parent);
}

BodyDesc DescFromEntity(const RigidBodyComponent& rb, const ColliderComponent* col,
                        const WorldTransform& tr) {
    BodyDesc d;
    d.Type = rb.Type;
    d.Mass = rb.Mass;
    d.Friction = rb.Friction;
    d.Restitution = rb.Restitution;
    d.Layer = rb.Layer;
    d.Sensor = rb.Sensor;
    d.Position = tr.Position;
    d.Rotation = tr.Rotation;

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

    // Хэндлы прошлой симуляции сбрасываем: сцена могла уже играться (Play ->
    // Stop -> Play), и в компонентах остались указатели на тела УМЕРШЕГО мира.
    // SyncBodies опознаёт «нет тела» именно по kInvalidBody, так что без этой
    // чистки второй запуск молча остался бы вообще без физики.
    for (auto e : scene.Registry().view<RigidBodyComponent>()) {
        scene.Registry().get<RigidBodyComponent>(e).RuntimeBody = kInvalidBody;
    }
    SyncBodies(scene);

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

void PhysicsScene::SyncBodies(Scene& scene) {
    if (!m_world) return;

    // 1. Новые сущности с RigidBodyComponent, но без тела. Признак «нет тела» —
    // сам RuntimeBody: сущность, пришедшая из сериализатора или порождённая
    // скриптом, несёт kInvalidBody, и другого маркера не требуется.
    auto view = scene.Registry().view<RigidBodyComponent, Transform>();
    for (auto e : view) {
        RigidBodyComponent& rb = view.get<RigidBodyComponent>(e);
        if (rb.RuntimeBody != kInvalidBody) continue;
        const WorldTransform tr = DecomposeWorld(scene.WorldMatrix(e));
        const ColliderComponent* col = scene.Registry().try_get<ColliderComponent>(e);
        rb.RuntimeBody = m_world->CreateBody(DescFromEntity(rb, col, tr));
        if (rb.RuntimeBody == kInvalidBody) continue; // Null-бэкенд: тел не бывает
        m_tracked.emplace_back(e, rb.RuntimeBody);
        m_bodyToEntity[rb.RuntimeBody] = e;
        ++m_bodyCount;
    }

    // 2. Осиротевшие тела: сущность уничтожена или с неё сняли компонент.
    // Без этого мир копил бы невидимые тела уничтоженных объектов — игра,
    // которая порождает и убирает предметы каждую секунду, за минуту набивала
    // бы физический мир мусором, с которым продолжали бы сталкиваться живые.
    size_t alive = 0;
    for (size_t i = 0; i < m_tracked.size(); ++i) {
        const auto& [entity, body] = m_tracked[i];
        const RigidBodyComponent* rb = scene.Registry().valid(entity)
            ? scene.Registry().try_get<RigidBodyComponent>(entity) : nullptr;
        if (rb && rb->RuntimeBody == body) {
            m_tracked[alive++] = m_tracked[i];
        } else {
            m_world->RemoveBody(body);
            m_bodyToEntity.erase(body);
            --m_bodyCount;
        }
    }
    m_tracked.resize(alive);
}

void PhysicsScene::Step(Scene& scene, float dt) {
    if (!m_world || !m_world->IsAvailable()) return;

    // Состав мира мог измениться с прошлого кадра (скрипт спавнит и удаляет).
    SyncBodies(scene);
    SyncCharacters(scene);

    // Кинематика: до шага толкаем тела за Transform сущности (её ведёт скрипт).
    auto view = scene.Registry().view<RigidBodyComponent, Transform>();
    for (auto e : view) {
        RigidBodyComponent& rb = view.get<RigidBodyComponent>(e);
        if (rb.Type != BodyType::Kinematic || rb.RuntimeBody == kInvalidBody) continue;
        const WorldTransform tr = DecomposeWorld(scene.WorldMatrix(e));
        m_world->SetBodyTransform(rb.RuntimeBody, tr.Position, tr.Rotation);
    }

    m_world->Step(dt);

    // После шага: позиции динамических тел -> обратно в Transform сущностей.
    for (auto e : view) {
        RigidBodyComponent& rb = view.get<RigidBodyComponent>(e);
        if (rb.Type != BodyType::Dynamic || rb.RuntimeBody == kInvalidBody) continue;
        Transform& tr = view.get<Transform>(e);
        glm::vec3 pos;
        glm::quat rot;
        m_world->GetBodyTransform(rb.RuntimeBody, pos, rot);

        // Физика говорит в МИРОВЫХ координатах, а Transform хранит ЛОКАЛЬНЫЕ.
        // У корневой сущности это одно и то же, поэтому раньше разницы никто не
        // замечал; у ребёнка запись мировой позиции в локальное поле означала
        // бы, что объект каждый кадр уезжает на смещение родителя — и чем
        // дальше родитель от начала координат, тем быстрее.
        const glm::mat4 parent = ParentWorldMatrix(scene, e);
        if (parent == glm::mat4(1.0f)) {
            tr.Position = pos;
            tr.Rotation = QuatToEuler(rot);
        } else {
            const glm::mat4 world = glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot);
            const WorldTransform local = DecomposeWorld(glm::inverse(parent) * world);
            tr.Position = local.Position;
            tr.Rotation = QuatToEuler(local.Rotation);
        }
    }

    PullCharacters(scene);

    // События столкновений: забираем накопленное за шаг и переводим в сущности.
    // Именно здесь, а не по запросу: очередь бэкенда одна, и кто прочитал её
    // первым, тот и забрал события у всех остальных. Список живёт до
    // следующего Step — читать его можно откуда угодно и сколько угодно раз.
    m_contacts.clear();
    if (m_world->SupportsContacts()) {
        m_world->PollContacts(m_rawContacts);
        m_contacts.reserve(m_rawContacts.size());
        for (const ContactEvent& raw : m_rawContacts) {
            auto ia = m_bodyToEntity.find(raw.A);
            auto ib = m_bodyToEntity.find(raw.B);
            // Тело без сущности — не ошибка: сущность могли удалить в этом же
            // кадре, и событие про неё игре уже не адресовать.
            if (ia == m_bodyToEntity.end() || ib == m_bodyToEntity.end()) continue;
            EntityContact c;
            c.Begin = raw.When == ContactEvent::Phase::Begin;
            c.A = ia->second;
            c.B = ib->second;
            c.Point = raw.Point;
            c.Normal = raw.Normal;
            c.Impulse = raw.Impulse;
            c.Sensor = raw.Sensor;
            m_contacts.push_back(c);
        }
    }
}

// Заводит контроллеры для сущностей, у которых есть компонент, но ещё нет
// контроллера, и снимает осиротевшие — ровно как SyncBodies для тел.
void PhysicsScene::SyncCharacters(Scene& scene) {
    if (!m_world || !m_world->SupportsCharacters()) return;

    auto view = scene.Registry().view<CharacterControllerComponent, Transform>();
    for (auto e : view) {
        CharacterControllerComponent& cc = view.get<CharacterControllerComponent>(e);
        if (cc.Runtime != kInvalidCharacter) continue;
        // У персонажа СВОЙ мир (игра сама отвечает, где твердь) — физике он не
        // принадлежит, и заводить ему тело в бэкенде значило бы столкнуть два
        // разных представления мира в одном теле.
        if (cc.Solid) continue;
        const Transform& tr = view.get<Transform>(e);
        CharacterDesc d;
        d.Radius = cc.Radius;
        d.Height = cc.Height;
        d.StepHeight = cc.StepHeight;
        d.MaxSlopeDeg = cc.MaxSlopeDeg;
        d.Mass = cc.Mass;
        d.Layer = cc.Layer;
        d.Position = tr.Position;
        cc.Runtime = m_world->CreateCharacter(d);
        if (cc.Runtime != kInvalidCharacter) m_characters.emplace_back(e, cc.Runtime);
    }

    size_t alive = 0;
    for (size_t i = 0; i < m_characters.size(); ++i) {
        const auto& [entity, handle] = m_characters[i];
        const CharacterControllerComponent* cc =
            scene.Registry().valid(entity)
                ? scene.Registry().try_get<CharacterControllerComponent>(entity)
                : nullptr;
        if (cc && cc->Runtime == handle) m_characters[alive++] = m_characters[i];
        else m_world->RemoveCharacter(handle);
    }
    m_characters.resize(alive);
}

// Переносит положение контроллеров в Transform сущностей и обновляет флаг
// опоры. После шага: до него положение ведёт игра (через MoveCharacter), после
// — физика, и путать эти два момента значит терять то одно, то другое.
void PhysicsScene::PullCharacters(Scene& scene) {
    if (!m_world || !m_world->SupportsCharacters()) return;
    for (const auto& [entity, handle] : m_characters) {
        if (!scene.Registry().valid(entity)) continue;
        auto* cc = scene.Registry().try_get<CharacterControllerComponent>(entity);
        auto* tr = scene.Registry().try_get<Transform>(entity);
        if (!cc || !tr) continue;
        const sage::physics::CharacterState st = m_world->GetCharacterState(handle);
        tr->Position = st.Position;
        cc->Grounded = st.Grounded;
        cc->GroundNormal = st.GroundNormal;
    }
}

PhysicsScene::EntityHit PhysicsScene::Raycast(const glm::vec3& origin, const glm::vec3& direction,
                                              float maxDistance, LayerMask mask) const {
    EntityHit out;
    if (!m_world) return out;
    RayHit hit;
    if (!m_world->Raycast(origin, direction, maxDistance, hit, mask)) return out;
    auto it = m_bodyToEntity.find(hit.Body);
    if (it == m_bodyToEntity.end()) return out;   // тело без сущности игре не адресовать
    out.Hit = true;
    out.Entity = it->second;
    out.Point = hit.Point;
    out.Normal = hit.Normal;
    out.Distance = hit.Distance;
    return out;
}

int PhysicsScene::OverlapSphere(const glm::vec3& center, float radius,
                                std::vector<entt::entity>& out, LayerMask mask) const {
    out.clear();
    if (!m_world) return 0;
    std::vector<BodyHandle> bodies;
    m_world->OverlapSphere(center, radius, bodies, mask);
    for (BodyHandle b : bodies) {
        auto it = m_bodyToEntity.find(b);
        if (it != m_bodyToEntity.end()) out.push_back(it->second);
    }
    return (int)out.size();
}
