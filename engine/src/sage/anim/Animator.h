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

    // Плавный переход к клипу за fadeDuration секунд: старый клип затухает, новый
    // проявляется, позы смешиваются покостно (lerp T/S, slerp R). Оба клипа
    // тикают во время перехода. Если сейчас ничего не играет (или fadeDuration<=0)
    // — эквивалент Play. Это основа «нет резких переключений» и одновременного
    // проигрывания двух клипов.
    void CrossFade(int clipIndex, float fadeDuration, bool loop = true);
    bool CrossFade(const std::string& clipName, float fadeDuration, bool loop = true);

    void Stop() { m_playing = false; }
    void SetSpeed(float s) { m_speed = s; }

    // Перематывает ТЕКУЩИЙ клип на абсолютное время (сек) и сразу пересчитывает
    // позу — без продвижения времени и без ожидания следующего Update. Нужен
    // всему, что показывает позу в ПРОИЗВОЛЬНЫЙ момент, а не «через dt от
    // прошлого кадра»: перетаскивание головки таймлайна в инструменте анимации,
    // покадровый рендер секвенции, превью клипа. Отменяет активный кросс-фейд
    // (смешивать нечего — время задано явно) и не меняет флаг проигрывания:
    // перемотка на паузе оставляет паузу. Время зажимается в [0, Duration] у
    // незацикленного клипа и заворачивается по модулю у зацикленного, поэтому
    // Seek за границу даёт то же, что и доигранный/зациклившийся клип.
    void Seek(float time);

    // Продвигает время и пересчитывает палитру костей. Без клипа/скелета — no-op.
    void Update(float dt);

    // Переопределение позы поверх клипа — то, чем инструмент анимации правит
    // позу руками. Указатель НЕ копируется и не владеется: вектор живёт снаружи
    // (в компоненте сцены), и его достаточно переустанавливать каждый кадр,
    // чтобы пережить перевыделение памяти в хранилище компонентов. nullptr или
    // пустой вектор — переопределений нет, поза целиком от клипа.
    //
    // Переопределения применяются ПОСЛЕ смешивания клипов, в локальном TRS, а
    // значит корректно работают и во время кросс-фейда, и вообще без клипа
    // (тогда база — дефолтная поза скелета).
    void SetPoseOverride(const std::vector<JointPose>* overrides) { m_overrides = overrides; }

    // --- Корневое движение (root motion) -----------------------------------
    //
    // Библиотеки анимаций поставляются в двух вариантах: с запечённым в клип
    // перемещением персонажа и без него. С запечённым и БЕЗ извлечения меш
    // уезжает от своей сущности: визуально персонаж идёт, а его коллайдер,
    // камера и точка звука остаются на месте — потом он телепортируется назад
    // на стыке цикла. Извлечение снимает перемещение с ПОЗЫ и отдаёт его
    // наружу, чтобы игра подвинула саму сущность (и физику вместе с ней).
    void SetRootMotion(bool enabled) { m_rootMotion = enabled; }
    bool RootMotion() const { return m_rootMotion; }

    // Смещение корня за последний Update, В ЛОКАЛЬНЫХ координатах модели.
    // Забирается один раз: повторный вызов вернёт ноль, иначе одно и то же
    // перемещение применилось бы дважды на кадр с двумя читателями.
    glm::vec3 ConsumeRootDelta() {
        const glm::vec3 d = m_rootDelta;
        m_rootDelta = glm::vec3(0.0f);
        return d;
    }
    const std::vector<JointPose>* PoseOverride() const { return m_overrides; }

    // Пересчитывает позу, не трогая время. Нужно после правки переопределений в
    // тот же кадр: без этого изменение кости было бы видно только со следующего
    // Update, то есть гизмо «отставало» бы на кадр.
    void RefreshPose() { ComputePoseBlended(FadeWeight()); }

    const std::vector<glm::mat4>& BoneMatrices() const { return m_bones; }
    int BoneCount() const { return (int)m_bones.size(); }

    // Глобальные матрицы костей в пространстве модели — БЕЗ inverseBind, в
    // отличие от палитры. Именно они нужны, чтобы нарисовать скелет и поставить
    // гизмо на кость: палитра к этому непригодна, она переводит вершины из
    // bind-позы, а не описывает положение сустава.
    const std::vector<glm::mat4>& GlobalMatrices() const { return m_globals; }

    bool Playing() const { return m_playing; }
    int CurrentClip() const { return m_clip; }
    float Time() const { return m_time; }
    int ClipCount() const { return m_clips ? (int)m_clips->size() : 0; }
    const std::string& ClipName(int i) const;

    // Идёт ли сейчас кросс-фейд, и его вес (0 -> целиком старый клип, 1 -> новый).
    bool Fading() const { return m_fadeFromClip >= 0; }
    float FadeWeight() const;

private:
    // Локальные TRS костей для клипа в момент time (дефолты, переопределённые
    // каналами). Общий сэмплер — используется и одиночным проигрыванием, и
    // обоими клипами кросс-фейда.
    void SamplePose(int clipIndex, float time,
                    std::vector<glm::vec3>& t, std::vector<glm::quat>& r, std::vector<glm::vec3>& s) const;
    // Считает палитру из TRS двух клипов, смешанных по weight (globals -> bones).
    void ComputePoseBlended(float weight);

    const Skeleton* m_skeleton = nullptr;
    const std::vector<AnimationClip>* m_clips = nullptr;

    int m_clip = -1;
    float m_time = 0.0f;
    float m_speed = 1.0f;
    bool m_loop = true;
    bool m_playing = false;

    // Кросс-фейд: затухающий («откуда») клип + прогресс перехода.
    int m_fadeFromClip = -1;
    float m_fadeFromTime = 0.0f;
    bool m_fadeFromLoop = true;
    float m_fadeElapsed = 0.0f;
    float m_fadeDuration = 0.0f;

    // Переопределения позы (не владеем — см. SetPoseOverride).
    const std::vector<JointPose>* m_overrides = nullptr;
    bool m_rootMotion = false;
    bool m_hasPrevRoot = false;
    glm::vec3 m_prevRoot{0.0f};  // локальный перенос корня в прошлом кадре
    glm::vec3 m_rootDelta{0.0f}; // накоплено с последнего ConsumeRootDelta

    std::vector<glm::mat4> m_bones;   // палитра (global * inverseBind)
    std::vector<glm::mat4> m_globals; // scratch: глобальные матрицы костей
};

} // namespace sage::anim
