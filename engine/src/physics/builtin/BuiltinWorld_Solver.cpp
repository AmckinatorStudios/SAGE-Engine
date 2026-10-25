#include "physics/builtin/BuiltinWorld.h"
#include "physics/builtin/BuiltinWorldDetail.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/core/Log.h"


// Решатель встроенной физики: контакты, скорости, отскок, соединения (см. BuiltinWorld.h).

namespace sage::physics {

using namespace builtin_detail;

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
                c.A = a;
                c.B = b;
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
        Body* a = c.A;
        Body* b = c.B;
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
            Body* a = c.A;
            Body* b = c.B;
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
        Body* a = c.A;
        Body* b = c.B;
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

} // namespace sage::physics
