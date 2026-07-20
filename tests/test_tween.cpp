// Модульные тесты системы Tween — сглаживание (Ease) и TweenManager: без GL.
#include "TestFramework.h"

#include "sage/core/Tween.h"

#include <glm/glm.hpp>

using namespace sage;

TEST(Ease_endpoints_and_linear) {
    // Все кривые проходят через (0->0) и (1->1).
    Easing all[] = {Easing::Linear, Easing::QuadIn, Easing::QuadOut, Easing::QuadInOut,
                    Easing::CubicIn, Easing::CubicOut, Easing::CubicInOut, Easing::SineIn,
                    Easing::SineOut, Easing::SineInOut, Easing::ExpoOut, Easing::BackOut,
                    Easing::ElasticOut, Easing::BounceOut};
    for (Easing e : all) {
        CHECK_NEAR(Ease(e, 0.0f), 0.0f, 1e-3);
        CHECK_NEAR(Ease(e, 1.0f), 1.0f, 1e-3);
    }
    CHECK_NEAR(Ease(Easing::Linear, 0.5f), 0.5f, 1e-4);
    CHECK_NEAR(Ease(Easing::QuadIn, 0.5f), 0.25f, 1e-4);   // 0.5^2
    CHECK_NEAR(Ease(Easing::CubicIn, 0.5f), 0.125f, 1e-4); // 0.5^3
}

TEST(Ease_quadout_is_faster_at_start) {
    // Ease-out обгоняет линейную в первой половине (быстрый старт, плавный конец).
    CHECK_TRUE(Ease(Easing::QuadOut, 0.25f) > 0.25f);
    CHECK_TRUE(Ease(Easing::QuadIn, 0.25f) < 0.25f); // ease-in — наоборот
}

TEST(Ease_back_overshoots) {
    // BackOut выходит за 1.0 к концу (пружинит) — характерная черта.
    bool overshoots = false;
    for (float t = 0.5f; t < 1.0f; t += 0.02f)
        if (Ease(Easing::BackOut, t) > 1.0f) overshoots = true;
    CHECK_TRUE(overshoots);
}

TEST(Tween_float_reaches_target_and_completes) {
    TweenManager tm;
    float value = 0.0f;
    bool done = false;
    tm.To<float>(0.0f, 10.0f, 1.0f, Easing::Linear,
                 [&](const float& v) { value = v; },
                 [&] { done = true; });
    CHECK_EQ(tm.ActiveCount(), 1);

    // 0.5 с линейно -> середина.
    tm.Update(0.5f);
    CHECK_NEAR(value, 5.0f, 1e-3);
    CHECK_FALSE(done);

    // Ещё 0.6 с -> перелёт за конец: значение = цель, onComplete, удалён.
    tm.Update(0.6f);
    CHECK_NEAR(value, 10.0f, 1e-3);
    CHECK_TRUE(done);
    CHECK_EQ(tm.ActiveCount(), 0);
}

TEST(Tween_vec3_interpolates) {
    TweenManager tm;
    glm::vec3 pos(0.0f);
    tm.To<glm::vec3>({0, 0, 0}, {4, 8, -2}, 1.0f, Easing::Linear,
                     [&](const glm::vec3& v) { pos = v; });
    tm.Update(0.25f);
    CHECK_NEAR(pos.x, 1.0f, 1e-3);
    CHECK_NEAR(pos.y, 2.0f, 1e-3);
    CHECK_NEAR(pos.z, -0.5f, 1e-3);
}

TEST(Tween_delay_holds_before_start) {
    TweenManager tm;
    float value = -1.0f;
    tm.To<float>(0.0f, 1.0f, 1.0f, Easing::Linear,
                 [&](const float& v) { value = v; }, {}, /*delay*/ 0.5f);
    tm.Update(0.4f); // ещё в задержке -> сеттер не звали
    CHECK_NEAR(value, -1.0f, 1e-4);
    tm.Update(0.35f); // прошло 0.75, за вычетом delay 0.5 -> t=0.25
    CHECK_NEAR(value, 0.25f, 1e-3);
}

TEST(Tween_pingpong_returns_toward_start) {
    TweenManager tm;
    float value = 0.0f;
    tm.To<float>(0.0f, 10.0f, 1.0f, Easing::Linear,
                 [&](const float& v) { value = v; }, {}, 0.0f, TweenLoop::PingPong);
    tm.Update(1.0f);  // достигли конца -> своп, начинаем обратно
    tm.Update(0.5f);  // половина обратного пути: 10 -> 0, t=0.5 -> 5
    CHECK_NEAR(value, 5.0f, 1e-3);
    CHECK_TRUE(tm.ActiveCount() == 1); // луп не завершается сам
}

TEST(Tween_cancel_stops_updates) {
    TweenManager tm;
    float value = 0.0f;
    TweenId id = tm.To<float>(0.0f, 10.0f, 1.0f, Easing::Linear,
                              [&](const float& v) { value = v; });
    tm.Update(0.5f);
    CHECK_NEAR(value, 5.0f, 1e-3);
    CHECK_TRUE(tm.IsActive(id));
    tm.Cancel(id);
    tm.Update(0.5f);            // отменён -> удалён, значение не двигается
    CHECK_NEAR(value, 5.0f, 1e-3);
    CHECK_FALSE(tm.IsActive(id));
    CHECK_EQ(tm.ActiveCount(), 0);
}

TEST(Tween_added_from_callback_is_safe) {
    // onComplete запускает НОВЫЙ твин — не должно ронять/портить вектор
    // (новые копятся в pending). Второй твин доезжает на следующих апдейтах.
    TweenManager tm;
    float value = 0.0f;
    tm.To<float>(0.0f, 1.0f, 1.0f, Easing::Linear,
                 [&](const float& v) { value = v; },
                 [&] {
                     tm.To<float>(1.0f, 2.0f, 1.0f, Easing::Linear,
                                  [&](const float& v) { value = v; });
                 });
    tm.Update(1.0f); // первый завершился, добавил второй в pending
    CHECK_NEAR(value, 1.0f, 1e-3);
    CHECK_EQ(tm.ActiveCount(), 1); // второй ждёт
    tm.Update(1.0f); // второй доехал
    CHECK_NEAR(value, 2.0f, 1e-3);
    CHECK_EQ(tm.ActiveCount(), 0);
}
