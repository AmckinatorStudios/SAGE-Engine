// Модульные тесты скелетной анимации: сэмплирование каналов (интерполяция/кламп/
// step/slerp), локальная матрица кости и проигрыватель Animator (дефолтная поза,
// зацикливание, разовое проигрывание, иерархия костей). Чистая математика — без GL.
#include "TestFramework.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "sage/anim/Skeleton.h"
#include "sage/anim/Animator.h"

using namespace sage::anim;

// --- AnimChannel::Sample ---------------------------------------------------

TEST(Sample_translation_linear_midpoint) {
    AnimChannel ch;
    ch.Target = AnimPath::Translation;
    ch.Interp = AnimInterp::Linear;
    ch.Times = {0.0f, 1.0f};
    ch.Values = {glm::vec4(0, 0, 0, 0), glm::vec4(10, 0, 0, 0)};
    glm::vec3 v(0.0f); glm::quat q;
    ch.Sample(0.5f, v, q);
    CHECK_NEAR(v.x, 5.0f, 1e-5); // линейно на середине
}

TEST(Sample_clamps_before_and_after) {
    AnimChannel ch;
    ch.Target = AnimPath::Translation;
    ch.Times = {0.0f, 1.0f};
    ch.Values = {glm::vec4(1, 0, 0, 0), glm::vec4(9, 0, 0, 0)};
    glm::vec3 v(0.0f); glm::quat q;
    ch.Sample(-5.0f, v, q); // до первого ключа — держим левый край
    CHECK_NEAR(v.x, 1.0f, 1e-5);
    ch.Sample(100.0f, v, q); // после последнего — держим правый край
    CHECK_NEAR(v.x, 9.0f, 1e-5);
}

TEST(Sample_step_holds_left_key) {
    AnimChannel ch;
    ch.Target = AnimPath::Translation;
    ch.Interp = AnimInterp::Step;
    ch.Times = {0.0f, 1.0f};
    ch.Values = {glm::vec4(0, 0, 0, 0), glm::vec4(10, 0, 0, 0)};
    glm::vec3 v(0.0f); glm::quat q;
    ch.Sample(0.75f, v, q); // Step — без интерполяции, левый ключ до следующего
    CHECK_NEAR(v.x, 0.0f, 1e-5);
}

TEST(Sample_rotation_endpoints) {
    AnimChannel ch;
    ch.Target = AnimPath::Rotation;
    glm::quat q0 = glm::angleAxis(glm::radians(0.0f), glm::vec3(0, 1, 0));
    glm::quat q1 = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
    ch.Times = {0.0f, 1.0f};
    ch.Values = {glm::vec4(q0.x, q0.y, q0.z, q0.w), glm::vec4(q1.x, q1.y, q1.z, q1.w)};
    glm::vec3 v; glm::quat q;

    ch.Sample(0.0f, v, q); // в начале — тождественный поворот
    glm::vec3 r0 = q * glm::vec3(1, 0, 0);
    CHECK_NEAR(r0.x, 1.0f, 1e-4);
    CHECK_NEAR(r0.z, 0.0f, 1e-4);

    ch.Sample(1.0f, v, q); // в конце — поворот Y на 90°: (1,0,0) -> (0,0,-1)
    glm::vec3 r1 = q * glm::vec3(1, 0, 0);
    CHECK_NEAR(r1.x, 0.0f, 1e-4);
    CHECK_NEAR(r1.z, -1.0f, 1e-4);
}

// --- Joint::LocalMatrix ----------------------------------------------------

TEST(Joint_local_matrix_translation) {
    Joint j;
    j.Translation = {1.0f, 2.0f, 3.0f};
    glm::vec4 p = j.LocalMatrix() * glm::vec4(0, 0, 0, 1);
    CHECK_NEAR(p.x, 1.0f, 1e-5);
    CHECK_NEAR(p.y, 2.0f, 1e-5);
    CHECK_NEAR(p.z, 3.0f, 1e-5);
}

// --- Animator --------------------------------------------------------------

// Двухкостный скелет: корень в начале координат, ребёнок на (0,1,0) над ним.
static Skeleton MakeTwoBoneRig() {
    Skeleton sk;
    sk.Joints.resize(2);
    sk.Joints[0].Parent = -1;
    sk.Joints[0].Translation = {0, 0, 0};
    sk.Joints[0].InverseBind = glm::mat4(1.0f);
    sk.Joints[1].Parent = 0;
    sk.Joints[1].Translation = {0, 1, 0};
    sk.Joints[1].InverseBind = glm::mat4(1.0f);
    return sk;
}

TEST(Animator_default_pose_is_bind) {
    Skeleton sk = MakeTwoBoneRig();
    std::vector<AnimationClip> clips; // без клипов — дефолтная поза
    Animator a;
    a.SetRig(&sk, &clips);
    CHECK_EQ(a.BoneCount(), 2);
    // Палитра[0] = глобальная корня = единичная (inverseBind=I).
    glm::vec4 p0 = a.BoneMatrices()[0] * glm::vec4(0, 0, 0, 1);
    CHECK_NEAR(p0.x, 0.0f, 1e-5);
    CHECK_NEAR(p0.y, 0.0f, 1e-5);
    // Палитра[1] = глобальная ребёнка = перенос (0,1,0).
    glm::vec4 p1 = a.BoneMatrices()[1] * glm::vec4(0, 0, 0, 1);
    CHECK_NEAR(p1.y, 1.0f, 1e-5);
}

TEST(Animator_rotating_root_moves_child_globally) {
    Skeleton sk = MakeTwoBoneRig();

    // Клип: корень (кость 0) повёрнут на 90° вокруг Z.
    glm::quat rot = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
    AnimationClip clip;
    clip.Name = "Bend";
    clip.Duration = 1.0f;
    AnimChannel ch;
    ch.Joint = 0;
    ch.Target = AnimPath::Rotation;
    ch.Times = {0.0f};
    ch.Values = {glm::vec4(rot.x, rot.y, rot.z, rot.w)};
    clip.Channels.push_back(ch);
    std::vector<AnimationClip> clips{clip};

    Animator a;
    a.SetRig(&sk, &clips);
    CHECK_TRUE(a.Play("Bend"));
    a.Update(0.0f); // пересчитать позу в t=0

    // Глобаль ребёнка = rot(корень) * translate(0,1,0). Точка (0,0,0) кости 1:
    // rot_z(90°) применённый к (0,1,0) даёт (-1,0,0).
    glm::vec4 p1 = a.BoneMatrices()[1] * glm::vec4(0, 0, 0, 1);
    CHECK_NEAR(p1.x, -1.0f, 1e-4);
    CHECK_NEAR(p1.y, 0.0f, 1e-4);
}

TEST(Animator_play_missing_clip_returns_false) {
    Skeleton sk = MakeTwoBoneRig();
    std::vector<AnimationClip> clips;
    Animator a;
    a.SetRig(&sk, &clips);
    CHECK_FALSE(a.Play("DoesNotExist"));
}

TEST(Animator_loop_wraps_time) {
    Skeleton sk = MakeTwoBoneRig();
    AnimationClip clip;
    clip.Name = "Loop";
    clip.Duration = 2.0f;
    std::vector<AnimationClip> clips{clip};
    Animator a;
    a.SetRig(&sk, &clips);
    a.Play("Loop", /*loop=*/true);
    a.Update(2.5f); // fmod(2.5, 2.0) = 0.5
    CHECK_NEAR(a.Time(), 0.5f, 1e-4);
    CHECK_TRUE(a.Playing());
}

TEST(Animator_non_loop_clamps_and_stops) {
    Skeleton sk = MakeTwoBoneRig();
    AnimationClip clip;
    clip.Name = "Once";
    clip.Duration = 2.0f;
    std::vector<AnimationClip> clips{clip};
    Animator a;
    a.SetRig(&sk, &clips);
    a.Play("Once", /*loop=*/false);
    a.Update(5.0f); // за концом — кламп до Duration и остановка
    CHECK_NEAR(a.Time(), 2.0f, 1e-4);
    CHECK_FALSE(a.Playing());
}

// --- Seek: постановка позы на произвольное время (перемотка) ----------------
// Нужна всему, что показывает кадр НЕ «через dt от предыдущего»: таймлайн
// инструмента анимации, покадровый рендер секвенции, превью клипа.

TEST(Animator_seek_sets_absolute_time) {
    Skeleton sk = MakeTwoBoneRig();
    AnimationClip clip; clip.Name = "Walk"; clip.Duration = 2.0f;
    std::vector<AnimationClip> clips{clip};
    Animator a;
    a.SetRig(&sk, &clips);
    a.Play("Walk", /*loop=*/false);
    a.Update(0.5f);
    a.Seek(1.25f);
    CHECK_NEAR(a.Time(), 1.25f, 1e-4); // время задано явно, а не сдвинуто на dt
}

TEST(Animator_seek_wraps_when_looping) {
    Skeleton sk = MakeTwoBoneRig();
    AnimationClip clip; clip.Name = "Loop"; clip.Duration = 2.0f;
    std::vector<AnimationClip> clips{clip};
    Animator a;
    a.SetRig(&sk, &clips);
    a.Play("Loop", /*loop=*/true);
    a.Seek(5.0f);                       // fmod(5, 2) = 1
    CHECK_NEAR(a.Time(), 1.0f, 1e-4);
    a.Seek(-0.5f);                      // отрицательное заворачивается вперёд
    CHECK_NEAR(a.Time(), 1.5f, 1e-4);
}

TEST(Animator_seek_clamps_when_not_looping) {
    Skeleton sk = MakeTwoBoneRig();
    AnimationClip clip; clip.Name = "Once"; clip.Duration = 2.0f;
    std::vector<AnimationClip> clips{clip};
    Animator a;
    a.SetRig(&sk, &clips);
    a.Play("Once", /*loop=*/false);
    a.Seek(99.0f);
    CHECK_NEAR(a.Time(), 2.0f, 1e-4);
    a.Seek(-99.0f);
    CHECK_NEAR(a.Time(), 0.0f, 1e-4);
}

TEST(Animator_seek_keeps_paused_state) {
    Skeleton sk = MakeTwoBoneRig();
    AnimationClip clip; clip.Name = "Walk"; clip.Duration = 2.0f;
    std::vector<AnimationClip> clips{clip};
    Animator a;
    a.SetRig(&sk, &clips);
    a.Play("Walk");
    a.Stop();               // перемотка на паузе не должна снимать паузу
    a.Seek(1.0f);
    CHECK_FALSE(a.Playing());
    CHECK_NEAR(a.Time(), 1.0f, 1e-4);
}

// --- Кросс-фейд между клипами (одновременное проигрывание + смешивание поз) ---
static AnimationClip MakeRootRotClip(const char* name, float deg) {
    glm::quat rot = glm::angleAxis(glm::radians(deg), glm::vec3(0, 0, 1));
    AnimationClip clip; clip.Name = name; clip.Duration = 1.0f;
    AnimChannel ch; ch.Joint = 0; ch.Target = AnimPath::Rotation; ch.Times = {0.0f};
    ch.Values = {glm::vec4(rot.x, rot.y, rot.z, rot.w)};
    clip.Channels.push_back(ch);
    return clip;
}

TEST(Animator_crossfade_blends_two_clips) {
    Skeleton sk = MakeTwoBoneRig();
    std::vector<AnimationClip> clips{MakeRootRotClip("Idle", 0.0f), MakeRootRotClip("Bend", 90.0f)};
    Animator a; a.SetRig(&sk, &clips);
    CHECK_TRUE(a.Play("Idle"));
    a.Update(0.0f);
    glm::vec4 p = a.BoneMatrices()[1] * glm::vec4(0, 0, 0, 1);
    CHECK_NEAR(p.x, 0.0f, 1e-3); CHECK_NEAR(p.y, 1.0f, 1e-3); // 0° -> ребёнок (0,1,0)

    CHECK_TRUE(a.CrossFade("Bend", 1.0f));
    a.Update(0.5f); // середина перехода -> slerp 0°..90° = 45°
    CHECK_TRUE(a.Fading());
    CHECK_NEAR(a.FadeWeight(), 0.5f, 1e-3);
    p = a.BoneMatrices()[1] * glm::vec4(0, 0, 0, 1);
    CHECK_NEAR(p.x, -0.7071f, 2e-2);
    CHECK_NEAR(p.y, 0.7071f, 2e-2);

    a.Update(0.6f); // переход завершён -> чистый Bend (90° -> ребёнок (-1,0,0))
    CHECK_FALSE(a.Fading());
    p = a.BoneMatrices()[1] * glm::vec4(0, 0, 0, 1);
    CHECK_NEAR(p.x, -1.0f, 1e-3);
    CHECK_NEAR(p.y, 0.0f, 1e-3);
    CHECK_EQ(a.CurrentClip(), 1);
}

TEST(Animator_play_cancels_crossfade) {
    Skeleton sk = MakeTwoBoneRig();
    std::vector<AnimationClip> clips{MakeRootRotClip("Idle", 0.0f), MakeRootRotClip("Bend", 90.0f)};
    Animator a; a.SetRig(&sk, &clips);
    a.Play("Idle"); a.Update(0.0f);
    a.CrossFade("Bend", 1.0f);
    CHECK_TRUE(a.Fading());
    a.Play("Idle");         // резкое переключение отменяет фейд
    CHECK_FALSE(a.Fading());
}

TEST(Animator_seek_collapses_crossfade) {
    // Переход — состояние, накопленное во времени; при явной перемотке его
    // нечем восстановить, поэтому он схлопывается в целевой клип.
    Skeleton sk = MakeTwoBoneRig();
    std::vector<AnimationClip> clips{MakeRootRotClip("Idle", 0.0f), MakeRootRotClip("Bend", 90.0f)};
    Animator a; a.SetRig(&sk, &clips);
    a.Play("Idle");
    a.CrossFade("Bend", 1.0f);
    CHECK_TRUE(a.Fading());
    a.Seek(0.5f);
    CHECK_FALSE(a.Fading());
    CHECK_TRUE(a.CurrentClip() == 1);
}

TEST(Animator_crossfade_without_current_is_play) {
    Skeleton sk = MakeTwoBoneRig();
    std::vector<AnimationClip> clips{MakeRootRotClip("Idle", 0.0f), MakeRootRotClip("Bend", 90.0f)};
    Animator a; a.SetRig(&sk, &clips);
    a.CrossFade("Bend", 0.5f); // ничего не играло -> эквивалент Play, без фейда
    CHECK_FALSE(a.Fading());
    CHECK_EQ(a.CurrentClip(), 1);
}

// --- Переопределение позы (ручная анимация костей) ---------------------------

TEST(Animator_pose_override_without_clip_moves_bone) {
    // Без единого клипа поза берётся из дефолтов скелета, а переопределение
    // должно её менять: это ровно тот случай, когда аниматор ставит позу с нуля.
    Skeleton sk = MakeTwoBoneRig();
    std::vector<AnimationClip> clips;
    Animator a;
    a.SetRig(&sk, &clips);

    std::vector<JointPose> pose(2);
    pose[0].HasRotation = true;
    pose[0].Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
    a.SetPoseOverride(&pose);
    a.RefreshPose();

    // Поворот корня на 90° вокруг Z переносит ребёнка из (0,1,0) в (-1,0,0).
    glm::vec4 p1 = a.BoneMatrices()[1] * glm::vec4(0, 0, 0, 1);
    CHECK_NEAR(p1.x, -1.0f, 1e-4);
    CHECK_NEAR(p1.y, 0.0f, 1e-4);
}

TEST(Animator_pose_override_beats_clip) {
    // Переопределение применяется ПОСЛЕ сэмплирования клипа, поэтому выигрывает.
    Skeleton sk = MakeTwoBoneRig();
    std::vector<AnimationClip> clips{MakeRootRotClip("Bend", 90.0f)};
    Animator a;
    a.SetRig(&sk, &clips);
    a.Play("Bend");
    a.Update(0.0f);

    std::vector<JointPose> pose(2);
    pose[0].HasRotation = true;
    pose[0].Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // разогнуть обратно
    a.SetPoseOverride(&pose);
    a.Update(0.0f);

    glm::vec4 p1 = a.BoneMatrices()[1] * glm::vec4(0, 0, 0, 1);
    CHECK_NEAR(p1.x, 0.0f, 1e-4);
    CHECK_NEAR(p1.y, 1.0f, 1e-4); // поза клипа перебита, ребёнок снова сверху
}

TEST(Animator_pose_override_is_per_channel) {
    // Переопределён только перенос — поворот обязан остаться от клипа.
    Skeleton sk = MakeTwoBoneRig();
    std::vector<AnimationClip> clips{MakeRootRotClip("Bend", 90.0f)};
    Animator a;
    a.SetRig(&sk, &clips);
    a.Play("Bend");

    std::vector<JointPose> pose(2);
    pose[0].HasTranslation = true;
    pose[0].Translation = {5.0f, 0.0f, 0.0f};
    a.SetPoseOverride(&pose);
    a.Update(0.0f);

    // Корень сдвинут на +5 по X, ребёнок по-прежнему повёрнут клипом в (-1,0,0).
    glm::vec4 p1 = a.BoneMatrices()[1] * glm::vec4(0, 0, 0, 1);
    CHECK_NEAR(p1.x, 4.0f, 1e-4);
    CHECK_NEAR(p1.y, 0.0f, 1e-4);
}

TEST(Animator_pose_override_shorter_than_skeleton_is_ok) {
    // Вектор переопределений короче скелета — обычное дело: кости, до которых
    // аниматор не дошёл, просто берутся от клипа. Выхода за границы быть не должно.
    Skeleton sk = MakeTwoBoneRig();
    std::vector<AnimationClip> clips;
    Animator a;
    a.SetRig(&sk, &clips);

    std::vector<JointPose> pose(1); // только корень
    pose[0].HasTranslation = true;
    pose[0].Translation = {0.0f, 2.0f, 0.0f};
    a.SetPoseOverride(&pose);
    a.RefreshPose();

    glm::vec4 p1 = a.BoneMatrices()[1] * glm::vec4(0, 0, 0, 1);
    CHECK_NEAR(p1.y, 3.0f, 1e-4); // 2 от корня + 1 от дефолта ребёнка
}

TEST(Animator_pose_override_can_be_removed) {
    Skeleton sk = MakeTwoBoneRig();
    std::vector<AnimationClip> clips;
    Animator a;
    a.SetRig(&sk, &clips);

    std::vector<JointPose> pose(2);
    pose[1].HasTranslation = true;
    pose[1].Translation = {0.0f, 7.0f, 0.0f};
    a.SetPoseOverride(&pose);
    a.RefreshPose();
    CHECK_NEAR((a.BoneMatrices()[1] * glm::vec4(0, 0, 0, 1)).y, 7.0f, 1e-4);

    a.SetPoseOverride(nullptr);
    a.RefreshPose();
    CHECK_NEAR((a.BoneMatrices()[1] * glm::vec4(0, 0, 0, 1)).y, 1.0f, 1e-4);
}

TEST(Animator_global_matrices_exclude_inverse_bind) {
    // Глобальные матрицы описывают ПОЛОЖЕНИЕ СУСТАВА, палитра — перенос вершин
    // из bind-позы. С нетривиальным inverseBind они обязаны разойтись, иначе
    // гизмо на кости встанет не туда.
    Skeleton sk = MakeTwoBoneRig();
    sk.Joints[1].InverseBind = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.0f, 0.0f));
    std::vector<AnimationClip> clips;
    Animator a;
    a.SetRig(&sk, &clips);

    CHECK_EQ((int)a.GlobalMatrices().size(), 2);
    const glm::vec4 joint = a.GlobalMatrices()[1] * glm::vec4(0, 0, 0, 1);
    CHECK_NEAR(joint.y, 1.0f, 1e-4); // сустав стоит на (0,1,0)
    const glm::vec4 skinned = a.BoneMatrices()[1] * glm::vec4(0, 0, 0, 1);
    CHECK_NEAR(skinned.y, 0.0f, 1e-4); // палитра в bind-позе ничего не двигает
}

// --- Ретаргет анимации между скелетами ---------------------------------------

#include "sage/anim/Retarget.h"

// Целевой скелет с ТАКОЙ ЖЕ топологией, но другими именами (префикс экспортёра),
// другими длинами костей и другой ориентацией сустава в бинд-позе. Именно это
// сочетание и ломает наивный перенос локальных поворотов.
static Skeleton MakeTargetRig() {
    Skeleton sk;
    sk.Joints.resize(2);
    sk.Joints[0].Name = "mixamorig:Root";
    sk.Joints[0].Parent = -1;
    sk.Joints[0].Translation = {0, 0, 0};
    sk.Joints[0].InverseBind = glm::mat4(1.0f);
    sk.Joints[1].Name = "mixamorig:Child_01";
    sk.Joints[1].Parent = 0;
    sk.Joints[1].Translation = {0, 2, 0}; // кость вдвое длиннее исходной
    sk.Joints[1].InverseBind = glm::mat4(1.0f);
    return sk;
}

static Skeleton MakeNamedSourceRig() {
    Skeleton sk = MakeTwoBoneRig();
    sk.Joints[0].Name = "Root";
    sk.Joints[1].Name = "child 01";
    return sk;
}

TEST(Retarget_normalizes_bone_names) {
    CHECK_TRUE(NormalizeBoneName("mixamorig:LeftArm") == "leftarm");
    CHECK_TRUE(NormalizeBoneName("Armature|Hips") == "hips");
    CHECK_TRUE(NormalizeBoneName("Hand_L") == "handl");
    CHECK_TRUE(NormalizeBoneName("hand.l") == "handl");
    CHECK_TRUE(NormalizeBoneName("Spine 02") == "spine02");
}

TEST(Retarget_maps_bones_by_name) {
    Skeleton src = MakeNamedSourceRig();
    Skeleton dst = MakeTargetRig();
    BoneMap map = MapByName(src, dst);
    CHECK_EQ(map.Mapped(), 2);
    CHECK_EQ(map.SourceToTarget[0], 0);
    CHECK_EQ(map.SourceToTarget[1], 1);
}

TEST(Retarget_unmatched_bones_are_dropped) {
    Skeleton src = MakeNamedSourceRig();
    src.Joints[1].Name = "TailTip"; // такой кости у цели нет
    Skeleton dst = MakeTargetRig();
    BoneMap map = MapByName(src, dst);
    CHECK_EQ(map.Mapped(), 1);
    CHECK_EQ(map.SourceToTarget[1], -1);

    AnimationClip clip = MakeRootRotClip("Bend", 90.0f);
    AnimChannel tail;                 // канал для кости без пары
    tail.Joint = 1;
    tail.Target = AnimPath::Rotation;
    tail.Times = {0.0f};
    tail.Values = {glm::vec4(0, 0, 0, 1)};
    clip.Channels.push_back(tail);

    AnimationClip out = Retarget(clip, src, dst, map);
    CHECK_EQ((int)out.Channels.size(), 1); // остался только канал корня
    CHECK_EQ(out.Channels[0].Joint, 0);
}

TEST(Retarget_preserves_pose_across_different_bind_rotations) {
    // Суть ретаргета: одинаковое ДВИЖЕНИЕ на скелетах с разной бинд-позой.
    // У цели корень в бинд-позе уже повёрнут на 30° — прямой перенос локального
    // поворота дал бы другую позу, перенос отклонения от бинд-позы даёт ту же.
    Skeleton src = MakeNamedSourceRig();
    Skeleton dst = MakeTargetRig();
    dst.Joints[0].Rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(0, 0, 1));

    AnimationClip clip = MakeRootRotClip("Bend", 90.0f); // источник гнёт корень на 90°
    AnimationClip out = Retarget(clip, src, dst);
    CHECK_EQ((int)out.Channels.size(), 1);

    // Ожидаем бинд-поворот цели, домноженный на то же отклонение (90°).
    const glm::quat expected = dst.Joints[0].Rotation *
                               (glm::inverse(src.Joints[0].Rotation) *
                                glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1)));
    const glm::vec4 v = out.Channels[0].Values[0];
    const glm::quat got(v.w, v.x, v.y, v.z);
    // Кватернионы q и -q — один поворот; сравниваем по модулю скалярного произведения.
    CHECK_NEAR(std::fabs(glm::dot(expected, got)), 1.0f, 1e-4);
}

TEST(Retarget_keeps_target_bone_lengths) {
    // Перенос кости — это её длина, и она принадлежит цели: канал переноса
    // НЕкорневой кости переноситься не должен, иначе персонаж «вытянется» под
    // того, с кого снято движение.
    Skeleton src = MakeNamedSourceRig();
    Skeleton dst = MakeTargetRig();

    AnimationClip clip;
    clip.Name = "Move";
    clip.Duration = 1.0f;
    AnimChannel childMove;
    childMove.Joint = 1;
    childMove.Target = AnimPath::Translation;
    childMove.Times = {0.0f};
    childMove.Values = {glm::vec4(0, 5, 0, 0)};
    clip.Channels.push_back(childMove);

    AnimationClip out = Retarget(clip, src, dst);
    CHECK_EQ((int)out.Channels.size(), 0);
    // Длина кости цели осталась своей.
    CHECK_NEAR(dst.Joints[1].Translation.y, 2.0f, 1e-5);
}

TEST(Retarget_scales_root_motion_to_target_size) {
    // Цель вдвое крупнее источника — шаг корня должен вырасти вдвое, иначе
    // высокий персонаж семенил бы шагами низкого.
    Skeleton src = MakeNamedSourceRig(); // размах 1
    Skeleton dst = MakeTargetRig();      // размах 2

    AnimationClip clip;
    clip.Name = "Walk";
    clip.Duration = 1.0f;
    AnimChannel rootMove;
    rootMove.Joint = 0;
    rootMove.Target = AnimPath::Translation;
    rootMove.Times = {0.0f, 1.0f};
    rootMove.Values = {glm::vec4(0, 0, 0, 0), glm::vec4(1, 0, 0, 0)}; // шаг на 1 вперёд
    clip.Channels.push_back(rootMove);

    AnimationClip out = Retarget(clip, src, dst);
    CHECK_EQ((int)out.Channels.size(), 1);
    CHECK_NEAR(out.Channels[0].Values[0].x, 0.0f, 1e-4);
    CHECK_NEAR(out.Channels[0].Values[1].x, 2.0f, 1e-4); // шаг удвоился
}

TEST(Retarget_result_plays_on_target_skeleton) {
    // Итоговая проверка: перенесённый клип реально проигрывается на цели.
    Skeleton src = MakeNamedSourceRig();
    Skeleton dst = MakeTargetRig();
    AnimationClip clip = MakeRootRotClip("Bend", 90.0f);

    std::vector<AnimationClip> clips{Retarget(clip, src, dst)};
    Animator a;
    a.SetRig(&dst, &clips);
    CHECK_TRUE(a.Play("Bend"));
    a.Update(0.0f);

    // Кость 1 цели стоит на (0,2,0); поворот корня на 90° вокруг Z уводит её в (-2,0,0).
    glm::vec4 p1 = a.BoneMatrices()[1] * glm::vec4(0, 0, 0, 1);
    CHECK_NEAR(p1.x, -2.0f, 1e-3);
    CHECK_NEAR(p1.y, 0.0f, 1e-3);
}
