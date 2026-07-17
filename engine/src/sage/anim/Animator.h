#pragma once
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/anim/Skeleton.h"

namespace sage::anim {

// ---------------------------------------------------------------------------
// Animator — проигрыватель скелетной анимации. Хранит ПОКАДРОВОЕ состояние
// одного экземпляра (текущий клип, время, палитра костей); сам скелет и клипы
// разделяются (owned внешним ассетом SkinnedModel и передаются по ссылке).
//
//   animator.SetRig(skeleton, clips);
//   animator.Play("Walk");            // или по индексу
//   ... каждый кадр:
//   animator.Update(dt);
//   shader.SetMat4Array("uBones", animator.BoneMatrices().data(), animator.BoneCount());
//
// Палитра BoneMatrices()[i] = global(joint i) * inverseBind(joint i) — именно то,
// на что скиннинг-шейдер умножает позицию вершины (взвешенно по костям).
// ---------------------------------------------------------------------------
class Animator {
public:
    // Привязать скелет и набор клипов (указатели должны жить, пока жив Animator;
    // обычно это данные внутри SkinnedModel). Инициализирует палитру дефолтной позой.
    void SetRig(const Skeleton* skeleton, const std::vector<AnimationClip>* clips);

    void Play(int clipIndex, bool loop = true);
    bool Play(const std::string& clipName, bool loop = true); // false, если клипа нет
    void Stop() { m_playing = false; }
    void SetSpeed(float s) { m_speed = s; }

    // Продвигает время и пересчитывает палитру костей. Без клипа/скелета — no-op.
    void Update(float dt);

    const std::vector<glm::mat4>& BoneMatrices() const { return m_bones; }
    int BoneCount() const { return (int)m_bones.size(); }

    bool Playing() const { return m_playing; }
    int CurrentClip() const { return m_clip; }
    float Time() const { return m_time; }
    int ClipCount() const { return m_clips ? (int)m_clips->size() : 0; }
    const std::string& ClipName(int i) const;

private:
    void ComputePose(float time); // сэмплирует клип -> глобальные -> палитра

    const Skeleton* m_skeleton = nullptr;
    const std::vector<AnimationClip>* m_clips = nullptr;

    int m_clip = -1;
    float m_time = 0.0f;
    float m_speed = 1.0f;
    bool m_loop = true;
    bool m_playing = false;

    std::vector<glm::mat4> m_bones;   // палитра (global * inverseBind)
    std::vector<glm::mat4> m_globals; // scratch: глобальные матрицы костей
};

} // namespace sage::anim
