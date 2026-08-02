// Выбор объекта мышью: луч против коробки и против треугольников.
//
// Тест написан по конкретной жалобе — «кликаю прямо по объекту, а выбирается не
// он». Причина была не в вычислении луча, а в том, ЧТО им проверяли: вместо
// габаритов меша брался куб со стороной радиуса ограничивающей СФЕРЫ. Поэтому
// здесь проверяется не «луч куда-то попал», а именно те случаи, в которых сфера
// и коробка расходятся: угол куба, промах рядом с гранью и большой пол, внутри
// сферы которого лежит вся сцена.
#include "TestFramework.h"

#include "sage/render/MeshRaycast.h"

#include <cmath>
#include <vector>

using sage::render::RayAabb;
using sage::render::RayGeometry;
using sage::render::RayHit;

namespace {

// Куб от -0.5 до 0.5 — тот самый примитив, которым в редакторе застроена сцена.
void BuildCube(std::vector<Vertex>& v, std::vector<unsigned int>& i) {
    const glm::vec3 c[8] = {
        {-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
        {-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},  {0.5f, 0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f}};
    v.clear();
    for (const glm::vec3& p : c) {
        Vertex vert;
        vert.Position = p;
        v.push_back(vert);
    }
    const unsigned int faces[36] = {0, 1, 2, 0, 2, 3, 5, 4, 7, 5, 7, 6, 4, 0, 3, 4, 3, 7,
                                    1, 5, 6, 1, 6, 2, 3, 2, 6, 3, 6, 7, 4, 5, 1, 4, 1, 0};
    i.assign(faces, faces + 36);
}

} // namespace

TEST(picking_aabb_basic) {
    const glm::vec3 lo(-0.5f), hi(0.5f);

    // Прямо в центр спереди.
    RayHit h = RayAabb(glm::vec3(0, 0, 5), glm::vec3(0, 0, -1), lo, hi);
    CHECK_TRUE(h.Hit);
    CHECK_NEAR(h.Distance, 4.5f, 1e-4f);

    // Мимо: луч проходит выше коробки.
    CHECK_FALSE(RayAabb(glm::vec3(0, 2, 5), glm::vec3(0, 0, -1), lo, hi).Hit);

    // Коробка ПОЗАДИ начала луча — не попадание, иначе объекты за спиной
    // выигрывали бы выбор у тех, на которые человек смотрит.
    CHECK_FALSE(RayAabb(glm::vec3(0, 0, -5), glm::vec3(0, 0, -1), lo, hi).Hit);

    // Начало внутри коробки — расстояние 0, а не отрицательное t входа.
    RayHit inside = RayAabb(glm::vec3(0, 0, 0), glm::vec3(0, 0, -1), lo, hi);
    CHECK_TRUE(inside.Hit);
    CHECK_NEAR(inside.Distance, 0.0f, 1e-6f);

    // Луч, параллельный паре плоскостей: деления на ноль быть не должно ни в
    // попадающем случае, ни в промахе.
    CHECK_TRUE(RayAabb(glm::vec3(-5, 0, 0), glm::vec3(1, 0, 0), lo, hi).Hit);
    CHECK_FALSE(RayAabb(glm::vec3(-5, 9, 0), glm::vec3(1, 0, 0), lo, hi).Hit);
}

TEST(picking_sphere_bounds_would_overshoot) {
    // Ровно тот промах, который засчитывался как попадание. Куб ±0.5; радиус его
    // ограничивающей сферы — 0.866, и старый пикинг проверял коробку ±0.866.
    // Луч на высоте 0.7 проходит НАД кубом, но внутри той раздутой коробки.
    std::vector<Vertex> v;
    std::vector<unsigned int> i;
    BuildCube(v, i);

    const glm::vec3 lo(-0.5f), hi(0.5f);
    const glm::vec3 o(0.0f, 0.7f, 5.0f), d(0.0f, 0.0f, -1.0f);

    CHECK_FALSE(RayGeometry(o, d, lo, hi, &v, &i).Hit);           // настоящий габарит: промах
    const float r = std::sqrt(3.0f) * 0.5f;
    CHECK_TRUE(RayAabb(o, d, glm::vec3(-r), glm::vec3(r)).Hit);   // по сфере: ложное попадание
}

TEST(picking_triangles_beat_box) {
    // Дырка в объекте: два куба, разнесённые по X, с просветом посередине.
    // Клик в просвет не должен выбирать ничего — по коробке, охватывающей оба,
    // он выбрал бы.
    std::vector<Vertex> v;
    std::vector<unsigned int> i;
    BuildCube(v, i);
    const size_t base = v.size();
    for (size_t k = 0; k < base; ++k) {
        Vertex shifted = v[k];
        shifted.Position.x += 3.0f;
        v.push_back(shifted);
    }
    const size_t idxBase = i.size();
    for (size_t k = 0; k < idxBase; ++k) i.push_back(i[k] + (unsigned int)base);

    const glm::vec3 lo(-0.5f, -0.5f, -0.5f), hi(3.5f, 0.5f, 0.5f);

    // Просвет между кубами (x = 1.5).
    RayHit gap = RayGeometry(glm::vec3(1.5f, 0, 5), glm::vec3(0, 0, -1), lo, hi, &v, &i);
    CHECK_FALSE(gap.Hit);
    CHECK_TRUE(RayAabb(glm::vec3(1.5f, 0, 5), glm::vec3(0, 0, -1), lo, hi).Hit); // коробка врёт

    // По самому кубу — попадание, и оно помечено точным.
    RayHit solid = RayGeometry(glm::vec3(0, 0, 5), glm::vec3(0, 0, -1), lo, hi, &v, &i);
    CHECK_TRUE(solid.Hit);
    CHECK_TRUE(solid.Exact);
    CHECK_NEAR(solid.Distance, 4.5f, 1e-3f);

    // Без копии геометрии тот же вызов честно деградирует до коробки: попадание
    // есть, но неточное — и вызывающий об этом знает.
    RayHit coarse = RayGeometry(glm::vec3(1.5f, 0, 5), glm::vec3(0, 0, -1), lo, hi, nullptr, nullptr);
    CHECK_TRUE(coarse.Hit);
    CHECK_FALSE(coarse.Exact);
}

TEST(picking_distance_scales_with_direction) {
    // Направление НЕ нормализуется: вызывающий переводит луч в локальные
    // координаты объекта обратной мировой матрицей, и t обязан остаться в
    // единицах мира — иначе объекты с разным масштабом сравнивать нечем.
    const glm::vec3 lo(-0.5f), hi(0.5f);
    RayHit unit = RayAabb(glm::vec3(0, 0, 5), glm::vec3(0, 0, -1), lo, hi);
    RayHit half = RayAabb(glm::vec3(0, 0, 5), glm::vec3(0, 0, -0.5f), lo, hi);
    CHECK_NEAR(unit.Distance, 4.5f, 1e-4f);
    CHECK_NEAR(half.Distance, 9.0f, 1e-4f);
}
