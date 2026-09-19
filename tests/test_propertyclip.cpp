// ===========================================================================
//  КЛИП ПО СВОЙСТВАМ: ключи, интерполяция, файл, «только для чтения».
//
//  Пока этот счёт жил бы внутри инструмента анимации, единственным способом
//  заметить ошибку был бы взгляд на экран, где что-то шевелится: «разгон
//  слишком резкий» и «ключ сработал на кадр позже» глазами не различаются.
//  Здесь время и значения — числа, и проверяются числами.
// ===========================================================================
#include "TestFramework.h"

#include "sage/anim/PropertyClip.h"

#include <cmath>

using sage::anim::Interp;
using sage::anim::Key;
using sage::anim::PropertyClip;
using sage::anim::Track;

namespace {

Track TwoKeys(Interp interp) {
    Track t;
    sage::anim::SetKey(t, 0.0f, glm::vec4(0.0f), interp);
    sage::anim::SetKey(t, 1.0f, glm::vec4(10.0f, 20.0f, 0.0f, 0.0f), interp);
    return t;
}

} // namespace

TEST(PropertyClip_linear_goes_straight) {
    const Track t = TwoKeys(Interp::Linear);
    CHECK_NEAR(sage::anim::Sample(t, 0.0f).x, 0.0f, 1e-4f);
    CHECK_NEAR(sage::anim::Sample(t, 0.5f).x, 5.0f, 1e-4f);
    CHECK_NEAR(sage::anim::Sample(t, 0.5f).y, 10.0f, 1e-4f);
    CHECK_NEAR(sage::anim::Sample(t, 1.0f).x, 10.0f, 1e-4f);
}

TEST(PropertyClip_constant_holds_until_the_next_key) {
    // Ступенька: значение держится до следующего ключа и переключается скачком.
    // Ровно это и нужно миганию и смене кадра спрайта, и линейная
    // интерполяция здесь даёт плавное «полупрозрачное» состояние, которого в
    // жизни не бывает.
    const Track t = TwoKeys(Interp::Constant);
    CHECK_NEAR(sage::anim::Sample(t, 0.0f).x, 0.0f, 1e-4f);
    CHECK_NEAR(sage::anim::Sample(t, 0.99f).x, 0.0f, 1e-4f);
    CHECK_NEAR(sage::anim::Sample(t, 1.0f).x, 10.0f, 1e-4f);
}

TEST(PropertyClip_outside_the_keys_holds_the_edges) {
    // Клип не обязан начинаться в нуле и кончаться в Duration: до первого ключа
    // держится первый, после последнего — последний. Иначе движение, занимающее
    // середину клипа, в начале прыгало бы в нули.
    Track t;
    sage::anim::SetKey(t, 0.25f, glm::vec4(3.0f), Interp::Linear);
    sage::anim::SetKey(t, 0.75f, glm::vec4(7.0f), Interp::Linear);
    CHECK_NEAR(sage::anim::Sample(t, 0.0f).x, 3.0f, 1e-4f);
    CHECK_NEAR(sage::anim::Sample(t, 10.0f).x, 7.0f, 1e-4f);
}

TEST(PropertyClip_bezier_with_flat_tangents_eases) {
    // Безье с ПЛОСКИМИ касательными — это «мягкий старт, мягкая остановка»:
    // середина остаётся на середине (кривая симметрична), а четверти
    // отодвигаются к краям. Проверяем именно это, а не форму вообще: цифры
    // конкретной кривой зависят от касательных, а симметрия — от правильности
    // счёта.
    Track t;
    Key a;
    a.Time = 0.0f;
    a.Value = glm::vec4(0.0f);
    a.Out = Interp::Bezier;
    a.OutTangent = {0.5f, 0.0f};   // доля отрезка: выходит горизонтально
    Key b;
    b.Time = 1.0f;
    b.Value = glm::vec4(10.0f);
    b.Out = Interp::Linear;
    b.InTangent = {-0.5f, 0.0f};   // входит горизонтально: тормозит к концу
    t.Keys = {a, b};

    CHECK_NEAR(sage::anim::Sample(t, 0.5f).x, 5.0f, 0.05f);
    // Четверть пути по времени даёт МЕНЬШЕ четверти по значению: это и есть
    // разгон. На прямой было бы ровно 2.5.
    CHECK_TRUE(sage::anim::Sample(t, 0.25f).x < 2.4f);
    // И симметрично на торможении.
    CHECK_TRUE(sage::anim::Sample(t, 0.75f).x > 7.6f);
    // Монотонность: значение не имеет права идти назад на возрастающей кривой.
    float prev = -1.0f;
    for (int i = 0; i <= 20; ++i) {
        const float v = sage::anim::Sample(t, (float)i / 20.0f).x;
        CHECK_TRUE(v >= prev - 1e-3f);
        prev = v;
    }
}

TEST(PropertyClip_set_key_replaces_and_sorts) {
    Track t;
    sage::anim::SetKey(t, 1.0f, glm::vec4(1.0f));
    sage::anim::SetKey(t, 0.0f, glm::vec4(0.0f));
    sage::anim::SetKey(t, 0.5f, glm::vec4(5.0f));
    CHECK_EQ((int)t.Keys.size(), 3);
    CHECK_NEAR(t.Keys[0].Time, 0.0f, 1e-5f);
    CHECK_NEAR(t.Keys[1].Time, 0.5f, 1e-5f);
    CHECK_NEAR(t.Keys[2].Time, 1.0f, 1e-5f);

    // Ключ в то же время — это ЗАМЕНА, а не второй ключ: иначе порядок между
    // ними зависел бы от того, в каком порядке их создавали, и «поправил
    // значение» давало бы мигание между старым и новым.
    sage::anim::SetKey(t, 0.5f, glm::vec4(9.0f));
    CHECK_EQ((int)t.Keys.size(), 3);
    CHECK_NEAR(t.Keys[1].Value.x, 9.0f, 1e-5f);
}

TEST(PropertyClip_wrap_time_loops_and_clamps) {
    // Зацикленный клип идёт по кругу, незацикленный ОСТАЁТСЯ В КОНЦЕ. Простой
    // остаток от деления дал бы прыжок в начало ровно на последнем кадре —
    // движение, которое «дёргается, когда заканчивается».
    CHECK_NEAR(sage::anim::WrapTime(2.5f, 2.0f, true), 0.5f, 1e-4f);
    CHECK_NEAR(sage::anim::WrapTime(2.5f, 2.0f, false), 2.0f, 1e-4f);
    // Отрицательное время (клип отматывают назад) не имеет права уйти за начало.
    CHECK_NEAR(sage::anim::WrapTime(-0.5f, 2.0f, true), 1.5f, 1e-4f);
    CHECK_NEAR(sage::anim::WrapTime(-0.5f, 2.0f, false), 0.0f, 1e-4f);
}

TEST(PropertyClip_file_round_trip) {
    PropertyClip c;
    c.Name = "Мигание";
    c.Duration = 2.0f;
    c.Loop = false;
    Track t;
    t.Target = "Icon";
    t.Property = "fill.color";
    sage::anim::SetKey(t, 0.0f, glm::vec4(1, 0, 0, 1), Interp::Bezier);
    sage::anim::SetKey(t, 1.0f, glm::vec4(0, 1, 0, 1), Interp::Constant);
    t.Keys[0].OutTangent = {0.3f, 0.2f};
    c.Tracks.push_back(t);
    c.Markers.push_back({0.5f, "удар"});

    std::string err;
    PropertyClip back;
    CHECK_TRUE(sage::anim::FromJsonString(sage::anim::ToJsonString(c), back, err));
    CHECK_TRUE(back.Name == "Мигание");
    CHECK_NEAR(back.Duration, 2.0f, 1e-5f);
    CHECK_FALSE(back.Loop);
    CHECK_EQ((int)back.Tracks.size(), 1);
    CHECK_TRUE(back.Tracks[0].Target == "Icon");
    CHECK_TRUE(back.Tracks[0].Property == "fill.color");
    CHECK_EQ((int)back.Tracks[0].Keys.size(), 2);
    CHECK_TRUE(back.Tracks[0].Keys[0].Out == Interp::Bezier);
    CHECK_NEAR(back.Tracks[0].Keys[0].OutTangent.x, 0.3f, 1e-5f);
    CHECK_TRUE(back.Tracks[0].Keys[1].Out == Interp::Constant);
    CHECK_EQ((int)back.Markers.size(), 1);
    CHECK_TRUE(back.Markers[0].Name == "удар");
}

TEST(PropertyClip_imported_flag_survives_the_file) {
    // «Пришёл из модели» — свойство САМОГО КЛИПА, и переживать файл оно
    // обязано: иначе после перезапуска редактора импортированный клип стал бы
    // обычным, и правку в нём съел бы следующий переимпорт модели.
    PropertyClip c;
    c.Imported = true;
    std::string err;
    PropertyClip back;
    CHECK_TRUE(sage::anim::FromJsonString(sage::anim::ToJsonString(c), back, err));
    CHECK_TRUE(back.Imported);
}

TEST(PropertyClip_file_sorts_keys_it_was_given_out_of_order) {
    // Файл могли поправить руками. Несортированные ключи дали бы движение
    // рывками, а не ошибку, — поэтому порядок наводит сам загрузчик.
    const std::string text = R"({
      "name": "Ручной", "duration": 1.0,
      "tracks": [{"property": "object.position", "keys": [
        {"time": 1.0, "value": [10,0,0,0]},
        {"time": 0.0, "value": [0,0,0,0]}
      ]}]
    })";
    PropertyClip c;
    std::string err;
    CHECK_TRUE(sage::anim::FromJsonString(text, c, err));
    CHECK_NEAR(c.Tracks[0].Keys[0].Time, 0.0f, 1e-5f);
    CHECK_NEAR(sage::anim::Sample(c.Tracks[0], 0.5f).x, 5.0f, 1e-4f);
}

TEST(PropertyClip_bezier_with_default_tangents_is_a_straight_line) {
    // Ключ, только что переключённый в безье, не имеет права изменить
    // движение: человек просил возможность настроить кривую, а не другое
    // движение. Поэтому касательные по умолчанию — треть отрезка по обеим
    // осям, то есть ровно прямая.
    Track bez = TwoKeys(Interp::Bezier);
    const Track lin = TwoKeys(Interp::Linear);
    for (int i = 0; i <= 10; ++i) {
        const float t = (float)i / 10.0f;
        CHECK_NEAR(sage::anim::Sample(bez, t).x, sage::anim::Sample(lin, t).x, 1e-3f);
    }
}
