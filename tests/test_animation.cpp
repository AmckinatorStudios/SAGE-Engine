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

// --- Обратная кинематика ------------------------------------------------------

#include "sage/anim/IK.h"

// Трёхкостная «рука»: плечо в начале координат, локоть на (0,1,0), кисть на
// (0,2,0). Длины звеньев по единице — удобно считать достижимость в уме.
static Skeleton MakeArmRig() {
    Skeleton sk;
    sk.Joints.resize(3);
    sk.Joints[0].Name = "Shoulder";
    sk.Joints[0].Parent = -1;
    sk.Joints[0].Translation = {0, 0, 0};
    sk.Joints[1].Name = "Elbow";
    sk.Joints[1].Parent = 0;
    sk.Joints[1].Translation = {0, 1, 0};
    sk.Joints[2].Name = "Hand";
    sk.Joints[2].Parent = 1;
    sk.Joints[2].Translation = {0, 1, 0};
    for (Joint& j : sk.Joints) j.InverseBind = glm::mat4(1.0f);
    return sk;
}

// Глобальные матрицы скелета в позе, заданной переопределениями (пустой вектор
// — бинд-поза). Отдельная функция: она нужна и до вызова солвера, и после —
// чтобы проверить, куда встал конец цепочки.
static std::vector<glm::mat4> PoseGlobals(const Skeleton& sk, const std::vector<JointPose>& pose) {
    Animator a;
    std::vector<AnimationClip> clips;
    a.SetRig(&sk, &clips);
    if (!pose.empty()) a.SetPoseOverride(&pose);
    a.RefreshPose();
    return a.GlobalMatrices();
}

TEST(IK_chain_from_end_collects_parents) {
    Skeleton sk = MakeArmRig();
    std::vector<int> chain = ChainFromEnd(sk, 2, 3);
    CHECK_EQ((int)chain.size(), 3);
    CHECK_EQ(chain[0], 0); // от корня к концу
    CHECK_EQ(chain[2], 2);
    // Цепочка длиннее скелета не набирается — упираемся в корень.
    CHECK_TRUE(ChainFromEnd(sk, 2, 5).empty());
}

TEST(IK_two_bone_reaches_target) {
    Skeleton sk = MakeArmRig();
    std::vector<JointPose> pose;
    std::vector<glm::mat4> globals = PoseGlobals(sk, pose);

    // Цель сбоку, в пределах досягаемости (расстояние 1.5 при размахе 2).
    const glm::vec3 target(1.5f, 0.0f, 0.0f);
    TwoBoneChain chain{0, 1, 2};
    IKResult result = SolveTwoBone(sk, globals, chain, target);
    CHECK_TRUE(result.Solved);
    CHECK_TRUE(result.Reached);

    ApplyIK(result, pose, sk.Count());
    std::vector<glm::mat4> solved = PoseGlobals(sk, pose);
    const glm::vec3 hand(solved[2][3]);
    CHECK_NEAR(glm::length(hand - target), 0.0f, 1e-3);
}

TEST(IK_two_bone_keeps_bone_lengths) {
    // Главное свойство IK: кости не растягиваются. Проверяем на цели, до
    // которой пришлось изрядно согнуться.
    Skeleton sk = MakeArmRig();
    std::vector<JointPose> pose;
    std::vector<glm::mat4> globals = PoseGlobals(sk, pose);

    IKResult result = SolveTwoBone(sk, globals, TwoBoneChain{0, 1, 2}, glm::vec3(0.8f, 0.6f, 0.0f));
    CHECK_TRUE(result.Solved);
    ApplyIK(result, pose, sk.Count());
    std::vector<glm::mat4> solved = PoseGlobals(sk, pose);

    const glm::vec3 shoulder(solved[0][3]), elbow(solved[1][3]), hand(solved[2][3]);
    CHECK_NEAR(glm::length(elbow - shoulder), 1.0f, 1e-3);
    CHECK_NEAR(glm::length(hand - elbow), 1.0f, 1e-3);
}

TEST(IK_two_bone_unreachable_target_stretches_toward_it) {
    // Цель дальше вытянутой руки: дотянуться нельзя, но рука обязана вытянуться
    // В ЕЁ СТОРОНУ, а не остаться висеть или растянуться до цели.
    Skeleton sk = MakeArmRig();
    std::vector<JointPose> pose;
    std::vector<glm::mat4> globals = PoseGlobals(sk, pose);

    const glm::vec3 target(10.0f, 0.0f, 0.0f);
    IKResult result = SolveTwoBone(sk, globals, TwoBoneChain{0, 1, 2}, target);
    CHECK_TRUE(result.Solved);
    CHECK_FALSE(result.Reached); // честно сообщает, что не достал

    ApplyIK(result, pose, sk.Count());
    std::vector<glm::mat4> solved = PoseGlobals(sk, pose);
    const glm::vec3 hand(solved[2][3]);
    // Рука вытянута почти на полный размах (2) и смотрит на цель.
    CHECK_NEAR(glm::length(hand), 2.0f, 1e-2);
    CHECK_TRUE(glm::dot(glm::normalize(hand), glm::normalize(target)) > 0.999f);
}

TEST(IK_two_bone_pole_target_chooses_bend_side) {
    // Полюс решает, в какую сторону выгибается локоть. Два разных полюса при
    // одной цели обязаны дать РАЗНЫЕ положения локтя — иначе полюс не работает.
    Skeleton sk = MakeArmRig();
    const std::vector<glm::mat4> globals = PoseGlobals(sk, {});
    const glm::vec3 target(1.5f, 0.0f, 0.0f);

    std::vector<JointPose> poseA;
    const glm::vec3 poleFront(0.0f, 0.0f, 3.0f);
    IKResult a = SolveTwoBone(sk, globals, TwoBoneChain{0, 1, 2}, target, &poleFront);
    ApplyIK(a, poseA, sk.Count());
    const glm::vec3 elbowA(PoseGlobals(sk, poseA)[1][3]);

    std::vector<JointPose> poseB;
    const glm::vec3 poleBack(0.0f, 0.0f, -3.0f);
    IKResult b = SolveTwoBone(sk, globals, TwoBoneChain{0, 1, 2}, target, &poleBack);
    ApplyIK(b, poseB, sk.Count());
    const glm::vec3 elbowB(PoseGlobals(sk, poseB)[1][3]);

    CHECK_TRUE(glm::length(elbowA - elbowB) > 0.3f);
    // Локоть уходит в сторону своего полюса.
    CHECK_TRUE(elbowA.z > elbowB.z);
}

TEST(IK_two_bone_is_deterministic) {
    // Аналитический солвер обязан давать один и тот же ответ на один и тот же
    // вход: при перемотке таймлайна кадр не должен зависеть от истории.
    Skeleton sk = MakeArmRig();
    const std::vector<glm::mat4> globals = PoseGlobals(sk, {});
    IKResult a = SolveTwoBone(sk, globals, TwoBoneChain{0, 1, 2}, glm::vec3(1.2f, 0.7f, 0.3f));
    IKResult b = SolveTwoBone(sk, globals, TwoBoneChain{0, 1, 2}, glm::vec3(1.2f, 0.7f, 0.3f));
    CHECK_EQ((int)a.Rotations.size(), (int)b.Rotations.size());
    for (size_t i = 0; i < a.Rotations.size(); ++i) {
        CHECK_NEAR(std::fabs(glm::dot(a.Rotations[i], b.Rotations[i])), 1.0f, 1e-6);
    }
}

TEST(IK_two_bone_rejects_bad_input) {
    Skeleton sk = MakeArmRig();
    const std::vector<glm::mat4> globals = PoseGlobals(sk, {});
    CHECK_FALSE(SolveTwoBone(sk, globals, TwoBoneChain{-1, 1, 2}, glm::vec3(1, 0, 0)).Solved);
    CHECK_FALSE(SolveTwoBone(sk, globals, TwoBoneChain{0, 1, 99}, glm::vec3(1, 0, 0)).Solved);
    // Цель ровно в корне: направления нет, решать нечего.
    CHECK_FALSE(SolveTwoBone(sk, globals, TwoBoneChain{0, 1, 2}, glm::vec3(0, 0, 0)).Solved);
}

TEST(IK_fabrik_chain_reaches_and_keeps_lengths) {
    // Длинная цепочка: пять костей по единице вдоль +Y.
    Skeleton sk;
    sk.Joints.resize(5);
    for (int i = 0; i < 5; ++i) {
        sk.Joints[i].Name = "j" + std::to_string(i);
        sk.Joints[i].Parent = i - 1;
        sk.Joints[i].Translation = {0.0f, i == 0 ? 0.0f : 1.0f, 0.0f};
        sk.Joints[i].InverseBind = glm::mat4(1.0f);
    }

    std::vector<JointPose> pose;
    std::vector<glm::mat4> globals = PoseGlobals(sk, pose);
    const std::vector<int> chain{0, 1, 2, 3, 4};
    const glm::vec3 target(2.0f, 1.5f, 0.5f);

    IKResult result = SolveChain(sk, globals, chain, target, 20, 1e-4f);
    CHECK_TRUE(result.Solved);
    CHECK_TRUE(result.Reached);

    ApplyIK(result, pose, sk.Count());
    std::vector<glm::mat4> solved = PoseGlobals(sk, pose);

    const glm::vec3 tip(solved[4][3]);
    CHECK_NEAR(glm::length(tip - target), 0.0f, 1e-2);
    // Корень остался на месте — иначе персонаж уезжал бы за собственной рукой.
    CHECK_NEAR(glm::length(glm::vec3(solved[0][3])), 0.0f, 1e-4);
    // Все звенья сохранили длину.
    for (int i = 0; i + 1 < 5; ++i) {
        const float len = glm::length(glm::vec3(solved[i + 1][3]) - glm::vec3(solved[i][3]));
        CHECK_NEAR(len, 1.0f, 1e-2);
    }
}

TEST(IK_apply_weight_blends_with_existing_pose) {
    // Половинный вес обязан дать позу МЕЖДУ исходной и решением IK: так IK
    // включается плавно, а не рывком на одном кадре.
    Skeleton sk = MakeArmRig();
    const std::vector<glm::mat4> globals = PoseGlobals(sk, {});
    IKResult result = SolveTwoBone(sk, globals, TwoBoneChain{0, 1, 2}, glm::vec3(1.5f, 0.0f, 0.0f));

    std::vector<JointPose> full;
    ApplyIK(result, full, sk.Count(), 1.0f);
    const glm::vec3 handFull(PoseGlobals(sk, full)[2][3]);

    std::vector<JointPose> half((size_t)sk.Count());
    half[0].HasRotation = true;
    half[0].Rotation = glm::quat(1, 0, 0, 0); // исходная поза — бинд
    half[1].HasRotation = true;
    half[1].Rotation = glm::quat(1, 0, 0, 0);
    ApplyIK(result, half, sk.Count(), 0.5f);
    const glm::vec3 handHalf(PoseGlobals(sk, half)[2][3]);

    const glm::vec3 handBind(PoseGlobals(sk, {})[2][3]);
    const float toFull = glm::length(handHalf - handFull);
    const float toBind = glm::length(handHalf - handBind);
    CHECK_TRUE(toFull > 1e-3f); // не совпало с полным решением
    CHECK_TRUE(toBind > 1e-3f); // и не осталось в исходной позе
}

TEST(IK_aim_turns_bone_toward_target) {
    // Кости рига смотрят вдоль +Y. Просим плечо посмотреть вбок (+X): ось
    // кости обязана лечь на направление к цели.
    Skeleton sk = MakeArmRig();
    const std::vector<glm::mat4> globals = PoseGlobals(sk, {});

    const glm::vec3 target(3.0f, 0.0f, 0.0f);
    IKResult res = SolveAim(sk, globals, 0, glm::vec3(0, 1, 0), target, 180.0f);
    CHECK_TRUE(res.Solved);

    std::vector<JointPose> pose;
    ApplyIK(res, pose, sk.Count());
    const std::vector<glm::mat4> solved = PoseGlobals(sk, pose);

    // Куда теперь смотрит ось кости — по положению её ребёнка.
    const glm::vec3 root(solved[0][3]), child(solved[1][3]);
    const glm::vec3 dir = glm::normalize(child - root);
    const glm::vec3 want = glm::normalize(target - root);
    CHECK_NEAR(glm::dot(dir, want), 1.0f, 1e-3);
}

TEST(IK_aim_respects_cone_limit) {
    // Цель ровно позади (-Y при исходном +Y) — разворот на 180°. С ограничением
    // в 45° кость обязана повернуться РОВНО на 45°, а не отказаться работать и
    // не вывернуться целиком: шея так не гнётся.
    Skeleton sk = MakeArmRig();
    const std::vector<glm::mat4> globals = PoseGlobals(sk, {});

    IKResult res = SolveAim(sk, globals, 0, glm::vec3(0, 1, 0), glm::vec3(0.0f, -2.0f, 0.0f), 45.0f);
    CHECK_TRUE(res.Solved);
    std::vector<JointPose> pose;
    ApplyIK(res, pose, sk.Count());
    const std::vector<glm::mat4> solved = PoseGlobals(sk, pose);

    const glm::vec3 dir = glm::normalize(glm::vec3(solved[1][3]) - glm::vec3(solved[0][3]));
    const float deg = glm::degrees(std::acos(glm::clamp(glm::dot(dir, glm::vec3(0, 1, 0)), -1.0f, 1.0f)));
    CHECK_NEAR(deg, 45.0f, 1.0);
}

TEST(IK_aim_end_sets_end_orientation) {
    // Доворот конца: кисти задаём мировую (модельную) ориентацию — поворот на
    // 90° вокруг Z — и проверяем, что она у неё именно такая. Это то, чем
    // ступню кладут на склон.
    Skeleton sk = MakeArmRig();
    const std::vector<glm::mat4> globals = PoseGlobals(sk, {});

    const glm::quat want = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
    IKResult res = AimEnd(sk, globals, 2, want);
    CHECK_TRUE(res.Solved);

    std::vector<JointPose> pose;
    ApplyIK(res, pose, sk.Count());
    const std::vector<glm::mat4> solved = PoseGlobals(sk, pose);
    const glm::quat got = glm::normalize(glm::quat_cast(glm::mat3(solved[2])));
    // Кватернионы q и -q — один поворот, поэтому сравниваем по модулю скалярного.
    CHECK_NEAR(std::fabs(glm::dot(got, want)), 1.0f, 1e-3);
}

TEST(IK_foot_lock_holds_contact_point_then_releases) {
    // Прилипание опорной ноги. Пока стопа внизу, цель не двигается ни на
    // сантиметр, хотя «свободная» цель уезжает вперёд каждый кадр — это и есть
    // лекарство от скольжения. Как только стопа поднялась, цель отпускается.
    bool locked = false;
    float blend = 0.0f;
    glm::vec3 lockedAt(0.0f);
    const glm::vec3 free(9.0f, 9.0f, 9.0f);
    const float dt = 0.016f, release = 0.12f;

    // Кадр касания: стопа на 0.05 при пороге 0.12.
    glm::vec3 t0 = FootLockTarget(locked, lockedAt, blend, dt, 0.05f, 0.12f, release,
                                  glm::vec3(1.0f, 0.0f, 0.0f), free);
    CHECK_TRUE(locked);
    CHECK_NEAR(blend, 1.0f, 1e-5);
    CHECK_NEAR(glm::length(t0 - glm::vec3(1.0f, 0.0f, 0.0f)), 0.0f, 1e-5);

    // Персонаж поехал вперёд, стопа в модели всё ещё внизу — держим ТУ ЖЕ точку.
    glm::vec3 t1 = FootLockTarget(locked, lockedAt, blend, dt, 0.02f, 0.12f, release,
                                  glm::vec3(1.4f, 0.0f, 0.0f), free);
    CHECK_NEAR(glm::length(t1 - t0), 0.0f, 1e-5);

    // Стопа поднялась — замок снят, но цель уезжает к свободной ПОСТЕПЕННО:
    // мгновенный отпуск дал бы щелчок на один кадр.
    glm::vec3 t2 = FootLockTarget(locked, lockedAt, blend, dt, 0.40f, 0.12f, release,
                                  glm::vec3(1.8f, 0.4f, 0.0f), free);
    CHECK_FALSE(locked);
    CHECK_TRUE(blend > 0.0f && blend < 1.0f);
    CHECK_TRUE(glm::length(t2 - t0) > 1e-3f);       // сдвинулась
    CHECK_TRUE(glm::length(t2 - free) > 1.0f);      // но далеко не дошла

    // Через releaseTime замок отпускает полностью.
    for (int i = 0; i < 20; ++i)
        FootLockTarget(locked, lockedAt, blend, dt, 0.40f, 0.12f, release,
                       glm::vec3(1.8f, 0.4f, 0.0f), free);
    CHECK_NEAR(blend, 0.0f, 1e-5);
    glm::vec3 t3 = FootLockTarget(locked, lockedAt, blend, dt, 0.40f, 0.12f, release,
                                  glm::vec3(1.8f, 0.4f, 0.0f), free);
    CHECK_NEAR(glm::length(t3 - free), 0.0f, 1e-5);

    // И следующее касание запоминает НОВУЮ точку, а не старую.
    glm::vec3 t4 = FootLockTarget(locked, lockedAt, blend, dt, 0.01f, 0.12f, release,
                                  glm::vec3(2.2f, 0.0f, 0.0f), free);
    CHECK_NEAR(glm::length(t4 - glm::vec3(2.2f, 0.0f, 0.0f)), 0.0f, 1e-5);
}

TEST(IK_aim_rejects_bad_input) {
    Skeleton sk = MakeArmRig();
    const std::vector<glm::mat4> globals = PoseGlobals(sk, {});
    CHECK_FALSE(SolveAim(sk, globals, -1, glm::vec3(0, 1, 0), glm::vec3(1, 0, 0)).Solved);
    CHECK_FALSE(SolveAim(sk, globals, 99, glm::vec3(0, 1, 0), glm::vec3(1, 0, 0)).Solved);
    // Нулевая ось и цель В САМОЙ кости — направления нет, поворачивать не на что.
    CHECK_FALSE(SolveAim(sk, globals, 0, glm::vec3(0, 0, 0), glm::vec3(1, 0, 0)).Solved);
    CHECK_FALSE(SolveAim(sk, globals, 0, glm::vec3(0, 1, 0), glm::vec3(0, 0, 0)).Solved);
    CHECK_FALSE(AimEnd(sk, globals, 99, glm::quat(1, 0, 0, 0)).Solved);
}

// --- Декодирование звука ------------------------------------------------------

#include "sage/audio/AudioEngine.h"

#include <cstdio>
#include <filesystem>

// Собирает валидный WAV в памяти. Проверять декодер на реальном файле нечем:
// ассетов в репозитории движка нет и быть не должно.
static std::vector<unsigned char> MakeWav(int sampleRate, int channels, int frames) {
    std::vector<unsigned char> out;
    auto put32 = [&](unsigned int v) {
        out.push_back((unsigned char)(v & 0xFF));
        out.push_back((unsigned char)((v >> 8) & 0xFF));
        out.push_back((unsigned char)((v >> 16) & 0xFF));
        out.push_back((unsigned char)((v >> 24) & 0xFF));
    };
    auto put16 = [&](unsigned short v) {
        out.push_back((unsigned char)(v & 0xFF));
        out.push_back((unsigned char)((v >> 8) & 0xFF));
    };
    auto tag = [&](const char* s) { for (int i = 0; i < 4; ++i) out.push_back((unsigned char)s[i]); };

    const int dataBytes = frames * channels * 2;
    tag("RIFF"); put32((unsigned)(36 + dataBytes)); tag("WAVE");
    tag("fmt "); put32(16); put16(1); put16((unsigned short)channels);
    put32((unsigned)sampleRate);
    put32((unsigned)(sampleRate * channels * 2));
    put16((unsigned short)(channels * 2)); put16(16);
    tag("data"); put32((unsigned)dataBytes);

    // Каналы намеренно РАЗНЫЕ: так видно, что сведение в моно усредняет, а не
    // берёт первый канал.
    for (int f = 0; f < frames; ++f) {
        for (int c = 0; c < channels; ++c) {
            const short value = (short)(c == 0 ? 16000 : -8000);
            put16((unsigned short)value);
        }
    }
    return out;
}

TEST(DecodeToMono_reads_wav_from_memory) {
    const std::vector<unsigned char> wav = MakeWav(44100, 2, 100);
    std::vector<float> samples;
    int rate = 0;
    CHECK_TRUE(AudioEngine::DecodeToMono(wav.data(), wav.size(), samples, rate));
    CHECK_EQ(rate, 44100);
    CHECK_EQ((int)samples.size(), 100);
    // (16000 + (-8000)) / 2 / 32768 ≈ 0.122 — именно среднее двух каналов.
    CHECK_NEAR(samples[0], 0.122f, 5e-3);
}

TEST(DecodeToMono_handles_mono_source) {
    const std::vector<unsigned char> wav = MakeWav(22050, 1, 64);
    std::vector<float> samples;
    int rate = 0;
    CHECK_TRUE(AudioEngine::DecodeToMono(wav.data(), wav.size(), samples, rate));
    CHECK_EQ(rate, 22050);
    CHECK_EQ((int)samples.size(), 64);
    CHECK_NEAR(samples[0], 16000.0f / 32768.0f, 5e-3);
}

TEST(DecodeToMono_reads_file) {
    const std::vector<unsigned char> wav = MakeWav(48000, 2, 256);
    const std::string path = "/tmp/sage_decode_test.wav";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    CHECK_TRUE(f != nullptr);
    if (f) {
        std::fwrite(wav.data(), 1, wav.size(), f);
        std::fclose(f);
    }

    std::vector<float> samples;
    int rate = 0;
    CHECK_TRUE(AudioEngine::DecodeToMono(path, samples, rate));
    CHECK_EQ(rate, 48000);
    CHECK_EQ((int)samples.size(), 256);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(DecodeToMono_rejects_garbage) {
    // Мусор и пустой вход не должны ни падать, ни выдавать «успех».
    const std::vector<unsigned char> garbage(512, 0xAB);
    std::vector<float> samples;
    int rate = 0;
    CHECK_FALSE(AudioEngine::DecodeToMono(garbage.data(), garbage.size(), samples, rate));
    CHECK_FALSE(AudioEngine::DecodeToMono(nullptr, 0, samples, rate));
    CHECK_FALSE(AudioEngine::DecodeToMono("/tmp/нет-такого-файла.wav", samples, rate));
}

// --- Синонимы имён костей при ретаргете ------------------------------------
// Библиотеки анимаций и модели делают разные люди по разным соглашениям:
// Unreal зовёт бедро thigh_l, Blender — UpperLeg.L, Mixamo — LeftUpLeg.
// Совпадение по буквам находит только руки и голову, и анимация выходит без ног.
TEST(Retarget_matches_bones_across_naming_conventions) {
    using namespace sage::anim;
    CHECK_EQ(CanonicalBoneName("thighl"), CanonicalBoneName("upperlegl"));
    CHECK_EQ(CanonicalBoneName("calfr"), CanonicalBoneName("lowerlegr"));
    CHECK_EQ(CanonicalBoneName("claviclel"), CanonicalBoneName("shoulderl"));
    CHECK_EQ(CanonicalBoneName("forearml"), CanonicalBoneName("lowerarml"));
    CHECK_EQ(CanonicalBoneName("pelvis"), CanonicalBoneName("hips"));

    // Сторона не теряется: левое не должно совпасть с правым.
    CHECK_TRUE(CanonicalBoneName("thighl") != CanonicalBoneName("thighr"));
    // Незнакомое имя остаётся собой, а не сваливается в общий мешок.
    CHECK_EQ(CanonicalBoneName("tailtip"), std::string("tailtip"));

    // Скелеты в двух разных соглашениях должны сойтись.
    Skeleton src, dst;
    src.Joints = {{"root"}, {"thigh_l"}, {"calf_l"}, {"clavicle_l"}, {"lowerarm_l"}};
    dst.Joints = {{"root"}, {"UpperLeg.L"}, {"LowerLeg.L"}, {"Shoulder.L"}, {"Forearm.L"}};
    for (auto& j : src.Joints) j.Parent = -1;
    for (auto& j : dst.Joints) j.Parent = -1;
    const BoneMap map = MapByName(src, dst);
    CHECK_EQ(map.Mapped(), 5);

    // Точное имя главнее синонима: если кость с тем же именем есть, берём её.
    Skeleton exact;
    exact.Joints = {{"thigh_l"}, {"UpperLeg.L"}};
    for (auto& j : exact.Joints) j.Parent = -1;
    Skeleton one;
    one.Joints = {{"thigh_l"}};
    one.Joints[0].Parent = -1;
    const BoneMap m2 = MapByName(one, exact);
    CHECK_EQ(m2.SourceToTarget[0], 0);
}

// --- Извлечение корневого движения -----------------------------------------
// Клип с запечённым перемещением: без извлечения меш уезжает от своей сущности,
// с извлечением поза стоит на месте, а смещение отдаётся наружу.
TEST(Animator_extracts_root_motion_and_survives_loop) {
    using namespace sage::anim;
    Skeleton sk;
    sk.Joints.resize(1);
    sk.Joints[0].Name = "root";
    sk.Joints[0].Parent = -1;

    // Корень едет вперёд на 2 метра за 2 секунды и на стыке цикла возвращается.
    AnimationClip clip;
    clip.Name = "Walk";
    clip.Duration = 2.0f;
    AnimChannel ch;
    ch.Joint = 0;
    ch.Target = AnimPath::Translation;
    ch.Times = {0.0f, 2.0f};
    ch.Values = {{0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 2.0f, 0.0f}};
    clip.Channels.push_back(ch);
    std::vector<AnimationClip> clips{clip};

    Animator a;
    a.SetRig(&sk, &clips);
    a.SetRootMotion(true);
    a.Play(0, /*loop=*/true);

    float travelled = 0.0f;
    for (int i = 0; i < 200; ++i) {           // 4 секунды = два полных цикла
        a.Update(0.02f);
        travelled += a.ConsumeRootDelta().z;
    }
    // Два цикла по два метра. Откат на стыке не должен вычитаться — иначе
    // персонаж раз в цикл прыгал бы назад на всю пройденную дистанцию.
    CHECK_TRUE(travelled > 3.5f);
    CHECK_TRUE(travelled < 4.5f);

    // Поза при этом стоит на месте: перемещение снято с корня.
    const glm::mat4& root = a.GlobalMatrices()[0];
    CHECK_NEAR(root[3][2], 0.0f, 1e-3);

    // Забрали — второй раз не отдаётся: иначе два читателя сдвинули бы дважды.
    // Два шага, а не один: ровно на стыке цикла шаг приходится на отброшенный
    // откат, и смещения в этом кадре законно нет.
    a.Update(0.02f);
    a.ConsumeRootDelta();
    a.Update(0.02f);
    const glm::vec3 first = a.ConsumeRootDelta();
    const glm::vec3 second = a.ConsumeRootDelta();
    CHECK_TRUE(glm::length(first) > 0.0f);
    CHECK_NEAR(glm::length(second), 0.0f, 1e-6);
}
