#include "sage/anim/IK.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/constants.hpp>

namespace sage::anim {

namespace {

constexpr float kEps = 1e-6f;

glm::vec3 PositionOf(const glm::mat4& m) { return glm::vec3(m[3]); }

// Поворот, переводящий направление from в направление to. Кратчайшая дуга.
glm::quat RotationBetween(const glm::vec3& from, const glm::vec3& to) {
    const glm::vec3 a = glm::normalize(from);
    const glm::vec3 b = glm::normalize(to);
    const float dot = glm::clamp(glm::dot(a, b), -1.0f, 1.0f);
    if (dot > 1.0f - 1e-6f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // уже совпадают
    if (dot < -1.0f + 1e-6f) {
        // Противоположные направления: ось поворота не выводится из векторного
        // произведения (оно нулевое) — берём любую перпендикулярную.
        glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), a);
        if (glm::length2(axis) < 1e-6f) axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), a);
        return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
    }
    const glm::vec3 axis = glm::normalize(glm::cross(a, b));
    return glm::angleAxis(std::acos(dot), axis);
}

// Глобальный поворот кости из её глобальной матрицы (масштаб снимается).
glm::quat GlobalRotation(const glm::mat4& m) {
    glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    const float sx = glm::length(c0), sy = glm::length(c1), sz = glm::length(c2);
    if (sx < kEps || sy < kEps || sz < kEps) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return glm::normalize(glm::quat_cast(glm::mat3(c0 / sx, c1 / sy, c2 / sz)));
}

// Глобальный поворот РОДИТЕЛЯ кости (единичный у корня) — нужен, чтобы
// перевести посчитанный глобальный поворот обратно в локальный.
glm::quat ParentGlobalRotation(const Skeleton& skeleton, const std::vector<glm::mat4>& globals,
                               int joint) {
    const int parent = skeleton.Joints[(size_t)joint].Parent;
    if (parent < 0 || parent >= (int)globals.size()) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return GlobalRotation(globals[(size_t)parent]);
}

} // namespace

std::vector<int> ChainFromEnd(const Skeleton& skeleton, int end, int length) {
    if (end < 0 || end >= skeleton.Count() || length < 2) return {};
    std::vector<int> chain;
    int current = end;
    for (int i = 0; i < length; ++i) {
        if (current < 0) return {}; // упёрлись в корень раньше, чем набрали длину
        chain.push_back(current);
        current = skeleton.Joints[(size_t)current].Parent;
    }
    std::reverse(chain.begin(), chain.end()); // от корня к концу
    return chain;
}

IKResult SolveTwoBone(const Skeleton& skeleton, const std::vector<glm::mat4>& globals,
                      const TwoBoneChain& chain, const glm::vec3& target,
                      const glm::vec3* poleTarget) {
    IKResult result;
    if (!chain.Valid() || (int)globals.size() != skeleton.Count()) return result;
    if (chain.Root >= skeleton.Count() || chain.Middle >= skeleton.Count() ||
        chain.End >= skeleton.Count()) {
        return result;
    }

    const glm::vec3 a = PositionOf(globals[(size_t)chain.Root]);
    const glm::vec3 b = PositionOf(globals[(size_t)chain.Middle]);
    const glm::vec3 c = PositionOf(globals[(size_t)chain.End]);

    const float lab = glm::length(b - a);
    const float lcb = glm::length(b - c);
    if (lab < kEps || lcb < kEps) return result; // вырожденная цепочка
    if (glm::length(target - a) < kEps) return result; // цель в корне — направления нет

    // Расстояние до цели зажимаем в достижимый диапазон: кости не тянутся.
    // Персонаж вытягивается к недостижимой цели, но длина конечности физична.
    const float rawDistance = glm::length(target - a);
    const float minReach = std::abs(lab - lcb) + 1e-4f;
    const float maxReach = lab + lcb - 1e-4f;
    result.Reached = rawDistance <= maxReach && rawDistance >= minReach;
    const float lat = glm::clamp(rawDistance, minReach, maxReach);

    // Считаем ДЕЛЬТЫ углов, а не абсолютные повороты: текущий поворот кости
    // несёт ещё и скручивание вдоль своей оси (у настоящих ригов оно есть
    // всегда), и абсолютная пересборка это скручивание бы потеряла.
    const glm::vec3 dirAC = glm::normalize(c - a);
    const glm::vec3 dirAB = glm::normalize(b - a);
    const glm::vec3 dirAT = glm::normalize(target - a);

    const float angleAB_now = std::acos(glm::clamp(glm::dot(dirAC, dirAB), -1.0f, 1.0f));
    const float angleBC_now = std::acos(
        glm::clamp(glm::dot(glm::normalize(a - b), glm::normalize(c - b)), -1.0f, 1.0f));
    const float angleAT_now = std::acos(glm::clamp(glm::dot(dirAC, dirAT), -1.0f, 1.0f));

    // Желаемые углы — теорема косинусов для треугольника со сторонами lab, lcb, lat.
    const float angleAB_want = std::acos(
        glm::clamp((lcb * lcb - lab * lab - lat * lat) / (-2.0f * lab * lat), -1.0f, 1.0f));
    const float angleBC_want = std::acos(
        glm::clamp((lat * lat - lab * lab - lcb * lcb) / (-2.0f * lab * lcb), -1.0f, 1.0f));

    // Ось сгиба (нормаль плоскости, в которой работает сустав) и ось доворота
    // всей цепочки к цели.
    glm::vec3 axisBend;
    glm::vec3 axisSwing = glm::cross(dirAC, target - a);

    const glm::vec3 poleFrom = poleTarget ? (*poleTarget - a) : (b - a);
    axisBend = glm::cross(c - a, poleFrom);
    if (glm::length2(axisBend) < 1e-8f) {
        // Цепочка выпрямлена в струну (или полюс лежит на её оси) — плоскость
        // сгиба не определена. Гнём в сторону ЦЕЛИ: это единственный выбор,
        // который не выглядит произвольным. Ровно этот случай возникает у
        // персонажа в бинд-позе с прямыми руками и ногами.
        axisBend = axisSwing;
    }
    if (glm::length2(axisBend) < 1e-8f) {
        // И цель на той же прямой: сгибать некуда, берём любую перпендикулярную.
        axisBend = glm::cross(dirAC, glm::vec3(0.0f, 1.0f, 0.0f));
        if (glm::length2(axisBend) < 1e-8f) axisBend = glm::cross(dirAC, glm::vec3(1.0f, 0.0f, 0.0f));
    }
    if (glm::length2(axisSwing) < 1e-8f) axisSwing = axisBend; // цель уже на оси цепочки
    axisBend = glm::normalize(axisBend);
    axisSwing = glm::normalize(axisSwing);

    // Оси заданы в пространстве модели, а повороты домножаются к ЛОКАЛЬНЫМ
    // поворотам костей — поэтому оси переводятся в систему координат кости.
    const glm::quat globalRoot = GlobalRotation(globals[(size_t)chain.Root]);
    const glm::quat globalMid = GlobalRotation(globals[(size_t)chain.Middle]);
    const glm::quat parentRoot = ParentGlobalRotation(skeleton, globals, chain.Root);

    const glm::quat localRoot = glm::normalize(glm::inverse(parentRoot) * globalRoot);
    const glm::quat localMid = glm::normalize(glm::inverse(globalRoot) * globalMid);

    const glm::quat bendRoot =
        glm::angleAxis(angleAB_want - angleAB_now, glm::inverse(globalRoot) * axisBend);
    const glm::quat bendMid =
        glm::angleAxis(angleBC_want - angleBC_now, glm::inverse(globalMid) * axisBend);
    const glm::quat swing = glm::angleAxis(angleAT_now, glm::inverse(globalRoot) * axisSwing);

    // ПОРЯДОК: сначала сгиб, потом доворот к цели. В произведении это значит
    // swing слева от bendRoot — постумножение применяет правый множитель к
    // вектору первым. Обратный порядок разваливает решение ровно в самом
    // частом случае: у выпрямленной руки ось сгиба совпадает с осью кости, и
    // после доворота сгибать уже нечего вокруг чего — рука остаётся прямой.
    result.Joints = {chain.Root, chain.Middle};
    result.Rotations = {glm::normalize(localRoot * swing * bendRoot),
                        glm::normalize(localMid * bendMid)};
    result.Solved = true;
    return result;
}

IKResult SolveChain(const Skeleton& skeleton, const std::vector<glm::mat4>& globals,
                    const std::vector<int>& chain, const glm::vec3& target, int iterations,
                    float tolerance) {
    IKResult result;
    if (chain.size() < 2 || (int)globals.size() != skeleton.Count()) return result;
    for (int j : chain) {
        if (j < 0 || j >= skeleton.Count()) return result;
    }

    // FABRIK работает с ТОЧКАМИ, а не с матрицами: суставы двигаются как узлы
    // цепи, и только в конце из их положений выводятся повороты костей.
    const size_t n = chain.size();
    std::vector<glm::vec3> points(n);
    for (size_t i = 0; i < n; ++i) points[i] = PositionOf(globals[(size_t)chain[i]]);

    std::vector<float> lengths(n - 1);
    float totalLength = 0.0f;
    for (size_t i = 0; i + 1 < n; ++i) {
        lengths[i] = glm::length(points[i + 1] - points[i]);
        totalLength += lengths[i];
        if (lengths[i] < kEps) return result; // нулевое звено — цепочка вырождена
    }

    const glm::vec3 origin = points[0];
    const float targetDistance = glm::length(target - origin);

    if (targetDistance > totalLength) {
        // Цель недостижима — вытягиваем цепочку по прямой в её сторону. Это и
        // есть правильный ответ: персонаж тянется, но не удлиняет кости.
        const glm::vec3 dir = glm::normalize(target - origin);
        for (size_t i = 0; i + 1 < n; ++i) points[i + 1] = points[i] + dir * lengths[i];
        result.Reached = false;
    } else {
        result.Reached = true;
        for (int iter = 0; iter < iterations; ++iter) {
            // Прямой проход: ставим конец в цель и подтягиваем к нему остальных.
            points[n - 1] = target;
            for (size_t i = n - 1; i > 0; --i) {
                const glm::vec3 dir = glm::normalize(points[i - 1] - points[i]);
                points[i - 1] = points[i] + dir * lengths[i - 1];
            }
            // Обратный проход: возвращаем корень на место (он прибит к телу) и
            // выправляем цепочку от него. Без него персонаж уезжал бы за рукой.
            points[0] = origin;
            for (size_t i = 0; i + 1 < n; ++i) {
                const glm::vec3 dir = glm::normalize(points[i + 1] - points[i]);
                points[i + 1] = points[i] + dir * lengths[i];
            }
            if (glm::length(points[n - 1] - target) < tolerance) break;
        }
    }

    // Повороты из смещения точек: каждая кость доворачивается на угол между
    // старым и новым направлением на следующий сустав. Идём от корня, накапливая
    // поворот родителя, — иначе локальные повороты считались бы от устаревшей
    // ориентации.
    result.Joints.reserve(n - 1);
    result.Rotations.reserve(n - 1);
    glm::quat parentAccum = ParentGlobalRotation(skeleton, globals, chain[0]);

    for (size_t i = 0; i + 1 < n; ++i) {
        const int joint = chain[i];
        const glm::quat oldGlobal = GlobalRotation(globals[(size_t)joint]);
        const glm::vec3 oldDir =
            glm::normalize(PositionOf(globals[(size_t)chain[i + 1]]) - PositionOf(globals[(size_t)joint]));
        const glm::vec3 newDir = glm::normalize(points[i + 1] - points[i]);

        const glm::quat newGlobal = glm::normalize(RotationBetween(oldDir, newDir) * oldGlobal);
        result.Joints.push_back(joint);
        result.Rotations.push_back(glm::normalize(glm::inverse(parentAccum) * newGlobal));
        parentAccum = newGlobal;
    }
    result.Solved = true;
    return result;
}

void ApplyIK(const IKResult& result, std::vector<JointPose>& pose, int skeletonSize,
             float weight) {
    if (!result.Solved || skeletonSize <= 0) return;
    if ((int)pose.size() < skeletonSize) pose.resize((size_t)skeletonSize);

    const float w = glm::clamp(weight, 0.0f, 1.0f);
    if (w <= 0.0f) return;

    for (size_t i = 0; i < result.Joints.size(); ++i) {
        const int joint = result.Joints[i];
        if (joint < 0 || joint >= skeletonSize) continue;
        JointPose& jp = pose[(size_t)joint];
        // Вес меньше единицы смешивает результат IK с тем, что БЫЛО в позе
        // (клип или ручная правка), — так IK включается плавно, а не рывком.
        jp.Rotation = w >= 1.0f || !jp.HasRotation
                          ? result.Rotations[i]
                          : glm::slerp(jp.Rotation, result.Rotations[i], w);
        jp.HasRotation = true;
    }
}

} // namespace sage::anim
