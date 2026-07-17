#include "sage/anim/Animator.h"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace sage::anim {

void Animator::SetRig(const Skeleton* skeleton, const std::vector<AnimationClip>* clips) {
    m_skeleton = skeleton;
    m_clips = clips;
    m_clip = -1;
    m_time = 0.0f;
    m_playing = false;

    int n = skeleton ? skeleton->Count() : 0;
    m_bones.assign(n, glm::mat4(1.0f));
    m_globals.assign(n, glm::mat4(1.0f));
    // Начальная поза — дефолтная (bind): палитра из TRS дефолтов.
    ComputePose(0.0f);
}

void Animator::Play(int clipIndex, bool loop) {
    if (!m_clips || clipIndex < 0 || clipIndex >= (int)m_clips->size()) {
        m_playing = false;
        m_clip = -1;
        return;
    }
    m_clip = clipIndex;
    m_time = 0.0f;
    m_loop = loop;
    m_playing = true;
}

bool Animator::Play(const std::string& clipName, bool loop) {
    if (!m_clips) return false;
    for (int i = 0; i < (int)m_clips->size(); ++i) {
        if ((*m_clips)[i].Name == clipName) { Play(i, loop); return true; }
    }
    return false;
}

const std::string& Animator::ClipName(int i) const {
    static const std::string kEmpty;
    if (!m_clips || i < 0 || i >= (int)m_clips->size()) return kEmpty;
    return (*m_clips)[i].Name;
}

void Animator::Update(float dt) {
    if (!m_skeleton) return;
    if (m_playing && m_clip >= 0 && m_clips) {
        const AnimationClip& clip = (*m_clips)[m_clip];
        m_time += dt * m_speed;
        if (clip.Duration > 0.0f) {
            if (m_loop) {
                m_time = std::fmod(m_time, clip.Duration);
                if (m_time < 0.0f) m_time += clip.Duration;
            } else if (m_time >= clip.Duration) {
                m_time = clip.Duration;
                m_playing = false;
            }
        }
    }
    ComputePose(m_time);
}

void Animator::ComputePose(float time) {
    if (!m_skeleton) return;
    const auto& joints = m_skeleton->Joints;
    int n = (int)joints.size();

    // 1. Локальные матрицы: дефолтные TRS, переопределённые активными каналами.
    std::vector<glm::vec3> t(n), s(n);
    std::vector<glm::quat> r(n);
    for (int i = 0; i < n; ++i) {
        t[i] = joints[i].Translation;
        r[i] = joints[i].Rotation;
        s[i] = joints[i].Scale;
    }
    if (m_clip >= 0 && m_clips) {
        for (const AnimChannel& ch : (*m_clips)[m_clip].Channels) {
            if (ch.Joint < 0 || ch.Joint >= n) continue;
            glm::vec3 v; glm::quat q;
            ch.Sample(time, v, q);
            switch (ch.Target) {
                case AnimPath::Translation: t[ch.Joint] = v; break;
                case AnimPath::Rotation:    r[ch.Joint] = q; break;
                case AnimPath::Scale:       s[ch.Joint] = v; break;
            }
        }
    }

    // 2. Локальные матрицы из TRS (по одной на кость).
    std::vector<glm::mat4> local(n);
    for (int i = 0; i < n; ++i) {
        local[i] = glm::translate(glm::mat4(1.0f), t[i]) * glm::mat4_cast(r[i]) *
                   glm::scale(glm::mat4(1.0f), s[i]);
    }

    // 3. Глобальные матрицы: global = parentGlobal * local. Родитель может идти
    //    в списке ПОЗЖЕ ребёнка, поэтому поднимаемся по цепочке родителей
    //    (надёжно при любом порядке костей; глубины скелетов невелики).
    for (int i = 0; i < n; ++i) {
        glm::mat4 global = local[i];
        int p = joints[i].Parent;
        while (p >= 0) {
            global = local[p] * global;
            p = joints[p].Parent;
        }
        m_globals[i] = global;
    }

    // 4. Палитра костей: global * inverseBind.
    for (int i = 0; i < n; ++i) {
        m_bones[i] = m_globals[i] * joints[i].InverseBind;
    }
}

} // namespace sage::anim
