#include "sage/physics/PhysicsScene.h"

#include <algorithm>

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

// --- Геометрия зон для персонажей ---------------------------------------------
//
// Персонаж — не тело мира, и бэкенд о его входе в зону не сообщает. Поэтому
// касание «капсула персонажа — форма зоны» считается здесь. Расстояние от
// точки до выпуклой формы — выпуклая функция, и вдоль отрезка оси капсулы у
// неё один минимум: троичный поиск находит его точно, без особых случаев
// «отрезок против коробки» на каждую пару форм.

// Расстояние от точки (в ЛОКАЛЬНЫХ осях формы, капсула — вдоль Y) до формы;
// 0 — точка внутри.
float DistToShape(const glm::vec3& p, ShapeType shape, const glm::vec3& half, float radius,
                  float halfHeight) {
    switch (shape) {
        case ShapeType::Sphere: return glm::max(glm::length(p) - radius, 0.0f);
        case ShapeType::Capsule: {
            const glm::vec3 axis(0.0f, glm::clamp(p.y, -halfHeight, halfHeight), 0.0f);
            return glm::max(glm::length(p - axis) - radius, 0.0f);
        }
        case ShapeType::Box:
        default: return glm::length(glm::max(glm::abs(p) - half, glm::vec3(0.0f)));
    }
}

// Ближе всего отрезок [a, b] подходит к форме на этом расстоянии.
float SegmentToShape(const glm::vec3& a, const glm::vec3& b, ShapeType shape, const glm::vec3& half,
                     float radius, float halfHeight) {
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 40; ++i) {
        const float m1 = lo + (hi - lo) / 3.0f, m2 = hi - (hi - lo) / 3.0f;
        const float d1 = DistToShape(glm::mix(a, b, m1), shape, half, radius, halfHeight);
        const float d2 = DistToShape(glm::mix(a, b, m2), shape, half, radius, halfHeight);
        if (d1 > d2) lo = m1;
        else hi = m2;
    }
    return DistToShape(glm::mix(a, b, 0.5f * (lo + hi)), shape, half, radius, halfHeight);
}

// Касается ли капсула (ось [a, b] в мире, радиус r) тела, описанного desc.
bool CapsuleTouchesBody(const glm::vec3& a, const glm::vec3& b, float r, const BodyDesc& desc) {
    const glm::quat inv = glm::inverse(desc.Rotation);
    const glm::vec3 la = inv * (a - desc.Position), lb = inv * (b - desc.Position);
    if (desc.Children.empty())
        return SegmentToShape(la, lb, desc.Shape, desc.HalfExtents, desc.Radius, desc.HalfHeight) <= r;
    for (const ChildShape& c : desc.Children) {
        const glm::quat ci = glm::inverse(c.Rotation);
        if (SegmentToShape(ci * (la - c.Position), ci * (lb - c.Position), c.Shape, c.HalfExtents,
                           c.Radius, c.HalfHeight) <= r)
            return true;
    }
    return false;
}

} // namespace

bool sage::physics::CapsuleTouchesBodyForTest(const glm::vec3& a, const glm::vec3& b, float r,
                                              const BodyDesc& desc) {
    return CapsuleTouchesBody(a, b, r, desc);
}

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

void PhysicsScene::TeleportEntity(Scene& scene, entt::entity e) {
    if (!m_world || !scene.Registry().valid(e)) return;
    RigidBodyComponent* rb = scene.Registry().try_get<RigidBodyComponent>(e);
    if (!rb || rb->RuntimeBody == kInvalidBody) return;

    // Мировая поза, а не локальная: физика живёт в мире, а у объекта может быть
    // родитель. Разбор — тот же, что у кинематики ниже: две формулы для одного
    // и того же однажды разойдутся.
    const WorldTransform tr = DecomposeWorld(scene.WorldMatrix(e));
    m_world->SetBodyTransform(rb->RuntimeBody, tr.Position, tr.Rotation);
    // Скорость обнуляется намеренно: тело ПЕРЕСТАВИЛИ, а не бросили. Иначе
    // предмет, поднятый мышью на метр, улетает с той скоростью, которую набрал,
    // пока падал.
    m_world->SetLinearVelocity(rb->RuntimeBody, glm::vec3(0.0f));
}

void PhysicsScene::Step(Scene& scene, float dt) {
    if (!m_world || !m_world->IsAvailable()) return;

    // Состав мира мог измениться с прошлого кадра (скрипт спавнит и удаляет).
    SyncBodies(scene);
    SyncCharacters(scene);

    // Контроллеры персонажей — ДО шага мира: тяготение, прыжок, склон и
    // ступенька считаются здесь, а не в скрипте (см. StepCharacters).
    StepCharacters(scene, dt);

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
            // Зона — не удар: её касания копятся в списке «кто внутри», а
            // события входа и выхода строит UpdateTriggers по его разнице.
            if (raw.Sensor) {
                NoteSensorContact(scene, ia->second, ib->second,
                                  raw.When == ContactEvent::Phase::Begin);
                continue;
            }
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
    UpdateTriggers(scene);
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
        d.StepHeight = cc.StepOffset;
        d.MaxSlopeDeg = cc.SlopeLimit;
        d.SkinWidth = cc.SkinWidth;
        d.Mass = cc.Mass;
        d.Layer = cc.Layer;
        // Позиция контроллера — ПОДОШВЫ; точка объекта может быть не там
        // (начало координат модели — в поясе, в центре, где угодно).
        d.Position = tr.Position + cc.Center;
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

// --- ШАГ КОНТРОЛЛЕРОВ ПЕРСОНАЖА ---------------------------------------------
//
// ЗАЧЕМ ОН ЗДЕСЬ, А НЕ В СКРИПТЕ. Контроллеру нужно ровно то, чего у скрипта
// нет: шаг физики и результат столкновения того же шага. Пока тяготение писала
// игра, каждая игра писала его заново — и каждая чуть-чуть иначе: у одной
// падение линейное, у другой прыжок зависит от частоты кадров, у третьей на
// стыке плит дрожание. Это не разнообразие, это одна и та же ошибка, набранная
// заново столько раз, сколько есть игр.
//
// Что делает шаг: копит вертикальную скорость тяготением, прилипает к опоре
// (иначе персонаж отрывается на каждой выпуклости пола и «летит» вниз по
// лестнице), сбрасывает вертикаль при приземлении и об потолок, а на склоне
// круче предела превращает ход в скольжение вниз.
void PhysicsScene::StepCharacters(Scene& scene, float dt) {
    if (dt <= 0.0f) return;
    auto view = scene.Registry().view<CharacterControllerComponent, Transform>();
    for (auto e : view) {
        CharacterControllerComponent& cc = view.get<CharacterControllerComponent>(e);
        Transform& tr = view.get<Transform>(e);
        // Игра ведёт этого персонажа сама (старый sage.physics.MoveCharacter) —
        // второе, движковое тяготение удвоило бы её собственное.
        if (!cc.Managed) continue;
        const bool ownWorld = (bool)cc.Solid && cc.Motor;
        if (!ownWorld && cc.Runtime == sage::physics::kInvalidCharacter) continue;

        // --- Вертикаль -------------------------------------------------------
        if (cc.Grounded && cc.VerticalVelocity <= 0.0f) {
            // Небольшая прижимающая скорость, а не ноль: с нулём персонаж на
            // каждом стыке пола отрывается от опоры на кадр, и «стоит на земле»
            // мигает — вместе с ним мигают звук шагов и анимация приземления.
            cc.VerticalVelocity = -2.0f;
        } else {
            cc.VerticalVelocity += cc.Gravity * dt;
            // Предел падения: без него за долгое падение скорость дорастает до
            // величины, на которой шаг перепрыгивает пол целиком.
            const float kTerminal = 55.0f;
            if (cc.VerticalVelocity < -kTerminal) cc.VerticalVelocity = -kTerminal;
        }

        glm::vec3 velocity = cc.HasRequest ? cc.DesiredVelocity : glm::vec3(0.0f);
        velocity.y += cc.VerticalVelocity;

        // --- Склон круче предела: не идём, а съезжаем ------------------------
        if (cc.Grounded) {
            const float up = glm::clamp(cc.GroundNormal.y, -1.0f, 1.0f);
            const float slopeDeg = glm::degrees(std::acos(up));
            if (slopeDeg > cc.SlopeLimit) {
                // Направление вниз по склону — проекция «вниз» на плоскость
                // опоры. Склон, по которому нельзя идти, обязан сносить, иначе
                // на нём можно стоять, просто не двигаясь, — и тогда предел
                // склона не значит ничего.
                glm::vec3 down = glm::vec3(0.0f, -1.0f, 0.0f);
                glm::vec3 slide = down - cc.GroundNormal * glm::dot(down, cc.GroundNormal);
                const float len = glm::length(slide);
                if (len > 1e-4f) {
                    slide /= len;
                    const float speed = std::abs(cc.Gravity) * 0.5f;
                    velocity.x = slide.x * speed;
                    velocity.z = slide.z * speed;
                }
            }
        }

        const glm::vec3 before = ownWorld ? cc.Motor->State().Position : tr.Position + cc.Center;

        if (ownWorld) {
            sage::physics::CharacterDesc d = cc.Motor->Desc();
            d.Radius = cc.Radius;
            d.Height = cc.Height;
            d.StepHeight = cc.StepOffset;
            d.MaxSlopeDeg = cc.SlopeLimit;
            d.SkinWidth = cc.SkinWidth;
            d.Position = cc.Motor->State().Position;
            cc.Motor->Configure(d);
            cc.Motor->Move(cc.Solid, velocity, dt);
            const sage::physics::CharacterState& st = cc.Motor->State();
            tr.Position = st.Position - cc.Center;
            cc.Grounded = st.Grounded;
            cc.GroundNormal = st.GroundNormal;
            cc.Landed = cc.Motor->Landed();
            cc.LeftGround = cc.Motor->LeftGround();
            cc.Blocked = cc.Motor->Blocked();
            cc.StepUp = cc.Motor->StepUp();
            cc.Velocity = (st.Position - before) / dt;
        } else {
            m_world->MoveCharacter(cc.Runtime, velocity, dt);
            // Положение и опора приедут в PullCharacters после шага мира —
            // читать их здесь значило бы видеть состояние ДО столкновения.
            cc.Velocity = velocity;
        }

        // Уткнулись в потолок или встали на опору — вертикаль обнуляется.
        // Без этого прыжок в низкий проём «прилипает» к потолку, пока не
        // иссякнет набранная вверх скорость.
        if (cc.Grounded && cc.VerticalVelocity < 0.0f) cc.VerticalVelocity = 0.0f;

        // Запрос действует ОДИН кадр: персонаж, которому забыли сказать «иди»,
        // обязан остановиться сам, а не ехать вечно.
        cc.HasRequest = false;
        cc.DesiredVelocity = glm::vec3(0.0f);
    }
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
        tr->Position = st.Position - cc->Center;
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

// --- Триггер-зоны ------------------------------------------------------------

void PhysicsScene::NoteSensorContact(Scene& scene, entt::entity a, entt::entity b, bool begin) {
    auto& reg = scene.Registry();
    auto note = [&](entt::entity zone, entt::entity guest) {
        const RigidBodyComponent* rb = reg.valid(zone) ? reg.try_get<RigidBodyComponent>(zone) : nullptr;
        if (!rb || !rb->Sensor) return;
        const std::pair<entt::entity, entt::entity> key(zone, guest);
        auto it = std::find(m_bodyInside.begin(), m_bodyInside.end(), key);
        if (begin && it == m_bodyInside.end()) m_bodyInside.push_back(key);
        else if (!begin && it != m_bodyInside.end()) m_bodyInside.erase(it);
    };
    // Обе стороны: две зоны, коснувшиеся друг друга, — гость каждая у другой.
    note(a, b);
    note(b, a);
}

void PhysicsScene::UpdateTriggers(Scene& scene) {
    auto& reg = scene.Registry();
    std::vector<TriggerOverlap> prev;
    prev.swap(m_inside);

    auto zoneOf = [&](entt::entity e) -> const RigidBodyComponent* {
        if (!reg.valid(e)) return nullptr;
        const RigidBodyComponent* rb = reg.try_get<RigidBodyComponent>(e);
        return rb && rb->Sensor && rb->RuntimeBody != kInvalidBody ? rb : nullptr;
    };
    // Слой гостя: у тела — свой, у персонажа — свой. Нет ни того ни другого —
    // гостя больше нет в физике, и в зоне ему делать нечего.
    auto layerOf = [&](entt::entity e, LayerMask& out) -> bool {
        if (!reg.valid(e)) return false;
        if (const auto* rb = reg.try_get<RigidBodyComponent>(e)) { out = rb->Layer; return true; }
        if (const auto* cc = reg.try_get<CharacterControllerComponent>(e)) { out = cc->Layer; return true; }
        return false;
    };
    auto admit = [&](entt::entity zone, entt::entity guest) {
        const RigidBodyComponent* rb = zoneOf(zone);
        LayerMask layer = 0;
        if (!rb || zone == guest || !layerOf(guest, layer)) return;
        if ((layer & rb->TriggerMask) == 0) return;
        for (const TriggerOverlap& o : m_inside)
            if (o.Trigger == zone && o.Other == guest) return;
        m_inside.push_back({zone, guest, false});
    };

    // 1. Тело с телом — по сообщениям бэкенда. Пары удалённых и переставших
    // быть зонами выбрасываются здесь: иначе они жили бы вечно.
    m_bodyInside.erase(std::remove_if(m_bodyInside.begin(), m_bodyInside.end(),
                                      [&](const auto& p) {
                                          return !zoneOf(p.first) || !reg.valid(p.second) ||
                                                 !reg.all_of<RigidBodyComponent>(p.second);
                                      }),
                       m_bodyInside.end());
    for (const auto& [zone, guest] : m_bodyInside) admit(zone, guest);

    // 2. Персонажи — геометрией (бэкенд о них не сообщает).
    auto characters = reg.view<CharacterControllerComponent, Transform>();
    if (characters.begin() != characters.end()) {
        for (const auto& [zone, body] : m_tracked) {
            const RigidBodyComponent* rb = zoneOf(zone);
            if (!rb) continue;
            const ColliderComponent* col = reg.try_get<ColliderComponent>(zone);
            const BodyDesc desc = DescFromEntity(*rb, col, DecomposeWorld(scene.WorldMatrix(zone)));
            for (auto e : characters) {
                const CharacterControllerComponent& cc = characters.get<CharacterControllerComponent>(e);
                // Подошвы — там, где их ставит контроллер: точка объекта + Center.
                const glm::vec3 feet = glm::vec3(scene.WorldMatrix(e)[3]) + cc.Center;
                const float r = glm::max(cc.Radius, 0.001f);
                const float top = glm::max(cc.Height - r, r);
                if (CapsuleTouchesBody(feet + glm::vec3(0.0f, r, 0.0f), feet + glm::vec3(0.0f, top, 0.0f),
                                       r, desc))
                    admit(zone, e);
            }
        }
    }

    // 3. Разница с прошлым шагом — события входа и выхода.
    auto had = [](const std::vector<TriggerOverlap>& list, const TriggerOverlap& o) {
        for (const TriggerOverlap& x : list)
            if (x.Trigger == o.Trigger && x.Other == o.Other) return true;
        return false;
    };
    for (TriggerOverlap& o : m_inside) {
        o.Entered = !had(prev, o);
        if (!o.Entered) continue;
        EntityContact c;
        c.Begin = true;
        c.A = o.Trigger;
        c.B = o.Other;
        c.Sensor = true;
        m_contacts.push_back(c);
    }
    // Выход — и для удалённого гостя: скрипт зоны, считающий «сколько внутри»,
    // обязан узнать, что одного не стало, даже если того уже нет в сцене.
    for (const TriggerOverlap& o : prev) {
        if (had(m_inside, o)) continue;
        EntityContact c;
        c.Begin = false;
        c.A = o.Trigger;
        c.B = o.Other;
        c.Sensor = true;
        m_contacts.push_back(c);
    }
}

int PhysicsScene::ObjectsInTrigger(entt::entity trigger, std::vector<entt::entity>& out) const {
    out.clear();
    for (const TriggerOverlap& o : m_inside)
        if (o.Trigger == trigger) out.push_back(o.Other);
    return (int)out.size();
}

int PhysicsScene::TriggersOf(entt::entity other, std::vector<entt::entity>& out) const {
    out.clear();
    for (const TriggerOverlap& o : m_inside)
        if (o.Other == other) out.push_back(o.Trigger);
    return (int)out.size();
}
