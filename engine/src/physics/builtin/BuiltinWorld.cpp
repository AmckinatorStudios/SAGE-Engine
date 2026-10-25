#include "physics/builtin/BuiltinWorld.h"
#include "physics/builtin/BuiltinWorldDetail.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/core/Log.h"

namespace sage::physics {
using namespace builtin_detail;

// --- тела ---------------------------------------------------------------------

BuiltinWorld::Body* BuiltinWorld::Find(BodyHandle h) {
    auto it = m_bodies.find(h);
    return it == m_bodies.end() || !it->second.Alive ? nullptr : &it->second;
}
const BuiltinWorld::Body* BuiltinWorld::Find(BodyHandle h) const {
    auto it = m_bodies.find(h);
    return it == m_bodies.end() || !it->second.Alive ? nullptr : &it->second;
}

BodyHandle BuiltinWorld::CreateBody(const BodyDesc& desc) {
    Body b;
    b.Type = desc.Type;
    b.Position = desc.Position;
    b.Rotation = desc.Rotation;
    b.Friction = glm::clamp(desc.Friction, 0.0f, 2.0f);
    b.Restitution = glm::clamp(desc.Restitution, 0.0f, 1.0f);
    b.Layer = desc.Layer;
    b.Sensor = desc.Sensor;
    b.LinearDamping = glm::clamp(desc.LinearDamping, 0.0f, 1.0f);
    b.AngularDamping = glm::clamp(desc.AngularDamping, 0.0f, 1.0f);
    b.GravityScale = desc.GravityScale;
    b.Locks = desc.Locks;
    b.LinearVelocity = desc.LinearVelocity;
    b.AngularVelocity = desc.AngularVelocity;

    if (desc.Children.empty() && (desc.Shape == ShapeType::ConvexHull || desc.Shape == ShapeType::Mesh)) {
        // Оболочек и сеток у встроенного движка нет по замыслу (см. шапку
        // BuiltinWorld.h) — коробка по габариту модели. Грубо, но тело стоит
        // там, где объект нарисован, а не пропадает вовсе.
        glm::vec3 lo(0.0f), hi(0.0f);
        if (desc.Geometry && !desc.Geometry->Empty()) {
            lo = hi = desc.Geometry->Points.front();
            for (const glm::vec3& q : desc.Geometry->Points) { lo = glm::min(lo, q); hi = glm::max(hi, q); }
        } else {
            lo = -desc.HalfExtents;
            hi = desc.HalfExtents;
        }
        Part p;
        p.Shape = ShapeType::Box;
        p.Half = glm::max((hi - lo) * 0.5f, glm::vec3(0.01f));
        p.Position = (hi + lo) * 0.5f;
        b.Parts.push_back(p);
    } else if (desc.Children.empty()) {
        Part p;
        p.Shape = desc.Shape;
        p.Half = desc.HalfExtents;
        p.Radius = desc.Radius;
        p.HalfHeight = desc.HalfHeight;
        b.Parts.push_back(p);
    } else {
        for (const ChildShape& c : desc.Children) b.Parts.push_back(PartFromChild(c));
    }

    // Масса и инерция по ФОРМЕ: доли массы раздаются частям по объёму, инерции
    // частей сносятся к общему центру теоремой Гюйгенса—Штейнера. Ставить всем
    // телам шаровую инерцию — значит получить мир, где доска и кубик крутятся
    // одинаково.
    float totalVolume = 0.0f;
    glm::vec3 weightedCenter(0.0f);
    std::vector<glm::mat3> inertias(b.Parts.size());
    std::vector<glm::vec3> centers(b.Parts.size());
    std::vector<float> volumes(b.Parts.size());
    for (size_t i = 0; i < b.Parts.size(); ++i) {
        builtin::PartInertiaUnitMass(b.Parts[i], inertias[i], centers[i], volumes[i]);
        volumes[i] = std::max(volumes[i], 1e-6f);
        totalVolume += volumes[i];
        weightedCenter += centers[i] * volumes[i];
    }
    b.CenterLocal = totalVolume > kEps ? weightedCenter / totalVolume : glm::vec3(0.0f);

    if (b.Type == BodyType::Dynamic) {
        const float mass = std::max(desc.Mass, 1e-4f);
        b.InvMass = 1.0f / mass;
        glm::mat3 inertia(0.0f);
        for (size_t i = 0; i < b.Parts.size(); ++i) {
            const float m = mass * (volumes[i] / totalVolume);
            const glm::vec3 r = centers[i] - b.CenterLocal;
            const float r2 = glm::dot(r, r);
            // I += m·(I_части + (r²·E − r⊗r))
            glm::mat3 shift(0.0f);
            shift[0][0] = r2 - r.x * r.x; shift[0][1] = -r.x * r.y; shift[0][2] = -r.x * r.z;
            shift[1][0] = -r.y * r.x; shift[1][1] = r2 - r.y * r.y; shift[1][2] = -r.y * r.z;
            shift[2][0] = -r.z * r.x; shift[2][1] = -r.z * r.y; shift[2][2] = r2 - r.z * r.z;
            inertia += (inertias[i] + shift) * m;
        }
        // Вырожденная инерция (нулевая толщина) сделала бы матрицу
        // необратимой; подпираем минимумом, а не падаем.
        for (int i = 0; i < 3; ++i) inertia[i][i] = std::max(inertia[i][i], 1e-5f);
        b.InvInertiaLocal = glm::inverse(inertia);
    } else {
        b.InvMass = 0.0f;
        b.InvInertiaLocal = glm::mat3(0.0f);
    }

    const BodyHandle h = m_next++;
    m_bodies.emplace(h, std::move(b));
    return h;
}

void BuiltinWorld::RemoveBody(BodyHandle body) {
    m_bodies.erase(body);
    // Соединения на исчезнувшее тело держать нельзя: следующий же шаг обратился
    // бы к нему по несуществующему дескриптору.
    for (auto& [h, j] : m_joints)
        if (j.Desc.BodyA == body || j.Desc.BodyB == body) j.Alive = false;
    for (auto it = m_cache.begin(); it != m_cache.end();)
        it = (it->first.A == body || it->first.B == body) ? m_cache.erase(it) : std::next(it);
}

void BuiltinWorld::GetBodyTransform(BodyHandle body, glm::vec3& position, glm::quat& rotation) const {
    const Body* b = Find(body);
    if (!b) { position = glm::vec3(0.0f); rotation = glm::quat(1, 0, 0, 0); return; }
    position = b->Position;
    rotation = b->Rotation;
}

void BuiltinWorld::SetBodyTransform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation) {
    Body* b = Find(body);
    if (!b) return;
    b->Position = position;
    b->Rotation = glm::normalize(rotation);
    Wake(*b);
}

void BuiltinWorld::SetLinearVelocity(BodyHandle body, const glm::vec3& velocity) {
    Body* b = Find(body);
    if (!b) return;
    b->LinearVelocity = velocity;
    Wake(*b);
}

glm::vec3 BuiltinWorld::GetLinearVelocity(BodyHandle body) const {
    const Body* b = Find(body);
    return b ? b->LinearVelocity : glm::vec3(0.0f);
}

void BuiltinWorld::AddImpulse(BodyHandle body, const glm::vec3& impulse) {
    Body* b = Find(body);
    if (!b || !b->Movable()) return;
    b->LinearVelocity += impulse * b->InvMass;
    Wake(*b);
}

void BuiltinWorld::SetAngularVelocity(BodyHandle body, const glm::vec3& w) {
    Body* b = Find(body);
    if (!b || b->Type == BodyType::Static) return;
    b->AngularVelocity = w;
    ApplyLocks(*b);
    Wake(*b);
}

glm::vec3 BuiltinWorld::GetAngularVelocity(BodyHandle body) const {
    const Body* b = Find(body);
    return b ? b->AngularVelocity : glm::vec3(0.0f);
}

void BuiltinWorld::AddImpulseAtPoint(BodyHandle body, const glm::vec3& impulse, const glm::vec3& point) {
    Body* b = Find(body);
    if (!b || !b->Movable()) return;
    const glm::mat3 r = glm::mat3_cast(b->Rotation);
    b->InvInertiaWorld = r * b->InvInertiaLocal * glm::transpose(r);
    ApplyImpulse(*b, impulse, point);
    ApplyLocks(*b);
    Wake(*b);
}

void BuiltinWorld::AddAngularImpulse(BodyHandle body, const glm::vec3& impulse) {
    Body* b = Find(body);
    if (!b || !b->Movable()) return;
    const glm::mat3 r = glm::mat3_cast(b->Rotation);
    b->AngularVelocity += r * b->InvInertiaLocal * glm::transpose(r) * impulse;
    ApplyLocks(*b);
    Wake(*b);
}

void BuiltinWorld::AddForce(BodyHandle body, const glm::vec3& force) {
    Body* b = Find(body);
    if (!b || !b->Movable()) return;
    b->Force += force;
    Wake(*b);
}

void BuiltinWorld::AddForceAtPoint(BodyHandle body, const glm::vec3& force, const glm::vec3& point) {
    Body* b = Find(body);
    if (!b || !b->Movable()) return;
    b->Force += force;
    b->Torque += glm::cross(point - b->Center(), force);
    Wake(*b);
}

void BuiltinWorld::AddTorque(BodyHandle body, const glm::vec3& torque) {
    Body* b = Find(body);
    if (!b || !b->Movable()) return;
    b->Torque += torque;
    Wake(*b);
}

bool BuiltinWorld::IsSleeping(BodyHandle body) const {
    const Body* b = Find(body);
    return b && b->Movable() && b->Sleeping;
}

void BuiltinWorld::ApplyLocks(Body& b) {
    if (b.Locks == kLockNone) return;
    for (int i = 0; i < 3; ++i) {
        if (b.Locks & (kLockPosX << i)) b.LinearVelocity[i] = 0.0f;
        if (b.Locks & (kLockRotX << i)) b.AngularVelocity[i] = 0.0f;
    }
}

void BuiltinWorld::ApplyImpulse(Body& b, const glm::vec3& impulse, const glm::vec3& at) {
    if (!b.Movable()) return;
    b.LinearVelocity += impulse * b.InvMass;
    b.AngularVelocity += b.InvInertiaWorld * glm::cross(at - b.Center(), impulse);
}

// --- соединения ---------------------------------------------------------------

JointHandle BuiltinWorld::CreateJoint(const JointDesc& desc) {
    Body* a = Find(desc.BodyA);
    if (!a) return kInvalidJoint;
    Body* b = desc.BodyB == kInvalidBody ? nullptr : Find(desc.BodyB);
    if (desc.BodyB != kInvalidBody && !b) return kInvalidJoint;

    Joint j;
    j.Desc = desc;
    // Мировой якорь переводим в системы обоих тел ОДИН раз: дальше соединение
    // держит именно локальные точки, и поворот тела уносит крепление с собой.
    // ТРОС — ОСОБЫЙ СЛУЧАЙ. У всех остальных соединений якорь — это точка,
    // в которой тела СХОДЯТСЯ, поэтому она одна и общая. У троса точка одна
    // (крюк на потолке), а второй конец — САМО ТЕЛО, и крепить его к той же
    // точке значит получить нулевую длину: ограничение развернёт тела в
    // произвольную сторону, а тело сможет уехать на длину троса ПЛЮС вылет
    // своего крепления. Маятник в этом случае просто ложится на пол — что и
    // происходило до этой правки.
    const bool rope = desc.Type == JointType::Distance;
    j.LocalAnchorA = rope ? glm::vec3(0.0f)
                          : glm::conjugate(a->Rotation) * (desc.Anchor - a->Position);
    j.LocalAxisA = glm::conjugate(a->Rotation) * glm::normalize(desc.Axis + glm::vec3(0.0f, 1e-9f, 0.0f));
    if (b) {
        j.LocalAnchorB = glm::conjugate(b->Rotation) * (desc.Anchor - b->Position);
        j.LocalAxisB = glm::conjugate(b->Rotation) * glm::normalize(desc.Axis + glm::vec3(0.0f, 1e-9f, 0.0f));
        j.RelativeRotation = glm::conjugate(a->Rotation) * b->Rotation;
    } else {
        j.LocalAnchorB = desc.Anchor;
        j.LocalAxisB = glm::normalize(desc.Axis + glm::vec3(0.0f, 1e-9f, 0.0f));
        j.RelativeRotation = glm::conjugate(a->Rotation);
    }

    const JointHandle h = m_nextJoint++;
    m_joints.emplace(h, j);
    return h;
}

void BuiltinWorld::RemoveJoint(JointHandle joint) { m_joints.erase(joint); }

// --- шаг ----------------------------------------------------------------------

void BuiltinWorld::Step(float dt) {
    if (dt <= 0.0f) return;
    // Сила действует всё время кадра: её импульс копится и раздаётся
    // подшагам. Иначе число подшагов (0, 1 или 2 на кадр) решало бы, сколько
    // тело разгонится, и двигатель тянул бы по-разному на 30 и 144 кадрах.
    const float frame = std::min(dt, kFixedStep * kMaxSubSteps);
    for (auto& [h, b] : m_bodies) {
        if (!b.Alive) continue;
        b.PendingLinear += b.Force * frame;
        b.PendingAngular += b.Torque * frame;
        b.Force = b.Torque = glm::vec3(0.0f);
    }
    m_accum += dt;
    // Потолок накопителя: после долгой паузы (окно свернули, загрузка сцены)
    // накопилось бы несколько секунд, и физика попыталась бы отсчитать их
    // разом — кадр встал бы намертво, а мир взорвался.
    m_accum = std::min(m_accum, kFixedStep * kMaxSubSteps);
    // Сколько подшагов будет — известно заранее: импульс сил делится поровну.
    // Допуск в тысячную шага — чтобы кадр ровно в два шага не превращался в
    // один из-за округления.
    const int steps = std::min((int)((m_accum + kFixedStep * 1e-3f) / kFixedStep), kMaxSubSteps);
    if (steps == 0) return;
    for (auto& [h, b] : m_bodies) {
        if (!b.Alive || !b.Movable()) continue;
        b.Force = b.PendingLinear / (kFixedStep * steps);
        b.Torque = b.PendingAngular / (kFixedStep * steps);
        b.PendingLinear = b.PendingAngular = glm::vec3(0.0f);
    }
    for (int i = 0; i < steps; ++i) SubStep(kFixedStep);
    m_accum = std::max(m_accum - kFixedStep * steps, 0.0f);
    for (auto& [h, b] : m_bodies) b.Force = b.Torque = glm::vec3(0.0f);
}

void BuiltinWorld::SubStep(float dt) {
    // 1. Мировая обратная инерция и внешние силы.
    for (auto& [h, b] : m_bodies) {
        if (!b.Alive) continue;
        if (b.Movable()) {
            const glm::mat3 r = glm::mat3_cast(b.Rotation);
            b.InvInertiaWorld = r * b.InvInertiaLocal * glm::transpose(r);
            // Замороженный поворот — нулевая обратная инерция по мировой оси:
            // решатель контактов тогда сам не пытается крутить тело вокруг
            // неё, а не только «поправляется» после.
            if (b.Locks & kLockRotAll) {
                glm::mat3 keep(1.0f);
                for (int i = 0; i < 3; ++i)
                    if (b.Locks & (kLockRotX << i)) keep[i][i] = 0.0f;
                b.InvInertiaWorld = keep * b.InvInertiaWorld * keep;
            }
            if (glm::dot(b.Force, b.Force) > 0.0f || glm::dot(b.Torque, b.Torque) > 0.0f) {
                b.LinearVelocity += b.Force * b.InvMass * dt;
                b.AngularVelocity += b.InvInertiaWorld * b.Torque * dt;
                Wake(b);
            }
            if (!b.Sleeping) b.LinearVelocity += m_gravity * b.GravityScale * dt;
            // Затухание как у Jolt: dv/dt = −c·v.
            b.LinearVelocity *= std::max(0.0f, 1.0f - b.LinearDamping * dt);
            b.AngularVelocity *= std::max(0.0f, 1.0f - b.AngularDamping * dt);
            ApplyLocks(b);
        } else {
            b.InvInertiaWorld = glm::mat3(0.0f);
        }
    }

    BuildContacts(dt);
    WarmStart();
    SolveVelocities(dt, kVelocityIterations, /*useBias=*/true);
    Integrate(dt);
    // Расслабляющий проход БЕЗ выталкивания: он снимает ту скорость, которую
    // внесла коррекция проникновения. Выталкивание — это подкачка энергии в
    // систему, и без её снятия стопка ящиков медленно «дышит», а мяч,
    // продавивший пол, отскакивает выше, чем упал.
    SolveVelocities(dt, kRelaxIterations, /*useBias=*/false);
    ApplyRestitution();
    UpdateSleep(dt);
    EmitContactEvents();
}

// --- широкая и узкая фазы ------------------------------------------------------

// --- соединения: скоростные ограничения ---------------------------------------

// --- интегрирование ------------------------------------------------------------

void BuiltinWorld::Integrate(float dt) {
    for (auto& [h, b] : m_bodies) {
        if (!b.Alive || b.Sleeping) continue;
        if (b.Type == BodyType::Static) continue;

        if (b.Movable()) ApplyLocks(b);
        b.Position += b.LinearVelocity * dt;
        if (b.Type == BodyType::Kinematic) continue;

        // Поворот: q += ½·ω·q·dt с последующей нормализацией. Кватернион
        // накапливает ошибку длины на каждом шаге, и без нормализации тело
        // через минуту начинает необратимо раздуваться в матрице поворота.
        if (glm::dot(b.AngularVelocity, b.AngularVelocity) > kEps) {
            const glm::quat w(0.0f, b.AngularVelocity.x, b.AngularVelocity.y, b.AngularVelocity.z);
            b.Rotation = glm::normalize(b.Rotation + 0.5f * dt * (w * b.Rotation));
        }
        // Смещение центра масс при повороте: тело крутится вокруг ЦЕНТРА, а
        // начало координат ездит вместе с ним. Иначе составное тело при
        // вращении медленно уползает в сторону.
        // (Position задаёт начало координат; центр = Position + R·CenterLocal.)
    }
}

void BuiltinWorld::UpdateSleep(float dt) {
    for (auto& [h, b] : m_bodies) {
        if (!b.Alive || !b.Movable()) continue;
        const bool slow = glm::length(b.LinearVelocity) < kSleepLinear &&
                          glm::length(b.AngularVelocity) < kSleepAngular;
        if (slow) {
            b.IdleTime += dt;
            if (b.IdleTime > kSleepDelay && !b.Sleeping) {
                b.Sleeping = true;
                b.LinearVelocity = glm::vec3(0.0f);
                b.AngularVelocity = glm::vec3(0.0f);
            }
        } else {
            b.IdleTime = 0.0f;
            b.Sleeping = false;
        }
    }
}

// --- события -------------------------------------------------------------------

void BuiltinWorld::EmitContactEvents() {
    std::set<std::pair<BodyHandle, BodyHandle>> now;
    for (const ContactConstraint& c : m_contacts) {
        // Спекулятивные точки (зазор ещё положителен) — это ЕЩЁ НЕ касание.
        // Считать их касанием значило бы играть звук удара до самого удара.
        bool touching = false;
        for (int i = 0; i < c.M.Count; ++i)
            if (c.M.Points[i].Separation <= 0.0f) { touching = true; break; }
        if (!touching) continue;
        now.insert({c.Key.A, c.Key.B});
    }

    for (const auto& pair : now) {
        if (m_touching.count(pair)) continue;   // уже касались — не событие
        const Body* a = Find(pair.first);
        const Body* b = Find(pair.second);
        if (!a || !b) continue;
        // Точку и нормаль берём у первой найденной точки этой пары: событию
        // нужна одна репрезентативная, а не весь манифольд.
        ContactEvent e;
        e.When = ContactEvent::Phase::Begin;
        e.A = pair.first;
        e.B = pair.second;
        e.Sensor = a->Sensor || b->Sensor;
        for (const ContactConstraint& c : m_contacts) {
            if (c.Key.A != pair.first || c.Key.B != pair.second) continue;
            e.Normal = c.M.Normal;
            e.Point = c.M.Points[0].Position;
            if (!e.Sensor) {
                float sum = 0.0f;
                for (int i = 0; i < c.M.Count; ++i) sum += c.M.Points[i].NormalImpulse;
                e.Impulse = sum;
            }
            break;
        }
        m_events.push_back(e);
    }
    for (const auto& pair : m_touching) {
        if (now.count(pair)) continue;
        ContactEvent e;
        e.When = ContactEvent::Phase::End;
        e.A = pair.first;
        e.B = pair.second;
        const Body* a = Find(pair.first);
        const Body* b = Find(pair.second);
        e.Sensor = (a && a->Sensor) || (b && b->Sensor);
        m_events.push_back(e);
    }
    m_touching.swap(now);

    // Манифольды в кэш — на следующий шаг за тёплым стартом.
    m_cache.clear();
    for (const ContactConstraint& c : m_contacts) m_cache[c.Key] = c.M;
}

void BuiltinWorld::PollContacts(std::vector<ContactEvent>& out) {
    out.swap(m_events);
    m_events.clear();
}

// --- запросы --------------------------------------------------------------------

bool BuiltinWorld::Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                           RayHit& out, LayerMask mask) const {
    out = RayHit{};
    const float len = glm::length(direction);
    if (len < 1e-6f || maxDistance <= 0.0f) return false;
    const glm::vec3 dir = direction / len;

    float best = maxDistance;
    for (const auto& [h, b] : m_bodies) {
        if (!b.Alive || (b.Layer & mask) == 0) continue;
        const Pose pose{b.Position, b.Rotation};
        for (const Part& p : b.Parts) {
            float t;
            glm::vec3 n;
            if (!builtin::RaycastPart(p, pose, origin, dir, best, t, n)) continue;
            if (t >= best) continue;
            best = t;
            out.Hit = true;
            out.Body = h;
            out.Distance = t;
            out.Point = origin + dir * t;
            // Нормаль всегда НАВСТРЕЧУ лучу: попав в заднюю грань, «наружу»
            // указывает не та сторона, и отражение уходит внутрь тела.
            out.Normal = glm::dot(n, dir) > 0.0f ? -n : n;
        }
    }
    return out.Hit;
}

int BuiltinWorld::OverlapSphere(const glm::vec3& center, float radius, std::vector<BodyHandle>& out,
                                LayerMask mask) const {
    out.clear();
    if (radius <= 0.0f) return 0;
    for (const auto& [h, b] : m_bodies) {
        if (!b.Alive || (b.Layer & mask) == 0) continue;
        const Pose pose{b.Position, b.Rotation};
        for (const Part& p : b.Parts) {
            if (!builtin::OverlapSpherePart(p, pose, center, radius)) continue;
            out.push_back(h);
            break;
        }
    }
    return (int)out.size();
}

// --- контроллер персонажа --------------------------------------------------------

bool BuiltinWorld::SolidBox(const glm::vec3& min, const glm::vec3& max, LayerMask mask) const {
    // Персонаж — коробка, поэтому спрашиваем настоящим пересечением форм:
    // строим временную часть-коробку и сталкиваем её с телами мира.
    Part probe;
    probe.Shape = ShapeType::Box;
    probe.Half = (max - min) * 0.5f;
    const Pose probePose{(max + min) * 0.5f, glm::quat(1, 0, 0, 0)};

    for (const auto& [h, b] : m_bodies) {
        if (!b.Alive || b.Sensor || (b.Layer & mask) == 0) continue;
        // Сквозь динамику персонаж проходит: толкать её он не умеет, а
        // упираться в ящик, который от него не сдвинется, — значит застрять.
        if (b.Type == BodyType::Dynamic) continue;
        const Pose pose{b.Position, b.Rotation};
        for (const Part& p : b.Parts) {
            Manifold m;
            if (builtin::Collide(probe, probePose, p, pose, 0.0f, m) && m.Count > 0) {
                for (int i = 0; i < m.Count; ++i)
                    if (m.Points[i].Separation < 0.0f) return true;
            }
        }
    }
    return false;
}

CharacterHandle BuiltinWorld::CreateCharacter(const CharacterDesc& desc) {
    Character c;
    c.Motor.Configure(desc);
    c.CollidesWith = desc.CollidesWith;
    const CharacterHandle handle = m_nextCharacter++;
    m_characters.emplace(handle, std::move(c));
    return handle;
}

void BuiltinWorld::RemoveCharacter(CharacterHandle character) { m_characters.erase(character); }

void BuiltinWorld::MoveCharacter(CharacterHandle character, const glm::vec3& velocity, float dt) {
    auto it = m_characters.find(character);
    if (it == m_characters.end()) return;
    const LayerMask mask = it->second.CollidesWith;
    it->second.Motor.Move(
        [this, mask](const glm::vec3& min, const glm::vec3& max) { return SolidBox(min, max, mask); },
        velocity, dt);
}

CharacterState BuiltinWorld::GetCharacterState(CharacterHandle character) const {
    auto it = m_characters.find(character);
    return it == m_characters.end() ? CharacterState{} : it->second.Motor.State();
}

void BuiltinWorld::SetCharacterPosition(CharacterHandle character, const glm::vec3& position) {
    auto it = m_characters.find(character);
    if (it != m_characters.end()) it->second.Motor.SetPosition(position);
}

void BuiltinWorld::SetCharacterCollidesWith(CharacterHandle character, LayerMask mask) {
    auto it = m_characters.find(character);
    if (it != m_characters.end()) it->second.CollidesWith = mask;
}

// Касание — та же коробка, что у мотора, раздутая на пару сантиметров: мотор
// останавливает персонажа В ЗАЗОРЕ от тверди, и коробка ровно по размеру не
// пересекала бы ни пол, на котором он стоит, ни стену, в которую упёрся.
void BuiltinWorld::CharacterContacts(CharacterHandle character, std::vector<BodyHandle>& out) const {
    out.clear();
    auto it = m_characters.find(character);
    if (it == m_characters.end()) return;
    glm::vec3 min, max;
    it->second.Motor.Bounds(min, max);
    const glm::vec3 pad(0.03f);
    Part probe;
    probe.Shape = ShapeType::Box;
    probe.Half = (max - min) * 0.5f + pad;
    const Pose probePose{(max + min) * 0.5f, glm::quat(1, 0, 0, 0)};
    const LayerMask mask = it->second.CollidesWith;
    for (const auto& [h, b] : m_bodies) {
        if (!b.Alive || b.Sensor || (b.Layer & mask) == 0) continue;
        const Pose pose{b.Position, b.Rotation};
        for (const Part& p : b.Parts) {
            Manifold m;
            if (builtin::Collide(probe, probePose, p, pose, 0.0f, m) && m.Count > 0) {
                out.push_back(h);
                break;
            }
        }
    }
}

} // namespace sage::physics
