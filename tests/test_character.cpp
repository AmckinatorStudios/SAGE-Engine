// Контроллер персонажа как чистый алгоритм: CharacterMotor поверх «занят ли
// объём». Мира физики здесь нет вовсе — твердь описывается воксельной сеткой
// прямо в тесте, ровно так же, как её описывает игра на скриптах.
//
// ЗАЧЕМ ОТДЕЛЬНЫЙ ФАЙЛ. Раньше персонаж жил внутри Jolt-бэкенда, и все проверки
// начинались со строчки «if (!HasJolt()) return;» — то есть в сборке без Jolt
// ходьба, ступеньки и опора не проверялись ничем. Здесь проверяется сам
// алгоритм, и он один и тот же у встроенного бэкенда и у игры со своим миром.
#include "TestFramework.h"

#include "sage/physics/CharacterMotor.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <tuple>

using namespace sage::physics;

namespace {

// Воксельный мирок теста: множество занятых клеток 1x1x1. Клетка (x,y,z)
// занимает объём [x,x+1) × [y,y+1) × [z,z+1) — то же соглашение, что у игры.
struct VoxelWorld {
    std::set<std::tuple<int, int, int>> cells;

    void Fill(int x0, int y0, int z0, int x1, int y1, int z1) {
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y)
                for (int z = z0; z <= z1; ++z) cells.insert({x, y, z});
    }

    bool Solid(const glm::vec3& min, const glm::vec3& max) const {
        const int x0 = (int)std::floor(min.x), x1 = (int)std::floor(max.x);
        const int y0 = (int)std::floor(min.y), y1 = (int)std::floor(max.y);
        const int z0 = (int)std::floor(min.z), z1 = (int)std::floor(max.z);
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y)
                for (int z = z0; z <= z1; ++z)
                    if (cells.count({x, y, z})) return true;
        return false;
    }

    SolidQuery Query() const {
        return [this](const glm::vec3& a, const glm::vec3& b) { return Solid(a, b); };
    }
};

constexpr float kDt = 1.0f / 60.0f;

// Ходьба с тяготением, как её ведёт игра: скорость по горизонтали задаётся,
// вертикальная копится сама. Мотор намеренно не делает этого за игру — тяготение
// и прыжок её правила, а не физики.
void Walk(CharacterMotor& m, const VoxelWorld& w, float vx, float vz, int frames,
          float* vy = nullptr) {
    float localVy = 0.0f;
    float& fall = vy ? *vy : localVy;
    for (int i = 0; i < frames; ++i) {
        fall = m.State().Grounded ? -1.0f : fall - 22.0f * kDt;
        m.Move(w.Query(), {vx, fall, vz}, kDt);
        if (m.State().Grounded) fall = -1.0f;
    }
}

CharacterMotor MakeMotor(const glm::vec3& feet, float step = 0.6f) {
    CharacterDesc d;
    d.Radius = 0.3f;
    d.Height = 1.8f;
    d.StepHeight = step;
    d.Position = feet;
    CharacterMotor m;
    m.Configure(d);
    return m;
}

} // namespace

TEST(Character_motor_stands_on_the_floor_and_does_not_sink) {
    VoxelWorld w;
    w.Fill(-10, -1, -10, 10, -1, 10);   // пол — слой на y = -1..0

    CharacterMotor m = MakeMotor({0.5f, 2.0f, 0.5f});
    Walk(m, w, 0.0f, 0.0f, 120);

    std::printf("       стоит: y=%.4f опора=%d\n", m.State().Position.y, (int)m.State().Grounded);
    CHECK_TRUE(m.State().Grounded);
    // Подошва ровно на верхней грани пола, с точностью до зазора.
    CHECK_TRUE(std::abs(m.State().Position.y) < 0.01f);
}

TEST(Character_motor_is_stopped_by_a_wall_and_slides_along_it) {
    VoxelWorld w;
    w.Fill(-10, -1, -10, 10, -1, 10);
    w.Fill(3, 0, -10, 3, 3, 10);        // стена в четыре блока высотой на x = 3..4

    CharacterMotor m = MakeMotor({0.5f, 0.0f, 0.5f});
    Walk(m, w, 4.0f, 0.0f, 180);

    std::printf("       упёрся: x=%.3f (стена начинается на 3.0)\n", m.State().Position.x);
    // Радиус 0.3 — центр не ближе 2.7 к грани стены.
    CHECK_TRUE(m.State().Position.x > 2.5f && m.State().Position.x < 2.71f);
    CHECK_TRUE(m.Blocked());
    CHECK_TRUE(m.State().Grounded);

    // Вдоль стены — идёт свободно: упор по одной оси не должен гасить другую.
    const float z0 = m.State().Position.z;
    Walk(m, w, 4.0f, 3.0f, 60);
    std::printf("       вдоль стены прошёл dz=%.2f\n", m.State().Position.z - z0);
    CHECK_TRUE(m.State().Position.z - z0 > 2.0f);
}

TEST(Character_motor_steps_onto_a_low_ledge_and_lands_on_top_of_it) {
    VoxelWorld w;
    w.Fill(-10, -1, -10, 10, -1, 10);
    // Полублок высотой 0.5 сделать в целочисленной сетке нельзя, поэтому берём
    // ступеньку в целый блок и высоту шага чуть больше блока.
    w.Fill(3, 0, -10, 6, 0, 10);

    CharacterMotor m = MakeMotor({0.5f, 0.0f, 0.5f}, /*step=*/1.05f);
    Walk(m, w, 3.0f, 0.0f, 120);

    std::printf("       после ступеньки: x=%.2f y=%.3f опора=%d\n", m.State().Position.x,
                m.State().Position.y, (int)m.State().Grounded);
    CHECK_TRUE(m.State().Position.x > 3.5f);
    // СТОИТ НА ступеньке, а не висит над ней и не застрял перед ней.
    CHECK_TRUE(std::abs(m.State().Position.y - 1.0f) < 0.01f);
    CHECK_TRUE(m.State().Grounded);
}

// --- Тот самый баг ------------------------------------------------------------
//
// «Я могу забраться на любую высоту типа степ-шаги». Наивная ступенька («упёрся
// — поднимись и попробуй снова») повторяется КАЖДЫЙ КАДР и с каждым разом
// начинает подъём с новой высоты: держа W у отвесной стены, игрок за пару
// секунд оказывается на её верхушке, ни разу не прыгнув. Ни одна стена в игре
// после этого не является преградой.
//
// Лечится тем, что подъём обязан ЗАКОНЧИТЬСЯ ПОСАДКОЙ НА ОПОРУ: не нашлось
// поверхности в пределах высоты шага — шага не было.
TEST(Character_motor_cannot_climb_a_wall_by_repeated_steps) {
    VoxelWorld w;
    w.Fill(-10, -1, -10, 10, -1, 10);
    w.Fill(3, 0, -10, 3, 9, 10);        // отвес в десять блоков

    CharacterMotor m = MakeMotor({0.5f, 0.0f, 0.5f});

    // Десять секунд упорной ходьбы в стену — столько, что наивная реализация
    // успела бы подняться на добрую сотню блоков.
    float peak = 0.0f;
    for (int i = 0; i < 600; ++i) {
        Walk(m, w, 5.0f, 0.0f, 1);
        peak = std::max(peak, m.State().Position.y);
    }

    std::printf("       десять секунд в стену: максимум y=%.3f, конец y=%.3f x=%.2f\n", peak,
                m.State().Position.y, m.State().Position.x);
    CHECK_TRUE(peak < 0.01f);            // НИ РАЗУ не поднялся
    CHECK_TRUE(m.State().Position.x < 2.71f);
    CHECK_TRUE(m.State().Grounded);
}

// Тот же отвес, но подходим к нему прыжками: приземление на верхушку стены
// разрешено, а вот «дошагать» до неё — нет. Проверяет, что лечение бага не
// превратилось в запрет подниматься вообще.
TEST(Character_motor_still_climbs_a_stack_of_low_steps) {
    VoxelWorld w;
    w.Fill(-10, -1, -10, 10, -1, 10);
    // Лестница: каждая следующая ступень на блок выше предыдущей, а за ней
    // площадка — иначе персонаж взойдёт наверх и тут же сойдёт с последней
    // ступени обратно на пол, и по конечной высоте подъёма не увидеть.
    for (int i = 0; i < 5; ++i) w.Fill(2 + i, 0, -10, 2 + i, i, 10);
    w.Fill(7, 0, -10, 14, 4, 10);

    CharacterMotor m = MakeMotor({0.5f, 0.0f, 0.5f}, /*step=*/1.05f);
    Walk(m, w, 2.0f, 0.0f, 300);

    std::printf("       лестница: x=%.2f y=%.2f\n", m.State().Position.x, m.State().Position.y);
    CHECK_TRUE(m.State().Position.y > 4.5f);   // взошёл на верхнюю ступень
    CHECK_TRUE(m.State().Grounded);
}

TEST(Character_motor_reports_landing_as_an_edge_not_as_a_state) {
    VoxelWorld w;
    w.Fill(-10, -1, -10, 10, -1, 10);

    CharacterMotor m = MakeMotor({0.5f, 4.0f, 0.5f});

    int landings = 0;
    float vy = 0.0f;
    for (int i = 0; i < 240; ++i) {
        vy = m.State().Grounded ? 0.0f : vy - 22.0f * kDt;
        m.Move(w.Query(), {0.0f, vy, 0.0f}, kDt);
        if (m.Landed()) ++landings;
    }
    std::printf("       падений с высоты: %d (ожидается ровно одно)\n", landings);
    // Ровно одно: «приземлился» — край, а не состояние. Иначе звук удара о землю
    // играл бы каждый кадр, пока персонаж стоит.
    CHECK_TRUE(landings == 1);
    CHECK_TRUE(m.State().Grounded);
}

TEST(Character_motor_does_not_tunnel_through_a_wall_at_speed) {
    VoxelWorld w;
    w.Fill(-40, -1, -10, 40, -1, 10);
    w.Fill(10, 0, -10, 10, 3, 10);

    CharacterMotor m = MakeMotor({0.5f, 0.0f, 0.5f});
    // Сто метров в секунду и длинный кадр в придачу — за один шаг тело
    // пролетело бы стену насквозь, если бы шаг не дробился.
    for (int i = 0; i < 20; ++i) {
        m.Move(w.Query(), {100.0f, -1.0f, 0.0f}, 0.1f);
    }
    std::printf("       на скорости 100 м/с остановился на x=%.2f\n", m.State().Position.x);
    CHECK_TRUE(m.State().Position.x < 9.71f);
}

TEST(Character_motor_pushes_itself_out_when_walled_in) {
    VoxelWorld w;
    w.Fill(-10, -1, -10, 10, -1, 10);
    CharacterMotor m = MakeMotor({0.5f, 0.0f, 0.5f});

    // Замуровали: блок поставили прямо в персонажа. Без выталкивания он
    // остаётся внутри тверди, любое движение упирается в него самого, и игрок
    // просто замирает — молча, без единого сообщения.
    w.Fill(0, 0, 0, 0, 1, 0);
    Walk(m, w, 0.0f, 0.0f, 30);

    std::printf("       выбрался наверх: y=%.2f опора=%d\n", m.State().Position.y,
                (int)m.State().Grounded);
    CHECK_TRUE(m.State().Position.y > 1.9f);
    CHECK_TRUE(m.State().Grounded);
}

TEST(Character_motor_head_check_keeps_it_out_of_a_low_gallery) {
    VoxelWorld w;
    w.Fill(-10, -1, -10, 10, -1, 10);
    w.Fill(3, 0, -10, 8, 0, 10);        // ступенька в блок
    w.Fill(3, 1, -10, 8, 2, 10);        // и потолок прямо над ней: щели нет

    CharacterMotor m = MakeMotor({0.5f, 0.0f, 0.5f}, /*step=*/1.05f);
    Walk(m, w, 3.0f, 0.0f, 120);

    std::printf("       перед глухой ступенькой: x=%.2f y=%.2f\n", m.State().Position.x,
                m.State().Position.y);
    // Взойти некуда — над ступенькой сплошная стена. Остаёмся внизу, а не
    // залезаем внутрь перекрытия.
    CHECK_TRUE(m.State().Position.y < 0.01f);
    CHECK_TRUE(m.State().Position.x < 2.71f);
}
