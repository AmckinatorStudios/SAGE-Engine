#pragma once
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/anim/Skeleton.h"

namespace sage::anim {

// Клип, с которого персонаж начинает в сцене, — ПОЗА ПОКОЯ, и из поз покоя —
// самая простая. «Idle» пишут почти все (в любом регистре, с префиксом
// экспортёра), но у библиотек анимаций их десяток: «Crouch_Idle_Loop»,
// «Pistol_Idle_Loop», «Idle_Talking_Loop». Персонаж, поставленный в сцену на
// корточках, выглядит сломанным. Берётся «idle» с наименьшим числом прочих
// слов (служебные «loop», «anim» и префикс до '|' не считаются). Нет ни
// одного — первый клип; клипов нет — -1.
int PreferredIdleClip(const std::vector<AnimationClip>& clips);

// Локальная поза скелета: TRS каждой кости относительно родителя. Это общий
// язык всех, кто СТРОИТ позу (клип, кросс-фейд, граф анимации), и того, кто
// превращает её в палитру костей (Animator). Смешивание идёт в TRS, а не в
// матрицах: суставы движутся по кратчайшей дуге, масштаб не «схлопывается».
struct LocalPose {
    std::vector<glm::vec3> T;
    std::vector<glm::quat> R;
    std::vector<glm::vec3> S;
    int Size() const { return (int)T.size(); }
    void Resize(int n) { T.resize(n); R.resize(n); S.resize(n); }
};

// Поза клипа в момент time: поза покоя скелета, поверх — каналы клипа.
// clip == nullptr — поза покоя. Общий сэмплер для Animator и графа анимации:
// два сэмплера одного формата однажды разошлись бы в интерполяции.
void SampleClipPose(const Skeleton& skeleton, const AnimationClip* clip, float time, LocalPose& out);

// out = a·(1-w) + b·w покостно (lerp T/S, slerp R). out может совпадать с a или b.
// Размеры a и b обязаны совпадать (одна поза одного скелета).
void BlendPoses(const LocalPose& a, const LocalPose& b, float w, LocalPose& out);


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

    // ВНЕШНИЙ ИСТОЧНИК ПОЗЫ вместо собственных клипов — крючок для графа
    // анимации (sage/anim/AnimGraph.h). Поза строится снаружи, а Animator
    // делает с ней то же, что с позой клипа: корневое движение, ручные
    // переопределения и IK сверху, палитра костей для скиннинга. Так у графа
    // нет своего второго «проигрывателя», и всё, что умеет Animator, работает
    // поверх графа без единой строчки в графе.
    //
    // Указатель не копируется и не владеется (как у SetPoseOverride). Поза не
    // того размера (скелет сменился) игнорируется. nullptr — снова свои клипы.
    void SetExternalPose(const LocalPose* pose) { m_external = pose; }
    const LocalPose* ExternalPose() const { return m_external; }

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
    void SamplePose(int clipIndex, float time, LocalPose& out) const;
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
    const LocalPose* m_external = nullptr;  // не владеем — см. SetExternalPose
    bool m_rootMotion = false;
    bool m_hasPrevRoot = false;
    glm::vec3 m_prevRoot{0.0f};  // локальный перенос корня в прошлом кадре
    glm::vec3 m_rootDelta{0.0f}; // накоплено с последнего ConsumeRootDelta

    std::vector<glm::mat4> m_bones;   // палитра (global * inverseBind)
    std::vector<glm::mat4> m_globals; // scratch: глобальные матрицы костей
};

} // namespace sage::anim
