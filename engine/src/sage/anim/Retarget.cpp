#include "sage/anim/Retarget.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

namespace sage::anim {

namespace {

// Высота скелета в бинд-позе — расстояние от корня до самой дальней кости.
// Используется как масштаб перемещения корня: точной «единицы роста» у скелета
// нет, а размах цепочки костей ей пропорционален.
float SkeletonReach(const Skeleton& skeleton) {
    const int n = skeleton.Count();
    if (n == 0) return 0.0f;

    // Глобальные позиции бинд-позы: поднимаемся по цепочке родителей.
    std::vector<glm::mat4> globals((size_t)n, glm::mat4(1.0f));
    for (int i = 0; i < n; ++i) {
        glm::mat4 g = skeleton.Joints[(size_t)i].LocalMatrix();
        int p = skeleton.Joints[(size_t)i].Parent;
        while (p >= 0) {
            g = skeleton.Joints[(size_t)p].LocalMatrix() * g;
            p = skeleton.Joints[(size_t)p].Parent;
        }
        globals[(size_t)i] = g;
    }

    const glm::vec3 root(globals[0][3]);
    float reach = 0.0f;
    for (int i = 0; i < n; ++i) {
        reach = std::max(reach, glm::length(glm::vec3(globals[(size_t)i][3]) - root));
    }
    return reach;
}

} // namespace

std::string NormalizeBoneName(const std::string& name) {
    // Префикс экспортёра: всё до последнего ':' или '|' ("mixamorig:LeftArm",
    // "Armature|Hips"). Он говорит про пайплайн, а не про кость.
    size_t start = 0;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == ':' || name[i] == '|') start = i + 1;
    }

    std::string out;
    out.reserve(name.size() - start);
    for (size_t i = start; i < name.size(); ++i) {
        const unsigned char c = (unsigned char)name[i];
        if (c == '_' || c == '-' || c == '.' || c == ' ') continue; // разделители не значимы
        out.push_back((char)std::tolower(c));
    }
    return out;
}

BoneMap MapByName(const Skeleton& source, const Skeleton& target) {
    BoneMap map;
    map.SourceToTarget.assign((size_t)source.Count(), -1);

    // Индекс целевых костей по нормализованному имени. Дубликаты (две кости с
    // именами "Hand_L" и "hand.l") оставляют ПЕРВУЮ: выбрать вторую наугад
    // означало бы менять результат от порядка костей в файле.
    std::unordered_map<std::string, int> byName;
    for (int i = 0; i < target.Count(); ++i) {
        const std::string key = NormalizeBoneName(target.Joints[(size_t)i].Name);
        if (key.empty()) continue;
        byName.emplace(key, i);
    }

    for (int i = 0; i < source.Count(); ++i) {
        const std::string key = NormalizeBoneName(source.Joints[(size_t)i].Name);
        if (key.empty()) continue;
        auto it = byName.find(key);
        if (it != byName.end()) map.SourceToTarget[(size_t)i] = it->second;
    }
    return map;
}

AnimationClip Retarget(const AnimationClip& clip, const Skeleton& source, const Skeleton& target,
                       const BoneMap& map) {
    AnimationClip out;
    out.Name = clip.Name;
    out.Duration = clip.Duration;

    // Масштаб перемещения корня: во сколько раз целевой скелет крупнее.
    const float sourceReach = SkeletonReach(source);
    const float targetReach = SkeletonReach(target);
    const float rootScale =
        (sourceReach > 1e-4f && targetReach > 1e-4f) ? targetReach / sourceReach : 1.0f;

    for (const AnimChannel& ch : clip.Channels) {
        if (ch.Joint < 0 || ch.Joint >= (int)map.SourceToTarget.size()) continue;
        const int targetJoint = map.SourceToTarget[(size_t)ch.Joint];
        if (targetJoint < 0) continue; // кости нет у цели — переносить некуда

        const Joint& srcJoint = source.Joints[(size_t)ch.Joint];
        const Joint& dstJoint = target.Joints[(size_t)targetJoint];
        const bool isRoot = dstJoint.Parent < 0;

        // Длины костей принадлежат целевому скелету — переносим только корень
        // (это перемещение персонажа), и то с поправкой на размер.
        if (ch.Target == AnimPath::Translation && !isRoot) continue;
        // Масштаб костей — свойство исходной модели, а не движения.
        if (ch.Target == AnimPath::Scale) continue;

        AnimChannel result;
        result.Joint = targetJoint;
        result.Target = ch.Target;
        result.Interp = ch.Interp;
        result.Times = ch.Times;
        result.Values.reserve(ch.Values.size());

        if (ch.Target == AnimPath::Rotation) {
            // Отклонение от бинд-позы источника, приложенное к бинд-позе цели.
            const glm::quat srcBindInv = glm::inverse(srcJoint.Rotation);
            for (const glm::vec4& v : ch.Values) {
                const glm::quat srcRot(v.w, v.x, v.y, v.z);
                const glm::quat delta = srcBindInv * srcRot;
                const glm::quat dstRot = glm::normalize(dstJoint.Rotation * delta);
                result.Values.emplace_back(dstRot.x, dstRot.y, dstRot.z, dstRot.w);
            }
        } else {
            // Корневое перемещение: смещение относительно бинд-позы источника,
            // масштабированное под цель и добавленное к её бинд-позе. Прямая
            // подстановка позиции увела бы персонажа туда, где стоял оригинал.
            for (const glm::vec4& v : ch.Values) {
                const glm::vec3 offset = glm::vec3(v) - srcJoint.Translation;
                const glm::vec3 moved = dstJoint.Translation + offset * rootScale;
                result.Values.emplace_back(moved, 0.0f);
            }
        }

        out.Channels.push_back(std::move(result));
    }
    return out;
}

AnimationClip Retarget(const AnimationClip& clip, const Skeleton& source, const Skeleton& target) {
    return Retarget(clip, source, target, MapByName(source, target));
}

} // namespace sage::anim
