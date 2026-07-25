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
