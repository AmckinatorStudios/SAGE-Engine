#include "sage/anim/Skeleton.h"

#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>

namespace sage::anim {

glm::mat4 Joint::LocalMatrix() const {
    glm::mat4 t = glm::translate(glm::mat4(1.0f), Translation);
    glm::mat4 r = glm::mat4_cast(Rotation);
    glm::mat4 s = glm::scale(glm::mat4(1.0f), Scale);
    return t * r * s;
}

void AnimChannel::Sample(float t, glm::vec3& outVec3, glm::quat& outQuat) const {
    if (Times.empty()) return;

    // Кламп за границами клипа — до первого/после последнего ключа держим края.
    if (t <= Times.front() || Times.size() == 1) {
        if (Target == AnimPath::Rotation) outQuat = glm::normalize(glm::quat(Values.front().w, Values.front().x, Values.front().y, Values.front().z));
        else outVec3 = glm::vec3(Values.front());
        return;
    }
    if (t >= Times.back()) {
        if (Target == AnimPath::Rotation) outQuat = glm::normalize(glm::quat(Values.back().w, Values.back().x, Values.back().y, Values.back().z));
        else outVec3 = glm::vec3(Values.back());
        return;
    }

    // Ищем интервал [k, k+1], в который попадает t.
    size_t k = 0;
    while (k + 1 < Times.size() && Times[k + 1] < t) ++k;
    float t0 = Times[k], t1 = Times[k + 1];
    float a = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
    if (Interp == AnimInterp::Step) a = 0.0f; // держим левый ключ до следующего

    // КУБИЧЕСКАЯ КРИВАЯ — по формуле Эрмита из спецификации glTF: значение и
    // касательные на концах интервала, умноженные на его длину. Считается в тех
    // же компонентах, что и хранится; поворот после этого нормализуется —
    // кубическая кривая по кватернионам единичную длину не сохраняет.
    if (Interp == AnimInterp::CubicSpline && k + 1 < Values.size() &&
        InTangents.size() == Values.size() && OutTangents.size() == Values.size()) {
        const float dt = t1 - t0;
        const float a2 = a * a, a3 = a2 * a;
        const float h00 = 2.0f * a3 - 3.0f * a2 + 1.0f;
        const float h10 = a3 - 2.0f * a2 + a;
        const float h01 = -2.0f * a3 + 3.0f * a2;
        const float h11 = a3 - a2;
        const glm::vec4 value = h00 * Values[k] + h10 * dt * OutTangents[k] +
                                h01 * Values[k + 1] + h11 * dt * InTangents[k + 1];
        if (Target == AnimPath::Rotation)
            outQuat = glm::normalize(glm::quat(value.w, value.x, value.y, value.z));
        else
            outVec3 = glm::vec3(value);
        return;
    }

    if (Target == AnimPath::Rotation) {
        glm::quat q0(Values[k].w, Values[k].x, Values[k].y, Values[k].z);
        glm::quat q1(Values[k + 1].w, Values[k + 1].x, Values[k + 1].y, Values[k + 1].z);
        // glm::slerp сам разворачивает второй кватернион, если угол между ними
        // тупой: без этого поворот на 190° шёл бы длинным путём — через 170° в
        // обратную сторону. Проверено отдельно (test_animation).
        outQuat = glm::normalize(glm::slerp(glm::normalize(q0), glm::normalize(q1), a));
    } else {
        outVec3 = glm::mix(glm::vec3(Values[k]), glm::vec3(Values[k + 1]), a);
    }
}

} // namespace sage::anim
