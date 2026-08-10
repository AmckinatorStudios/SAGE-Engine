#include "physics/builtin/BuiltinWorld.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/core/Log.h"

namespace sage::physics {
namespace {

using builtin::Manifold;
using builtin::Part;
using builtin::Pose;

// Фиксированный шаг. Физика с переменным шагом невоспроизводима: одна и та же
// сцена на разных машинах расходится, а на просадке кадра пружины и стопки
// взрываются. Кадр разбивается на целое число таких шагов, остаток
// переносится на следующий кадр.
constexpr float kFixedStep = 1.0f / 120.0f;
constexpr int kMaxSubSteps = 8;   // потолок «спирали смерти» на тяжёлом кадре

// Итерации решателя. Десять — обычный компромисс между устойчивостью стопки и
// ценой шага; меньше — стопка проседает, больше — платим за незаметное.
constexpr int kVelocityIterations = 10;
constexpr int kRelaxIterations = 3;

// Допуск проникновения. Выталкивать контакт до нуля нельзя: тела начинают
// подрагивать, потому что решатель каждый кадр отталкивает их чуть сильнее,
// чем нужно, и они отскакивают обратно в контакт.
constexpr float kSlop = 0.005f;
constexpr float kBaumgarte = 0.2f;            // доля ошибки, исправляемая за шаг
constexpr float kMaxCorrectionSpeed = 3.0f;   // м/с — потолок выталкивания

// Спекулятивный запас: на сколько вперёд заводить контакт до касания.
constexpr float kSpeculativeMargin = 0.04f;

// Упругость ниже этого порога сближения не применяется — иначе лежащее тело
// бесконечно мелко подпрыгивает на собственной упругости.
constexpr float kRestitutionThreshold = 1.0f;

constexpr float kSleepLinear = 0.06f;    // м/с
constexpr float kSleepAngular = 0.12f;   // рад/с
constexpr float kSleepDelay = 0.5f;      // с покоя до засыпания

constexpr float kEps = 1e-8f;

builtin::Part PartFromChild(const ChildShape& c) {
    Part p;
    p.Shape = c.Shape;
    p.Half = c.HalfExtents;
    p.Radius = c.Radius;
    p.HalfHeight = c.HalfHeight;
    p.Position = c.Position;
    p.Rotation = c.Rotation;
    return p;
}

// Две касательные к нормали. Ортонормированный базис строим от той оси, что
// наименее сонаправлена с нормалью, — иначе на вертикальной нормали
// касательные вырождаются в ноль и трение исчезает ровно там, где оно нужнее
// всего (на полу).
void TangentBasis(const glm::vec3& n, glm::vec3& t1, glm::vec3& t2) {
    if (std::abs(n.x) >= 0.57735f) t1 = glm::normalize(glm::vec3(n.y, -n.x, 0.0f));
    else t1 = glm::normalize(glm::vec3(0.0f, n.z, -n.y));
    t2 = glm::cross(n, t1);
}

glm::mat3 Skew(const glm::vec3& v) {
    return glm::mat3(0.0f, v.z, -v.y, -v.z, 0.0f, v.x, v.y, -v.x, 0.0f);
}

} // namespace

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

    if (desc.Children.empty()) {
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
    j.LocalAnchorA = glm::conjugate(a->Rotation) * (desc.Anchor - a->Position);
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
    m_accum += dt;
    // Потолок накопителя: после долгой паузы (окно свернули, загрузка сцены)
    // накопилось бы несколько секунд, и физика попыталась бы отсчитать их
    // разом — кадр встал бы намертво, а мир взорвался.
    m_accum = std::min(m_accum, kFixedStep * kMaxSubSteps);
    int steps = 0;
    while (m_accum >= kFixedStep && steps < kMaxSubSteps) {
        SubStep(kFixedStep);
        m_accum -= kFixedStep;
        ++steps;
    }
}

void BuiltinWorld::SubStep(float dt) {
    // 1. Мировая обратная инерция и внешние силы.
    for (auto& [h, b] : m_bodies) {
        if (!b.Alive) continue;
        if (b.Movable()) {
            const glm::mat3 r = glm::mat3_cast(b.Rotation);
            b.InvInertiaWorld = r * b.InvInertiaLocal * glm::transpose(r);
            if (!b.Sleeping) b.LinearVelocity += m_gravity * dt;
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

void BuiltinWorld::BuildContacts(float dt) {
    m_contacts.clear();
    m_proxies.clear();
    m_pairs.clear();

    for (auto& [h, b] : m_bodies) {
        if (!b.Alive || b.Parts.empty()) continue;
        Pose pose{b.Position, b.Rotation};
        glm::vec3 lo(FLT_MAX), hi(-FLT_MAX), pmin, pmax;
        for (const Part& p : b.Parts) {
            builtin::PartAabb(p, pose, pmin, pmax);
            lo = glm::min(lo, pmin);
            hi = glm::max(hi, pmax);
        }
        // Габарит расширяем на путь, который тело пройдёт за шаг, плюс
        // спекулятивный запас: пара обязана найтись ДО касания, иначе быстрое
        // тело за кадр перескакивает препятствие целиком.
        const glm::vec3 sweep = b.LinearVelocity * dt;
        lo += glm::min(sweep, glm::vec3(0.0f)) - glm::vec3(kSpeculativeMargin);
        hi += glm::max(sweep, glm::vec3(0.0f)) + glm::vec3(kSpeculativeMargin);
        m_proxies.push_back({h, lo, hi});
    }

    // Заметание: сортируем по левой границе X и сравниваем только с теми, кто
    // ещё не кончился. Перебор всех пар давал бы квадрат от числа тел — на
    // трёхстах ящиках это сорок пять тысяч проверок каждый шаг.
    std::sort(m_proxies.begin(), m_proxies.end(),
              [](const Proxy& a, const Proxy& b) { return a.Min.x < b.Min.x; });
    for (size_t i = 0; i < m_proxies.size(); ++i) {
        const Proxy& pi = m_proxies[i];
        for (size_t j = i + 1; j < m_proxies.size(); ++j) {
            const Proxy& pj = m_proxies[j];
            if (pj.Min.x > pi.Max.x) break;   // дальше по X никто не пересечётся
            if (pi.Min.y > pj.Max.y || pj.Min.y > pi.Max.y) continue;
            if (pi.Min.z > pj.Max.z || pj.Min.z > pi.Max.z) continue;

            const Body* ba = Find(pi.Handle);
            const Body* bb = Find(pj.Handle);
            if (!ba || !bb) continue;
            // Две неподвижности друг о друга не бьются, две спящие — тоже.
            if (!ba->Movable() && !bb->Movable()) continue;
            if ((ba->Sleeping || !ba->Movable()) && (bb->Sleeping || !bb->Movable())) continue;
            if ((ba->Layer & bb->Layer) == 0 && ba->Layer != bb->Layer) {
                // Слои здесь — принадлежность, а не фильтр запроса: тела с
                // непересекающимися слоями друг друга не замечают.
                continue;
            }
            m_pairs.emplace_back(std::min(pi.Handle, pj.Handle), std::max(pi.Handle, pj.Handle));
        }
    }

    for (const auto& [ha, hb] : m_pairs) {
        Body* a = Find(ha);
        Body* b = Find(hb);
        if (!a || !b) continue;
        const Pose pa{a->Position, a->Rotation};
        const Pose pb{b->Position, b->Rotation};
        // Запас берётся ПО СКОРОСТИ СБЛИЖЕНИЯ, а не фиксированный. В этом вся
        // суть спекулятивных контактов: точка должна завестись раньше, чем
        // тело успеет пролететь препятствие. Снаряд, идущий сто шестьдесят
        // метров в секунду, за шаг проходит больше метра — с постоянным
        // запасом в четыре сантиметра узкая фаза не увидит стенку ни на одном
        // шаге, и он пройдёт сквозь неё, ни разу не столкнувшись.
        const float approach = glm::length(a->LinearVelocity - b->LinearVelocity) * dt;
        const float margin = std::max(kSpeculativeMargin, approach * 1.1f);

        for (uint16_t i = 0; i < (uint16_t)a->Parts.size(); ++i) {
            for (uint16_t j = 0; j < (uint16_t)b->Parts.size(); ++j) {
                Manifold m;
                if (!builtin::Collide(a->Parts[i], pa, b->Parts[j], pb, margin, m)) continue;
                if (m.Count == 0) continue;

                // Скорость сближения в каждой точке — до всякого решения.
                // Упругость считается только от неё (см. ApplyRestitution).
                for (int k = 0; k < m.Count; ++k) {
                    const glm::vec3 ra = m.Points[k].Position - a->Center();
                    const glm::vec3 rb = m.Points[k].Position - b->Center();
                    const glm::vec3 rel = (b->LinearVelocity + glm::cross(b->AngularVelocity, rb)) -
                                          (a->LinearVelocity + glm::cross(a->AngularVelocity, ra));
                    m.Points[k].RelativeVelocity = glm::dot(rel, m.Normal);
                }

                ContactConstraint c;
                c.Key = {ha, hb, i, j};
                c.M = m;
                // Трение и упругость пары: среднее геометрическое трения и
                // максимум упругости — общепринятая пара правил. Резиновый мяч
                // о бетон обязан прыгать, а не «усредняться» до половины.
                c.Friction = std::sqrt(a->Friction * b->Friction);
                c.Restitution = std::max(a->Restitution, b->Restitution);
                c.Sensor = a->Sensor || b->Sensor;

                // Тёплый старт: переносим импульсы точек с совпавшим Id.
                auto prev = m_cache.find(c.Key);
                c.Fresh = prev == m_cache.end();
                if (!c.Fresh) {
                    for (int pi2 = 0; pi2 < c.M.Count; ++pi2) {
                        for (int qi = 0; qi < prev->second.Count; ++qi) {
                            if (prev->second.Points[qi].Id != c.M.Points[pi2].Id) continue;
                            c.M.Points[pi2].NormalImpulse = prev->second.Points[qi].NormalImpulse;
                            c.M.Points[pi2].TangentImpulse[0] = prev->second.Points[qi].TangentImpulse[0];
                            c.M.Points[pi2].TangentImpulse[1] = prev->second.Points[qi].TangentImpulse[1];
                            break;
                        }
                    }
                }
                m_contacts.push_back(c);

                // Касание будит: спящий ящик, на который упал другой, обязан
                // проснуться — иначе новый ящик провалится в него насквозь.
                if (!c.Sensor) {
                    const bool aMoving = a->Movable() && !a->Sleeping;
                    const bool bMoving = b->Movable() && !b->Sleeping;
                    if (aMoving && b->Movable()) Wake(*b);
                    if (bMoving && a->Movable()) Wake(*a);
                }
            }
        }
    }
}

void BuiltinWorld::WarmStart() {
    for (ContactConstraint& c : m_contacts) {
        if (c.Sensor) continue;
        Body* a = Find(c.Key.A);
        Body* b = Find(c.Key.B);
        if (!a || !b) continue;
        glm::vec3 t1, t2;
        TangentBasis(c.M.Normal, t1, t2);
        for (int i = 0; i < c.M.Count; ++i) {
            const builtin::ContactPoint& p = c.M.Points[i];
            const glm::vec3 impulse =
                c.M.Normal * p.NormalImpulse + t1 * p.TangentImpulse[0] + t2 * p.TangentImpulse[1];
            ApplyImpulse(*a, -impulse, p.Position);
            ApplyImpulse(*b, impulse, p.Position);
        }
    }
}

void BuiltinWorld::SolveVelocities(float dt, int iterations, bool useBias) {
    const float invDt = dt > 0.0f ? 1.0f / dt : 0.0f;

    for (int iter = 0; iter < iterations; ++iter) {
        for (auto& [h, j] : m_joints) {
            if (j.Alive) SolveJoint(j, dt, useBias);
        }

        for (ContactConstraint& c : m_contacts) {
            if (c.Sensor) continue;
            Body* a = Find(c.Key.A);
            Body* b = Find(c.Key.B);
            if (!a || !b) continue;
            if (a->Sleeping && b->Sleeping) continue;

            const glm::vec3& n = c.M.Normal;
            glm::vec3 t1, t2;
            TangentBasis(n, t1, t2);

            for (int i = 0; i < c.M.Count; ++i) {
                builtin::ContactPoint& p = c.M.Points[i];
                const glm::vec3 ra = p.Position - a->Center();
                const glm::vec3 rb = p.Position - b->Center();

                auto relativeVelocity = [&]() {
                    return (b->LinearVelocity + glm::cross(b->AngularVelocity, rb)) -
                           (a->LinearVelocity + glm::cross(a->AngularVelocity, ra));
                };

                // --- нормаль ---
                {
                    const glm::vec3 rna = glm::cross(ra, n), rnb = glm::cross(rb, n);
                    const float k = a->InvMass + b->InvMass +
                                    glm::dot(n, glm::cross(a->InvInertiaWorld * rna, ra)) +
                                    glm::dot(n, glm::cross(b->InvInertiaWorld * rnb, rb));
                    if (k > kEps) {
                        const float vn = glm::dot(relativeVelocity(), n);

                        // Целевая скорость ВДОЛЬ НОРМАЛИ (нормаль смотрит от A
                        // к B, поэтому сближение — это отрицательное vn).
                        //
                        // Положительный зазор — спекулятивный контакт:
                        // разрешаем сблизиться ровно на него за шаг и ни на
                        // волос больше, то есть vn не ниже −зазор/dt. Так тело
                        // тормозит У ПОВЕРХНОСТИ, а не после того, как
                        // провалилось сквозь неё.
                        //
                        // Отрицательный зазор — проникновение: цель
                        // ПОЛОЖИТЕЛЬНАЯ, тела обязаны расходиться.
                        float target;
                        if (p.Separation > 0.0f) {
                            target = -p.Separation * invDt;
                        } else if (useBias) {
                            const float push = std::max(0.0f, -p.Separation - kSlop);
                            target = std::min(push * kBaumgarte * invDt, kMaxCorrectionSpeed);
                        } else {
                            target = 0.0f;
                        }
                        float lambda = -(vn - target) / k;
                        const float old = p.NormalImpulse;
                        p.NormalImpulse = std::max(0.0f, old + lambda);
                        lambda = p.NormalImpulse - old;
                        const glm::vec3 impulse = n * lambda;
                        ApplyImpulse(*a, -impulse, p.Position);
                        ApplyImpulse(*b, impulse, p.Position);
                    }
                }

                // --- трение по двум касательным ---
                if (c.Friction > 0.0f) {
                    const glm::vec3 tangents[2] = {t1, t2};
                    for (int k2 = 0; k2 < 2; ++k2) {
                        const glm::vec3& t = tangents[k2];
                        const glm::vec3 rta = glm::cross(ra, t), rtb = glm::cross(rb, t);
                        const float k = a->InvMass + b->InvMass +
                                        glm::dot(t, glm::cross(a->InvInertiaWorld * rta, ra)) +
                                        glm::dot(t, glm::cross(b->InvInertiaWorld * rtb, rb));
                        if (k <= kEps) continue;
                        const float vt = glm::dot(relativeVelocity(), t);
                        float lambda = -vt / k;
                        // Конус Кулона: касательный импульс не больше
                        // μ·нормального. Без ограничения трение способно
                        // остановить что угодно, и предметы прилипают к стенам.
                        const float limit = c.Friction * p.NormalImpulse;
                        const float old = p.TangentImpulse[k2];
                        p.TangentImpulse[k2] = glm::clamp(old + lambda, -limit, limit);
                        lambda = p.TangentImpulse[k2] - old;
                        const glm::vec3 impulse = t * lambda;
                        ApplyImpulse(*a, -impulse, p.Position);
                        ApplyImpulse(*b, impulse, p.Position);
                    }
                }
            }
        }
    }
}

// Отскок — последним проходом, уже после расслабления. Иначе расслабляющий
// проход, снимающий лишнюю скорость, снял бы вместе с ней и весь отскок: мяч
// с упругостью 0.8 вёл бы себя как мешок с песком.
void BuiltinWorld::ApplyRestitution() {
    for (ContactConstraint& c : m_contacts) {
        if (c.Sensor || c.Restitution <= 0.0f) continue;
        Body* a = Find(c.Key.A);
        Body* b = Find(c.Key.B);
        if (!a || !b) continue;
        const glm::vec3& n = c.M.Normal;

        for (int i = 0; i < c.M.Count; ++i) {
            builtin::ContactPoint& p = c.M.Points[i];
            // Слабый удар отскока не даёт: иначе лежащий мяч бесконечно
            // подрагивает на собственной упругости и никогда не засыпает.
            if (p.RelativeVelocity > -kRestitutionThreshold) continue;
            // И точка, которая ничего не держала, тоже: касание по скольжению
            // не должно отбрасывать.
            if (p.NormalImpulse <= 0.0f) continue;

            const glm::vec3 ra = p.Position - a->Center();
            const glm::vec3 rb = p.Position - b->Center();
            const glm::vec3 rna = glm::cross(ra, n), rnb = glm::cross(rb, n);
            const float k = a->InvMass + b->InvMass +
                            glm::dot(n, glm::cross(a->InvInertiaWorld * rna, ra)) +
                            glm::dot(n, glm::cross(b->InvInertiaWorld * rnb, rb));
            if (k <= kEps) continue;

            const float vn = glm::dot((b->LinearVelocity + glm::cross(b->AngularVelocity, rb)) -
                                          (a->LinearVelocity + glm::cross(a->AngularVelocity, ra)),
                                      n);
            float lambda = -(vn + p.RelativeVelocity * c.Restitution) / k;
            const float old = p.NormalImpulse;
            p.NormalImpulse = std::max(0.0f, old + lambda);
            lambda = p.NormalImpulse - old;
            const glm::vec3 impulse = n * lambda;
            ApplyImpulse(*a, -impulse, p.Position);
            ApplyImpulse(*b, impulse, p.Position);
        }
    }
}

// --- соединения: скоростные ограничения ---------------------------------------

void BuiltinWorld::SolveJoint(Joint& j, float dt, bool useBias) {
    Body* a = Find(j.Desc.BodyA);
    if (!a) { j.Alive = false; return; }
    Body* b = j.Desc.BodyB == kInvalidBody ? nullptr : Find(j.Desc.BodyB);
    if (j.Desc.BodyB != kInvalidBody && !b) { j.Alive = false; return; }

    // Соединённое тело не спит: пружина, которую держат, обязана продолжать
    // тянуть, даже если сама уже почти не движется.
    if (a->Movable()) Wake(*a);
    if (b && b->Movable()) Wake(*b);

    // Выталкивание позиционной ошибки — это подкачка энергии в систему, и в
    // расслабляющем проходе его быть не должно. Без этого дверь на петле
    // РАЗГОНЯЕТСЯ: каждый шаг центр съезжает с окружности по секущей, петля
    // возвращает его импульсом, и часть этого импульса остаётся в скорости.
    // За две секунды такая дверь улетает в бесконечность.
    const float invDt = (useBias && dt > 0.0f) ? 1.0f / dt : 0.0f;

    const glm::vec3 pa = a->Position + a->Rotation * j.LocalAnchorA;
    const glm::vec3 pb = b ? b->Position + b->Rotation * j.LocalAnchorB : j.LocalAnchorB;
    const glm::vec3 ra = pa - a->Center();
    const glm::vec3 rb = b ? pb - b->Center() : glm::vec3(0.0f);

    const float imA = a->InvMass, imB = b ? b->InvMass : 0.0f;
    const glm::mat3 iiA = a->InvInertiaWorld;
    const glm::mat3 iiB = b ? b->InvInertiaWorld : glm::mat3(0.0f);

    auto velA = [&]() { return a->LinearVelocity + glm::cross(a->AngularVelocity, ra); };
    auto velB = [&]() {
        return b ? b->LinearVelocity + glm::cross(b->AngularVelocity, rb) : glm::vec3(0.0f);
    };
    auto applyLinear = [&](const glm::vec3& impulse) {
        ApplyImpulse(*a, -impulse, pa);
        if (b) ApplyImpulse(*b, impulse, pb);
    };
    auto applyAngular = [&](const glm::vec3& angImpulse) {
        if (a->Movable()) a->AngularVelocity -= iiA * angImpulse;
        if (b && b->Movable()) b->AngularVelocity += iiB * angImpulse;
    };

    // Точечное ограничение (три степени свободы): якоря обязаны совпадать.
    // Общее для Fixed, Point, Hinge и Cone.
    auto solvePoint = [&]() {
        glm::mat3 k(0.0f);
        const float m = imA + imB;
        k[0][0] = m; k[1][1] = m; k[2][2] = m;
        const glm::mat3 sa = Skew(ra), sb = Skew(rb);
        k += sa * iiA * glm::transpose(sa);
        if (b) k += sb * iiB * glm::transpose(sb);
        for (int i = 0; i < 3; ++i) k[i][i] += 1e-7f;   // страховка от вырождения

        // Ошибка — на сколько разъехались якоря; скорость v = d(ошибка)/dt.
        // Цель: v_new = −ошибка·β/dt, то есть ошибка обязана УБЫВАТЬ. Знак
        // здесь стоит дорого: с обратным знаком связь каждый шаг увеличивает
        // расхождение на пятую часть, и дверь на петле улетает в бесконечность
        // за две секунды, разгоняясь по экспоненте.
        const glm::vec3 error = pb - pa;
        const glm::vec3 v = velB() - velA();
        const glm::vec3 bias = error * kBaumgarte * invDt;
        const glm::vec3 lambda = glm::inverse(k) * -(v + bias);
        j.PointImpulse += lambda;
        applyLinear(lambda);
    };

    // Полное угловое ограничение: относительный поворот держится тем, каким
    // был при создании соединения (сварка).
    auto solveAngularLock = [&](const glm::quat& targetRelative) {
        const glm::quat current = b ? glm::conjugate(a->Rotation) * b->Rotation
                                    : glm::conjugate(a->Rotation);
        glm::quat delta = glm::conjugate(targetRelative) * current;
        if (delta.w < 0.0f) delta = -delta;   // кратчайший путь
        const glm::vec3 error = a->Rotation * glm::vec3(delta.x, delta.y, delta.z) * 2.0f;

        glm::mat3 k = iiA + iiB;
        for (int i = 0; i < 3; ++i) k[i][i] += 1e-7f;
        const glm::vec3 w = (b ? b->AngularVelocity : glm::vec3(0.0f)) - a->AngularVelocity;
        const glm::vec3 lambda = glm::inverse(k) * -(w + error * kBaumgarte * invDt);
        j.AngularImpulse += lambda;
        applyAngular(lambda);
    };

    // Оставить свободным вращение только вокруг оси: гасим две поперечные
    // компоненты относительной угловой скорости.
    auto solveHingeAngular = [&](const glm::vec3& axisWorld) {
        glm::vec3 p1, p2;
        TangentBasis(axisWorld, p1, p2);
        // Ошибка перекоса: насколько ось тела B отошла от оси тела A.
        const glm::vec3 axisB = b ? b->Rotation * j.LocalAxisB : j.LocalAxisB;
        const glm::vec3 skew = glm::cross(axisWorld, axisB);

        const glm::vec3 w = (b ? b->AngularVelocity : glm::vec3(0.0f)) - a->AngularVelocity;
        glm::mat3 k = iiA + iiB;
        for (int i = 0; i < 3; ++i) k[i][i] += 1e-7f;
        const glm::mat3 kInv = glm::inverse(k);
        for (const glm::vec3& t : {p1, p2}) {
            const float kk = glm::dot(t, k * t);
            if (kk <= kEps) continue;
            const float err = glm::dot(skew, t);
            const float lambda = -(glm::dot(w, t) + err * kBaumgarte * invDt) / kk;
            applyAngular(t * lambda);
        }
        (void)kInv;
    };

    switch (j.Desc.Type) {
        case JointType::Point:
            solvePoint();
            break;

        case JointType::Fixed:
            solvePoint();
            solveAngularLock(j.RelativeRotation);
            break;

        case JointType::Distance: {
            // Одно ограничение по длине: тянет, когда трос натянут, и упирается,
            // когда сжали до минимума. Между пределами — свободно.
            glm::vec3 d = pb - pa;
            const float len = glm::length(d);
            if (len < kEps) break;
            const glm::vec3 n = d / len;
            float error = 0.0f;
            if (len > j.Desc.MaxDistance) error = len - j.Desc.MaxDistance;
            else if (len < j.Desc.MinDistance) error = len - j.Desc.MinDistance;
            else break;

            const glm::vec3 rna = glm::cross(ra, n), rnb = glm::cross(rb, n);
            const float k = imA + imB + glm::dot(n, glm::cross(iiA * rna, ra)) +
                            (b ? glm::dot(n, glm::cross(iiB * rnb, rb)) : 0.0f);
            if (k <= kEps) break;
            const float v = glm::dot(velB() - velA(), n);
            const float lambda = -(v + error * kBaumgarte * invDt) / k;
            applyLinear(n * lambda);
            break;
        }

        case JointType::Hinge: {
            const glm::vec3 axis = a->Rotation * j.LocalAxisA;
            solvePoint();
            solveHingeAngular(axis);
            if (j.Desc.UseLimits) {
                // Угол поворота вокруг оси относительно исходного положения.
                const glm::quat current = b ? glm::conjugate(a->Rotation) * b->Rotation
                                            : glm::conjugate(a->Rotation);
                glm::quat delta = glm::conjugate(j.RelativeRotation) * current;
                if (delta.w < 0.0f) delta = -delta;
                const glm::vec3 dv(delta.x, delta.y, delta.z);
                float angle = 2.0f * std::atan2(glm::dot(dv, j.LocalAxisA), delta.w);
                angle = glm::degrees(angle);

                float error = 0.0f;
                if (angle < j.Desc.MinLimit) error = angle - j.Desc.MinLimit;
                else if (angle > j.Desc.MaxLimit) error = angle - j.Desc.MaxLimit;
                if (error != 0.0f) {
                    const float kk = glm::dot(axis, (iiA + iiB) * axis);
                    if (kk > kEps) {
                        const float w = glm::dot((b ? b->AngularVelocity : glm::vec3(0.0f)) -
                                                     a->AngularVelocity, axis);
                        const float lambda =
                            -(w + glm::radians(error) * kBaumgarte * invDt) / kk;
                        applyAngular(axis * lambda);
                    }
                }
            }
            break;
        }

        case JointType::Slider: {
            // Ползун: движение свободно ВДОЛЬ оси, всё остальное заперто.
            const glm::vec3 axis = a->Rotation * j.LocalAxisA;
            solveAngularLock(j.RelativeRotation);

            glm::vec3 t1, t2;
            TangentBasis(axis, t1, t2);
            const glm::vec3 error = pb - pa;
            const glm::vec3 v = velB() - velA();
            for (const glm::vec3& t : {t1, t2}) {
                const glm::vec3 rta = glm::cross(ra, t), rtb = glm::cross(rb, t);
                const float k = imA + imB + glm::dot(t, glm::cross(iiA * rta, ra)) +
                                (b ? glm::dot(t, glm::cross(iiB * rtb, rb)) : 0.0f);
                if (k <= kEps) continue;
                const float lambda =
                    -(glm::dot(v, t) + glm::dot(error, t) * kBaumgarte * invDt) / k;
                applyLinear(t * lambda);
            }
            if (j.Desc.UseLimits) {
                const float along = glm::dot(error, axis);
                float err = 0.0f;
                if (along < j.Desc.MinLimit) err = along - j.Desc.MinLimit;
                else if (along > j.Desc.MaxLimit) err = along - j.Desc.MaxLimit;
                if (err != 0.0f) {
                    const glm::vec3 rna = glm::cross(ra, axis), rnb = glm::cross(rb, axis);
                    const float k = imA + imB + glm::dot(axis, glm::cross(iiA * rna, ra)) +
                                    (b ? glm::dot(axis, glm::cross(iiB * rnb, rb)) : 0.0f);
                    if (k > kEps) {
                        const float lambda =
                            -(glm::dot(velB() - velA(), axis) + err * kBaumgarte * invDt) / k;
                        applyLinear(axis * lambda);
                    }
                }
            }
            break;
        }

        case JointType::Cone: {
            // Конус: точка держится, а ось B не отклоняется от оси A больше чем
            // на полу-угол. Основа суставов тряпичной куклы — плечо болтается
            // свободно, но не выворачивается назад.
            solvePoint();
            const glm::vec3 axisA = a->Rotation * j.LocalAxisA;
            const glm::vec3 axisB = b ? b->Rotation * j.LocalAxisB : j.LocalAxisB;
            const float cosLimit = std::cos(glm::radians(glm::clamp(j.Desc.ConeHalfAngle, 0.0f, 179.0f)));
            const float c = glm::clamp(glm::dot(axisA, axisB), -1.0f, 1.0f);
            if (c >= cosLimit) break;   // внутри конуса — свободно

            glm::vec3 n = glm::cross(axisA, axisB);
            const float len = glm::length(n);
            if (len < 1e-5f) break;
            n /= len;
            const float error = std::acos(c) - std::acos(glm::clamp(cosLimit, -1.0f, 1.0f));
            const float kk = glm::dot(n, (iiA + iiB) * n);
            if (kk <= kEps) break;
            const float w = glm::dot((b ? b->AngularVelocity : glm::vec3(0.0f)) - a->AngularVelocity, n);
            const float lambda = -(w + error * kBaumgarte * invDt) / kk;
            applyAngular(n * std::min(lambda, 0.0f));
            break;
        }
    }
}

// --- интегрирование ------------------------------------------------------------

void BuiltinWorld::Integrate(float dt) {
    for (auto& [h, b] : m_bodies) {
        if (!b.Alive || b.Sleeping) continue;
        if (b.Type == BodyType::Static) continue;

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

} // namespace sage::physics
