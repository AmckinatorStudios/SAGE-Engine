#include "physics/builtin/Collide.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace sage::physics::builtin {
namespace {

constexpr float kEps = 1e-6f;

// Ближайшая точка отрезка к точке.
glm::vec3 ClosestOnSegment(const glm::vec3& a, const glm::vec3& b, const glm::vec3& p, float& t) {
    const glm::vec3 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    t = len2 > kEps ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return a + ab * t;
}

// Ближайшие точки двух отрезков. Классическая задача: параметры зажимаются в
// [0,1], и вырожденные случаи (точка, параллельность) разбираются отдельно —
// без этого деление на нулевой определитель даёт NaN, и тело улетает в
// бесконечность за один кадр.
void ClosestSegmentSegment(const glm::vec3& p1, const glm::vec3& q1, const glm::vec3& p2,
                           const glm::vec3& q2, float& s, float& t, glm::vec3& c1, glm::vec3& c2) {
    const glm::vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);

    if (a <= kEps && e <= kEps) { s = t = 0.0f; c1 = p1; c2 = p2; return; }
    if (a <= kEps) {
        s = 0.0f;
        t = glm::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = glm::dot(d1, r);
        if (e <= kEps) {
            t = 0.0f;
            s = glm::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = glm::dot(d1, d2);
            const float denom = a * e - b * b;
            s = denom > kEps ? glm::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) { t = 0.0f; s = glm::clamp(-c / a, 0.0f, 1.0f); }
            else if (t > 1.0f) { t = 1.0f; s = glm::clamp((b - c) / a, 0.0f, 1.0f); }
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
}

// Осевой отрезок капсулы в мире.
void CapsuleSegment(const Part& p, const Pose& pose, glm::vec3& a, glm::vec3& b) {
    const glm::vec3 axis = pose.Rotation * (p.Rotation * glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 c = pose.ToWorld(p.Position);
    a = c - axis * p.HalfHeight;
    b = c + axis * p.HalfHeight;
}

// Поза части в мире (тело + локальное смещение части).
Pose PartPose(const Part& p, const Pose& body) {
    Pose out;
    out.Rotation = body.Rotation * p.Rotation;
    out.Position = body.ToWorld(p.Position);
    return out;
}

// Ближайшая точка коробки (в её локальных координатах) к точке.
glm::vec3 ClosestOnBoxLocal(const glm::vec3& half, const glm::vec3& p) {
    return glm::clamp(p, -half, half);
}

void AddPoint(Manifold& m, const glm::vec3& pos, float sep, uint32_t id) {
    if (m.Count >= 8) return;
    ContactPoint& c = m.Points[m.Count++];
    c.Position = pos;
    c.Separation = sep;
    c.Id = id;
}

// --- сфера ------------------------------------------------------------------

bool SphereSphere(const Part& a, const Pose& pa, const Part& b, const Pose& pb, float margin,
                  Manifold& out) {
    const glm::vec3 ca = pa.ToWorld(a.Position), cb = pb.ToWorld(b.Position);
    glm::vec3 d = cb - ca;
    const float dist = glm::length(d);
    const float sep = dist - (a.Radius + b.Radius);
    if (sep > margin) return false;
    // Совпавшие центры: направление выбрать неоткуда, берём «вверх». Иначе
    // нормализация нуля даёт NaN, и оба тела исчезают из мира.
    out.Normal = dist > kEps ? d / dist : glm::vec3(0.0f, 1.0f, 0.0f);
    AddPoint(out, cb - out.Normal * b.Radius, sep, 0);
    return true;
}

bool SphereBox(const Part& s, const Pose& ps, const Part& b, const Pose& pb, float margin,
               Manifold& out, bool flip) {
    const Pose box = PartPose(b, pb);
    const glm::vec3 c = ps.ToWorld(s.Position);
    const glm::vec3 local = box.ToLocal(c);
    const glm::vec3 closest = ClosestOnBoxLocal(b.Half, local);
    glm::vec3 delta = local - closest;
    float dist = glm::length(delta);

    glm::vec3 nLocal;
    if (dist > kEps) {
        nLocal = -delta / dist;   // от сферы к коробке
    } else {
        // Центр сферы ВНУТРИ коробки: выталкиваем по ближайшей грани. Без
        // этой ветки шарик, провалившийся внутрь ящика, остаётся там навсегда.
        const glm::vec3 d = b.Half - glm::abs(local);
        const int axis = (d.x < d.y && d.x < d.z) ? 0 : (d.y < d.z ? 1 : 2);
        nLocal = glm::vec3(0.0f);
        nLocal[axis] = local[axis] > 0.0f ? -1.0f : 1.0f;
        dist = -d[axis];
    }
    const float sep = dist - s.Radius;
    if (sep > margin) return false;

    const glm::vec3 nWorld = box.Rotation * nLocal;      // от сферы к коробке
    const glm::vec3 point = box.ToWorld(closest);
    out.Normal = flip ? -nWorld : nWorld;                // всегда от A к B
    AddPoint(out, point, sep, 0);
    return true;
}

bool SphereCapsule(const Part& s, const Pose& ps, const Part& c, const Pose& pc, float margin,
                   Manifold& out, bool flip) {
    glm::vec3 a, b;
    CapsuleSegment(c, pc, a, b);
    const glm::vec3 center = ps.ToWorld(s.Position);
    float t;
    const glm::vec3 onAxis = ClosestOnSegment(a, b, center, t);
    glm::vec3 d = onAxis - center;
    const float dist = glm::length(d);
    const float sep = dist - (s.Radius + c.Radius);
    if (sep > margin) return false;
    const glm::vec3 n = dist > kEps ? d / dist : glm::vec3(0.0f, 1.0f, 0.0f);  // от сферы к капсуле
    out.Normal = flip ? -n : n;
    AddPoint(out, onAxis - n * c.Radius, sep, 0);
    return true;
}

// --- капсула-капсула ---------------------------------------------------------

bool CapsuleCapsule(const Part& a, const Pose& pa, const Part& b, const Pose& pb, float margin,
                    Manifold& out) {
    glm::vec3 a0, a1, b0, b1;
    CapsuleSegment(a, pa, a0, a1);
    CapsuleSegment(b, pb, b0, b1);

    float s, t;
    glm::vec3 ca, cb;
    ClosestSegmentSegment(a0, a1, b0, b1, s, t, ca, cb);
    glm::vec3 d = cb - ca;
    float dist = glm::length(d);
    const float radii = a.Radius + b.Radius;
    if (dist - radii > margin) return false;

    const glm::vec3 axisA = a1 - a0, axisB = b1 - b0;
    glm::vec3 n = dist > kEps ? d / dist : glm::vec3(0.0f, 1.0f, 0.0f);
    if (dist <= kEps) {
        // Оси пересеклись: нормаль ищем поперёк обеих осей, иначе капсулы
        // расталкиваются вдоль себя и «протыкают» друг друга.
        const glm::vec3 cross = glm::cross(axisA, axisB);
        if (glm::dot(cross, cross) > kEps) n = glm::normalize(cross);
    }
    out.Normal = n;

    // Две почти параллельные капсулы (бревно на бревне) обязаны дать ДВЕ точки:
    // с одной они катаются друг по другу, как карандаши, и никогда не
    // успокаиваются.
    const float la = glm::length(axisA), lb = glm::length(axisB);
    bool parallel = false;
    if (la > kEps && lb > kEps) {
        const float cosang = std::abs(glm::dot(axisA / la, axisB / lb));
        parallel = cosang > 0.995f;
    }
    if (!parallel) {
        AddPoint(out, cb - n * b.Radius, dist - radii, 0);
        return true;
    }

    // Общий участок вдоль оси A: концы отрезка B, спроецированные на A.
    const glm::vec3 dirA = axisA / la;
    float t0 = glm::clamp(glm::dot(b0 - a0, dirA) / la, 0.0f, 1.0f);
    float t1 = glm::clamp(glm::dot(b1 - a0, dirA) / la, 0.0f, 1.0f);
    if (t0 > t1) std::swap(t0, t1);
    const glm::vec3 e0 = a0 + axisA * t0;
    const glm::vec3 e1 = a0 + axisA * t1;
    for (int i = 0; i < 2; ++i) {
        const glm::vec3 onA = i == 0 ? e0 : e1;
        float tb;
        const glm::vec3 onB = ClosestOnSegment(b0, b1, onA, tb);
        const float sep = glm::dot(onB - onA, n) - radii;
        if (sep <= margin) AddPoint(out, onB - n * b.Radius, sep, (uint32_t)i);
    }
    if (out.Count == 0) AddPoint(out, cb - n * b.Radius, dist - radii, 0);
    return true;
}

// --- отсечение многоугольника полуплоскостями (Sutherland-Hodgman) -----------

int ClipPolygonByPlane(const glm::vec3* in, int count, const glm::vec3& planeN, float planeD,
                       glm::vec3* out) {
    int n = 0;
    for (int i = 0; i < count; ++i) {
        const glm::vec3& a = in[i];
        const glm::vec3& b = in[(i + 1) % count];
        const float da = glm::dot(planeN, a) - planeD;
        const float db = glm::dot(planeN, b) - planeD;
        if (da <= 0.0f) out[n++] = a;
        if ((da > 0.0f) != (db > 0.0f)) {
            const float u = da / (da - db);
            out[n++] = a + (b - a) * u;
        }
        if (n >= 14) break;   // страховка от вырожденного ввода
    }
    return n;
}

// Грань коробки, наиболее обращённая вдоль dir (в мировых координатах).
// Возвращает четыре угла и номер оси/знак — они и дают устойчивый Id контакта.
void BoxIncidentFace(const Part& box, const Pose& pose, const glm::vec3& dir, glm::vec3* corners,
                     int& axisOut, int& signOut) {
    const glm::vec3 local = pose.DirToLocal(dir);
    int axis = 0;
    float best = std::abs(local.x);
    if (std::abs(local.y) > best) { axis = 1; best = std::abs(local.y); }
    if (std::abs(local.z) > best) { axis = 2; }
    const int sign = local[axis] > 0.0f ? 1 : -1;

    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
    static const float su[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    static const float sv[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
    for (int i = 0; i < 4; ++i) {
        glm::vec3 c(0.0f);
        c[axis] = box.Half[axis] * (float)sign;
        c[u] = box.Half[u] * su[i];
        c[v] = box.Half[v] * sv[i];
        corners[i] = pose.ToWorld(c);
    }
    axisOut = axis;
    signOut = sign;
}

// --- коробка-коробка (теорема о разделяющей оси) -----------------------------

bool BoxBox(const Part& a, const Pose& poseA, const Part& b, const Pose& poseB, float margin,
            Manifold& out) {
    const Pose A = PartPose(a, poseA);
    const Pose B = PartPose(b, poseB);

    // Оси коробок в мире.
    glm::vec3 ax[3], bx[3];
    for (int i = 0; i < 3; ++i) {
        glm::vec3 e(0.0f); e[i] = 1.0f;
        ax[i] = A.Rotation * e;
        bx[i] = B.Rotation * e;
    }
    const glm::vec3 t = B.Position - A.Position;

    // R[i][j] = ax[i] · bx[j]; absR — с добавкой на случай почти параллельных
    // рёбер: без неё векторное произведение сонаправленных осей даёт нулевую
    // ось, нормализация которой — деление на ноль.
    float R[3][3], absR[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            R[i][j] = glm::dot(ax[i], bx[j]);
            absR[i][j] = std::abs(R[i][j]) + 1e-5f;
        }

    float bestSep = -FLT_MAX;
    int bestKind = -1;      // 0 — грань A, 1 — грань B, 2 — ребро-ребро
    int bestIndex = 0;      // ось грани или i*3+j для рёбер
    glm::vec3 bestAxis(0.0f);

    auto consider = [&](const glm::vec3& axis, float sep, int kind, int index) {
        if (sep > bestSep) { bestSep = sep; bestAxis = axis; bestKind = kind; bestIndex = index; }
    };

    // Грани A.
    for (int i = 0; i < 3; ++i) {
        const float ra = a.Half[i];
        const float rb = b.Half.x * absR[i][0] + b.Half.y * absR[i][1] + b.Half.z * absR[i][2];
        const float dist = glm::dot(t, ax[i]);
        const float sep = std::abs(dist) - (ra + rb);
        if (sep > margin) return false;
        consider(dist >= 0.0f ? ax[i] : -ax[i], sep, 0, i);
    }
    // Грани B.
    for (int j = 0; j < 3; ++j) {
        const float rb = b.Half[j];
        const float ra = a.Half.x * absR[0][j] + a.Half.y * absR[1][j] + a.Half.z * absR[2][j];
        const float dist = glm::dot(t, bx[j]);
        const float sep = std::abs(dist) - (ra + rb);
        if (sep > margin) return false;
        consider(dist >= 0.0f ? bx[j] : -bx[j], sep, 1, j);
    }
    // Девять осей «ребро A x ребро B». Без них коробки, столкнувшиеся рёбрами
    // крест-накрест, считаются пересекающимися и вминаются друг в друга.
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const glm::vec3 raw = glm::cross(ax[i], bx[j]);
            const float len = glm::length(raw);
            if (len < 1e-4f) continue;        // оси сонаправлены — ось вырождена
            const int i1 = (i + 1) % 3, i2 = (i + 2) % 3;
            const int j1 = (j + 1) % 3, j2 = (j + 2) % 3;
            // Проекции полурёбер считаются для НЕНОРМИРОВАННОЙ оси — так
            // устроены эти формулы: absR уже несёт в себе её длину. Значит и
            // расстояние надо брать по ней же, а делить на длину только
            // готовое перекрытие.
            //
            // Стоила эта тонкость дорого: с нормированной осью и
            // ненормированными радиусами почти сонаправленные коробки (ящик
            // на полу, повёрнутый на доли градуса) давали ЛОЖНУЮ разделяющую
            // ось с огромным зазором — и контакт пропадал совсем. Внешне это
            // выглядело как «предметы проваливаются сквозь пол, но не всегда».
            const float ra = a.Half[i1] * absR[i2][j] + a.Half[i2] * absR[i1][j];
            const float rb = b.Half[j1] * absR[i][j2] + b.Half[j2] * absR[i][j1];
            const float dist = glm::dot(t, raw);
            const float sep = (std::abs(dist) - (ra + rb)) / len;
            if (sep > margin) return false;
            const glm::vec3 axis = (dist >= 0.0f ? raw : -raw) / len;
            // Грани предпочтительнее рёбер при почти равной глубине: иначе
            // лежащий ящик каждый кадр перескакивает между «гранью» и
            // «ребром», нормаль дёргается, и он дрожит на месте.
            consider(axis, sep - 1e-3f, 2, i * 3 + j);
        }
    }
    if (bestKind < 0) return false;

    out.Normal = bestAxis;   // от A к B

    if (bestKind == 2) {
        // Ребро-ребро: одна точка в месте наибольшего сближения рёбер.
        const int i = bestIndex / 3, j = bestIndex % 3;
        // Опорные точки: угол каждой коробки, крайний вдоль нормали.
        glm::vec3 pa = A.Position, pb = B.Position;
        for (int k = 0; k < 3; ++k) {
            if (k != i) pa += ax[k] * (glm::dot(ax[k], bestAxis) > 0.0f ? a.Half[k] : -a.Half[k]);
            if (k != j) pb += bx[k] * (glm::dot(bx[k], bestAxis) < 0.0f ? b.Half[k] : -b.Half[k]);
        }
        float s, u;
        glm::vec3 c1, c2;
        ClosestSegmentSegment(pa - ax[i] * a.Half[i], pa + ax[i] * a.Half[i],
                              pb - bx[j] * b.Half[j], pb + bx[j] * b.Half[j], s, u, c1, c2);
        AddPoint(out, (c1 + c2) * 0.5f, bestSep, 0x8000u | (uint32_t)bestIndex);
        return true;
    }

    // Грань-грань: опорная грань у той коробки, чья ось выиграла, встречная —
    // у другой; встречная отсекается боковыми плоскостями опорной.
    const bool refIsA = bestKind == 0;
    const Part& refBox = refIsA ? a : b;
    const Pose& refPose = refIsA ? A : B;
    const Part& incBox = refIsA ? b : a;
    const Pose& incPose = refIsA ? B : A;
    const glm::vec3 refNormal = refIsA ? bestAxis : -bestAxis;   // наружу из опорной коробки

    int refAxis, refSign;
    glm::vec3 refCorners[4];
    BoxIncidentFace(refBox, refPose, refNormal, refCorners, refAxis, refSign);

    int incAxis, incSign;
    glm::vec3 poly[16], clipped[16];
    BoxIncidentFace(incBox, incPose, -refNormal, poly, incAxis, incSign);
    int count = 4;

    // Боковые плоскости опорной грани: четыре, по двум осям.
    const int u = (refAxis + 1) % 3, v = (refAxis + 2) % 3;
    glm::vec3 refU, refV;
    {
        glm::vec3 eu(0.0f); eu[u] = 1.0f;
        glm::vec3 ev(0.0f); ev[v] = 1.0f;
        refU = refPose.Rotation * eu;
        refV = refPose.Rotation * ev;
    }
    const float cu = glm::dot(refPose.Position, refU), cv = glm::dot(refPose.Position, refV);
    const glm::vec3 planeN[4] = {refU, -refU, refV, -refV};
    const float planeD[4] = {cu + refBox.Half[u], -(cu - refBox.Half[u]), cv + refBox.Half[v],
                             -(cv - refBox.Half[v])};
    for (int i = 0; i < 4; ++i) {
        count = ClipPolygonByPlane(poly, count, planeN[i], planeD[i], clipped);
        if (count == 0) return out.Count > 0;
        std::copy(clipped, clipped + count, poly);
    }

    // Оставляем точки, лежащие не дальше margin от плоскости опорной грани.
    const float refPlaneD = glm::dot(refPose.Position, refNormal) + refBox.Half[refAxis];
    for (int i = 0; i < count && out.Count < 8; ++i) {
        const float sep = glm::dot(refNormal, poly[i]) - refPlaneD;
        if (sep > margin) continue;
        // Точку кладём на поверхность ОПОРНОЙ грани — так глубина и точка
        // приложения импульса согласованы между собой.
        const glm::vec3 onFace = poly[i] - refNormal * sep;
        // Id: какая грань какой коробки встретилась. Устойчив, пока тела не
        // повернулись настолько, чтобы сменить грань, — то есть ровно столько,
        // сколько имеет смысл переносить импульс.
        const uint32_t id = (uint32_t)((refAxis * 2 + (refSign > 0 ? 1 : 0)) * 64 +
                                       (incAxis * 2 + (incSign > 0 ? 1 : 0)) * 8 + i);
        AddPoint(out, onFace, sep, id);
    }
    if (out.Count == 0) {
        // Отсечение вырезало всё (скользящее касание) — оставим одну точку,
        // иначе тела разъедутся сквозь друг друга без единого контакта.
        AddPoint(out, refPose.Position + refNormal * refBox.Half[refAxis], bestSep, 0);
    }
    return true;
}

// --- капсула-коробка ---------------------------------------------------------

bool CapsuleBox(const Part& cap, const Pose& pcap, const Part& box, const Pose& pbox, float margin,
                Manifold& out, bool flip) {
    const Pose B = PartPose(box, pbox);
    glm::vec3 s0, s1;
    CapsuleSegment(cap, pcap, s0, s1);

    // Ближайшая пара «отрезок — коробка». Итерация: точка на коробке ->
    // ближайшая к ней точка отрезка -> снова точка на коробке. Четыре прохода
    // сходятся с запасом; замкнутой формулы у этой задачи нет.
    glm::vec3 onSeg = (s0 + s1) * 0.5f;
    glm::vec3 onBox = B.ToWorld(ClosestOnBoxLocal(box.Half, B.ToLocal(onSeg)));
    for (int i = 0; i < 4; ++i) {
        float t;
        onSeg = ClosestOnSegment(s0, s1, onBox, t);
        onBox = B.ToWorld(ClosestOnBoxLocal(box.Half, B.ToLocal(onSeg)));
    }

    glm::vec3 d = onBox - onSeg;
    float dist = glm::length(d);
    glm::vec3 n;   // от капсулы к коробке
    if (dist > kEps) {
        n = d / dist;
    } else {
        // Ось капсулы внутри коробки — выталкиваем по ближайшей грани.
        const glm::vec3 local = B.ToLocal(onSeg);
        const glm::vec3 gap = box.Half - glm::abs(local);
        const int axis = (gap.x < gap.y && gap.x < gap.z) ? 0 : (gap.y < gap.z ? 1 : 2);
        glm::vec3 nl(0.0f);
        nl[axis] = local[axis] > 0.0f ? -1.0f : 1.0f;
        n = B.Rotation * nl;
        dist = -gap[axis];
    }
    if (dist - cap.Radius > margin) return false;

    out.Normal = flip ? -n : n;

    // Манифольд: капсула, лежащая на грани, обязана опираться ДВУМЯ точками —
    // иначе она катается по плоскому полу, как бревно, и не останавливается.
    // Проверяем оба конца оси: если оба почти на одной глубине, оба и берём.
    const glm::vec3 refPoint = onBox;
    const float baseSep = dist - cap.Radius;
    int added = 0;
    for (int i = 0; i < 2; ++i) {
        const glm::vec3 end = i == 0 ? s0 : s1;
        const glm::vec3 onBoxEnd = B.ToWorld(ClosestOnBoxLocal(box.Half, B.ToLocal(end)));
        const float sep = glm::dot(onBoxEnd - end, n) - cap.Radius;
        if (sep > margin || sep > baseSep + 0.05f) continue;
        const glm::vec3 p = flip ? end + n * cap.Radius : onBoxEnd;
        AddPoint(out, p, sep, (uint32_t)i);
        ++added;
    }
    if (added == 0) {
        const glm::vec3 p = flip ? onSeg + n * cap.Radius : refPoint;
        AddPoint(out, p, baseSep, 2);
    }
    return true;
}

} // namespace

bool Collide(const Part& a, const Pose& poseA, const Part& b, const Pose& poseB, float margin,
             Manifold& out) {
    out.Count = 0;
    const ShapeType sa = a.Shape, sb = b.Shape;

    if (sa == ShapeType::Sphere && sb == ShapeType::Sphere) return SphereSphere(a, poseA, b, poseB, margin, out);
    if (sa == ShapeType::Sphere && sb == ShapeType::Box)    return SphereBox(a, poseA, b, poseB, margin, out, false);
    if (sa == ShapeType::Box && sb == ShapeType::Sphere)    return SphereBox(b, poseB, a, poseA, margin, out, true);
    if (sa == ShapeType::Sphere && sb == ShapeType::Capsule) return SphereCapsule(a, poseA, b, poseB, margin, out, false);
    if (sa == ShapeType::Capsule && sb == ShapeType::Sphere) return SphereCapsule(b, poseB, a, poseA, margin, out, true);
    if (sa == ShapeType::Capsule && sb == ShapeType::Capsule) return CapsuleCapsule(a, poseA, b, poseB, margin, out);
    if (sa == ShapeType::Capsule && sb == ShapeType::Box)   return CapsuleBox(a, poseA, b, poseB, margin, out, false);
    if (sa == ShapeType::Box && sb == ShapeType::Capsule)   return CapsuleBox(b, poseB, a, poseA, margin, out, true);
    return BoxBox(a, poseA, b, poseB, margin, out);
}

void PartAabb(const Part& p, const Pose& body, glm::vec3& min, glm::vec3& max) {
    const Pose w = PartPose(p, body);
    switch (p.Shape) {
        case ShapeType::Sphere:
            min = w.Position - glm::vec3(p.Radius);
            max = w.Position + glm::vec3(p.Radius);
            return;
        case ShapeType::Capsule: {
            const glm::vec3 axis = w.Rotation * glm::vec3(0.0f, 1.0f, 0.0f) * p.HalfHeight;
            const glm::vec3 a = w.Position - axis, b = w.Position + axis;
            min = glm::min(a, b) - glm::vec3(p.Radius);
            max = glm::max(a, b) + glm::vec3(p.Radius);
            return;
        }
        default: {
            // Габарит повёрнутой коробки: сумма проекций полурёбер на каждую
            // мировую ось. Брать полуразмеры как есть было бы неверно —
            // повёрнутый ящик занимает больше места, и широкая фаза пропускала
            // бы его контакты.
            const glm::mat3 r = glm::mat3_cast(w.Rotation);
            glm::vec3 ext;
            for (int i = 0; i < 3; ++i)
                ext[i] = std::abs(r[0][i]) * p.Half.x + std::abs(r[1][i]) * p.Half.y +
                         std::abs(r[2][i]) * p.Half.z;
            min = w.Position - ext;
            max = w.Position + ext;
            return;
        }
    }
}

bool RaycastPart(const Part& p, const Pose& body, const glm::vec3& origin, const glm::vec3& dir,
                 float maxDistance, float& outDistance, glm::vec3& outNormal) {
    const Pose w = PartPose(p, body);
    switch (p.Shape) {
        case ShapeType::Sphere: {
            const glm::vec3 m = origin - w.Position;
            const float b = glm::dot(m, dir);
            const float c = glm::dot(m, m) - p.Radius * p.Radius;
            if (c > 0.0f && b > 0.0f) return false;
            const float disc = b * b - c;
            if (disc < 0.0f) return false;
            float t = -b - std::sqrt(disc);
            if (t < 0.0f) t = 0.0f;
            if (t > maxDistance) return false;
            outDistance = t;
            outNormal = glm::normalize(origin + dir * t - w.Position);
            return true;
        }
        case ShapeType::Capsule: {
            // Капсула = цилиндр + две шапки. Считаем в её локальных
            // координатах, где ось всегда Y: так задача сводится к
            // квадратному уравнению в плоскости XZ плюс проверка шапок.
            const glm::vec3 o = w.ToLocal(origin);
            const glm::vec3 d = w.DirToLocal(dir);
            const float r2 = p.Radius * p.Radius;
            float best = maxDistance;
            bool hit = false;
            glm::vec3 n(0.0f);

            const float a = d.x * d.x + d.z * d.z;
            if (a > kEps) {
                const float b = 2.0f * (o.x * d.x + o.z * d.z);
                const float c = o.x * o.x + o.z * o.z - r2;
                const float disc = b * b - 4.0f * a * c;
                if (disc >= 0.0f) {
                    const float t = (-b - std::sqrt(disc)) / (2.0f * a);
                    if (t >= 0.0f && t < best) {
                        const float y = o.y + d.y * t;
                        if (std::abs(y) <= p.HalfHeight) {
                            best = t; hit = true;
                            n = glm::normalize(glm::vec3(o.x + d.x * t, 0.0f, o.z + d.z * t));
                        }
                    }
                }
            }
            for (int s = -1; s <= 1; s += 2) {
                const glm::vec3 cap(0.0f, p.HalfHeight * (float)s, 0.0f);
                const glm::vec3 m = o - cap;
                const float b = glm::dot(m, d);
                const float c = glm::dot(m, m) - r2;
                const float disc = b * b - c;
                if (disc < 0.0f) continue;
                const float t = -b - std::sqrt(disc);
                if (t >= 0.0f && t < best) {
                    best = t; hit = true;
                    n = glm::normalize(o + d * t - cap);
                }
            }
            if (!hit) return false;
            outDistance = best;
            outNormal = w.Rotation * n;
            return true;
        }
        default: {
            // Коробка: метод плит в её локальных координатах — повёрнутый ящик
            // и должен протыкаться по своим граням, а не по осевому габариту.
            const glm::vec3 o = w.ToLocal(origin);
            const glm::vec3 d = w.DirToLocal(dir);
            float tmin = 0.0f, tmax = maxDistance;
            int axis = 0;
            float sign = 1.0f;
            for (int i = 0; i < 3; ++i) {
                if (std::abs(d[i]) < kEps) {
                    if (o[i] < -p.Half[i] || o[i] > p.Half[i]) return false;
                    continue;
                }
                const float inv = 1.0f / d[i];
                float t1 = (-p.Half[i] - o[i]) * inv;
                float t2 = (p.Half[i] - o[i]) * inv;
                float s = -1.0f;
                if (t1 > t2) { std::swap(t1, t2); s = 1.0f; }
                if (t1 > tmin) { tmin = t1; axis = i; sign = s; }
                tmax = std::min(tmax, t2);
                if (tmin > tmax) return false;
            }
            outDistance = tmin;
            glm::vec3 nl(0.0f);
            nl[axis] = sign;
            outNormal = w.Rotation * nl;
            return true;
        }
    }
}

bool OverlapSpherePart(const Part& p, const Pose& body, const glm::vec3& center, float radius) {
    const Pose w = PartPose(p, body);
    switch (p.Shape) {
        case ShapeType::Sphere: {
            const float r = radius + p.Radius;
            return glm::dot(center - w.Position, center - w.Position) <= r * r;
        }
        case ShapeType::Capsule: {
            glm::vec3 a, b;
            CapsuleSegment(p, body, a, b);
            float t;
            const glm::vec3 c = ClosestOnSegment(a, b, center, t);
            const float r = radius + p.Radius;
            return glm::dot(center - c, center - c) <= r * r;
        }
        default: {
            const glm::vec3 local = w.ToLocal(center);
            const glm::vec3 closest = ClosestOnBoxLocal(p.Half, local);
            const glm::vec3 d = local - closest;
            return glm::dot(d, d) <= radius * radius;
        }
    }
}

void PartInertiaUnitMass(const Part& p, glm::mat3& outInertia, glm::vec3& outCenter,
                         float& outVolume) {
    outCenter = p.Position;
    glm::mat3 local(0.0f);
    switch (p.Shape) {
        case ShapeType::Sphere: {
            const float i = 0.4f * p.Radius * p.Radius;
            local = glm::mat3(i);
            outVolume = (4.0f / 3.0f) * 3.14159265f * p.Radius * p.Radius * p.Radius;
            break;
        }
        case ShapeType::Capsule: {
            // Цилиндр + две полусферы, каждая со своей долей массы. Считать
            // капсулу шаром — значит получить капсулу, которая одинаково легко
            // крутится вдоль и поперёк себя; для длинной капсулы разница
            // десятикратная и видна невооружённым глазом.
            const float r = p.Radius, h = p.HalfHeight * 2.0f;
            const float vc = 3.14159265f * r * r * h;
            const float vs = (4.0f / 3.0f) * 3.14159265f * r * r * r;
            const float vol = vc + vs;
            outVolume = vol;
            const float mc = vc / vol, ms = vs / vol;
            const float iyy = mc * 0.5f * r * r + ms * 0.4f * r * r;
            float ixx = mc * (h * h / 12.0f + r * r * 0.25f);
            ixx += ms * (0.4f * r * r + 0.375f * r * h + 0.25f * h * h);
            local = glm::mat3(0.0f);
            local[0][0] = ixx; local[1][1] = iyy; local[2][2] = ixx;
            break;
        }
        default: {
            const glm::vec3 s = p.Half * 2.0f;
            local = glm::mat3(0.0f);
            local[0][0] = (s.y * s.y + s.z * s.z) / 12.0f;
            local[1][1] = (s.x * s.x + s.z * s.z) / 12.0f;
            local[2][2] = (s.x * s.x + s.y * s.y) / 12.0f;
            outVolume = s.x * s.y * s.z;
            break;
        }
    }
    // Поворот части: I' = R · I · Rᵀ. Без него повёрнутая часть составного
    // тела сообщала бы телу инерцию так, будто она стоит ровно.
    const glm::mat3 r = glm::mat3_cast(p.Rotation);
    outInertia = r * local * glm::transpose(r);
}

} // namespace sage::physics::builtin
