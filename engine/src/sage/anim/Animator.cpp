#include "sage/anim/Animator.h"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp> // glm::slerp для смешивания поворотов костей

namespace sage::anim {

void Animator::SetRig(const Skeleton* skeleton, const std::vector<AnimationClip>* clips) {
    m_skeleton = skeleton;
    m_clips = clips;
    m_clip = -1;
    m_time = 0.0f;
    m_playing = false;
    m_fadeFromClip = -1;
    m_fadeDuration = 0.0f;

    int n = skeleton ? skeleton->Count() : 0;
    m_bones.assign(n, glm::mat4(1.0f));
    m_globals.assign(n, glm::mat4(1.0f));
    // Начальная поза — дефолтная (bind): палитра из TRS дефолтов.
    ComputePoseBlended(1.0f);
}

void Animator::Play(int clipIndex, bool loop) {
    m_fadeFromClip = -1; // мгновенное переключение отменяет активный кросс-фейд
    m_fadeDuration = 0.0f;
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

void Animator::CrossFade(int clipIndex, float fadeDuration, bool loop) {
    if (!m_clips || clipIndex < 0 || clipIndex >= (int)m_clips->size()) return;
    // Нечего смешивать (ничего не играет) или нулевой переход — обычный Play.
    if (m_clip < 0 || fadeDuration <= 0.0f) { Play(clipIndex, loop); return; }
    if (clipIndex == m_clip && m_fadeFromClip < 0) return; // уже он и без перехода

    m_fadeFromClip = m_clip;    // текущий становится затухающим
    m_fadeFromTime = m_time;
    m_fadeFromLoop = m_loop;
    m_fadeElapsed = 0.0f;
    m_fadeDuration = fadeDuration;

    m_clip = clipIndex;         // новый — проявляющийся
    m_time = 0.0f;
    m_loop = loop;
    m_playing = true;
}

bool Animator::CrossFade(const std::string& clipName, float fadeDuration, bool loop) {
    if (!m_clips) return false;
    for (int i = 0; i < (int)m_clips->size(); ++i) {
        if ((*m_clips)[i].Name == clipName) { CrossFade(i, fadeDuration, loop); return true; }
    }
    return false;
}

float Animator::FadeWeight() const {
    if (m_fadeFromClip < 0 || m_fadeDuration <= 0.0f) return 1.0f;
    float w = m_fadeElapsed / m_fadeDuration;
    return w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
}

const std::string& Animator::ClipName(int i) const {
    static const std::string kEmpty;
    if (!m_clips || i < 0 || i >= (int)m_clips->size()) return kEmpty;
    return (*m_clips)[i].Name;
}

namespace {
// Продвигает время клипа с учётом лупа/окончания. Возвращает false, если
// НЕзацикленный клип доиграл (вызывающий останавливает).
bool AdvanceClipTime(const AnimationClip& clip, float& time, float dt, bool loop) {
    time += dt;
    if (clip.Duration <= 0.0f) return true;
    if (loop) {
        time = std::fmod(time, clip.Duration);
        if (time < 0.0f) time += clip.Duration;
        return true;
    }
    if (time >= clip.Duration) { time = clip.Duration; return false; }
    return true;
}
} // namespace

void Animator::Update(float dt) {
    if (!m_skeleton) return;
    float step = dt * m_speed;

    if (m_playing && m_clip >= 0 && m_clips) {
        if (!AdvanceClipTime((*m_clips)[m_clip], m_time, step, m_loop)) m_playing = false;
    }
    // Затухающий клип кросс-фейда тоже тикает (иначе «замер» кадр смешивался бы).
    if (m_fadeFromClip >= 0 && m_clips) {
        AdvanceClipTime((*m_clips)[m_fadeFromClip], m_fadeFromTime, step, m_fadeFromLoop);
        m_fadeElapsed += (step < 0.0f ? -step : step); // прогресс перехода не зависит от знака скорости
        if (m_fadeDuration <= 0.0f || m_fadeElapsed >= m_fadeDuration) {
            m_fadeFromClip = -1; // переход завершён — остаётся только новый клип
            m_fadeDuration = 0.0f;
        }
    }

    ComputePoseBlended(FadeWeight());
}

void Animator::SamplePose(int clipIndex, float time,
                          std::vector<glm::vec3>& t, std::vector<glm::quat>& r,
                          std::vector<glm::vec3>& s) const {
    const auto& joints = m_skeleton->Joints;
    int n = (int)joints.size();
    for (int i = 0; i < n; ++i) {
        t[i] = joints[i].Translation;
        r[i] = joints[i].Rotation;
        s[i] = joints[i].Scale;
    }
    if (clipIndex < 0 || !m_clips || clipIndex >= (int)m_clips->size()) return;
    for (const AnimChannel& ch : (*m_clips)[clipIndex].Channels) {
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

void Animator::ComputePoseBlended(float weight) {
    if (!m_skeleton) return;
    const auto& joints = m_skeleton->Joints;
    int n = (int)joints.size();

    // 1. Локальные TRS активного клипа; при кросс-фейде — смешать с затухающим
    //    (weight: 0 -> целиком старый, 1 -> целиком новый). Позы смешиваются В
    //    ПРОСТРАНСТВЕ TRS (lerp T/S, slerp R), а не матриц — так суставы идут по
    //    кратчайшей дуге без «схлопывания» масштаба.
    std::vector<glm::vec3> t(n), s(n);
    std::vector<glm::quat> r(n);
    SamplePose(m_clip, m_time, t, r, s);

    if (m_fadeFromClip >= 0 && weight < 1.0f) {
        std::vector<glm::vec3> t0(n), s0(n);
        std::vector<glm::quat> r0(n);
        SamplePose(m_fadeFromClip, m_fadeFromTime, t0, r0, s0);
        for (int i = 0; i < n; ++i) {
            t[i] = glm::mix(t0[i], t[i], weight);
            s[i] = glm::mix(s0[i], s[i], weight);
            r[i] = glm::slerp(r0[i], r[i], weight);
        }
    }

    // 2. Локальные матрицы из TRS.
    std::vector<glm::mat4> local(n);
    for (int i = 0; i < n; ++i) {
        local[i] = glm::translate(glm::mat4(1.0f), t[i]) * glm::mat4_cast(r[i]) *
                   glm::scale(glm::mat4(1.0f), s[i]);
    }

    // 3. Глобальные матрицы: поднимаемся по цепочке родителей (любой порядок костей).
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
