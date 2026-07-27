// Уровни детализации: упрощение меша (чистая математика, без GL) и выбор
// уровня по проекционному размеру.
#include "TestFramework.h"

#include "sage/render/LodSelect.h"
#include "sage/render/MeshSimplify.h"

#include <cmath>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

namespace {

// Сфера-икосаэдр не нужна: годится любая плотная замкнутая поверхность.
// Берём UV-сферу — у неё есть и полюса (сгущение вершин), и ровные пояса,
// то есть оба случая, на которых кластеризация может повести себя по-разному.
void BuildSphere(std::vector<Vertex>& verts, std::vector<unsigned int>& idx, int rings,
                 int sectors) {
    verts.clear();
    idx.clear();
    for (int r = 0; r <= rings; ++r) {
        const float v = (float)r / (float)rings;
        const float phi = v * 3.14159265f;
        for (int s = 0; s <= sectors; ++s) {
            const float u = (float)s / (float)sectors;
            const float theta = u * 2.0f * 3.14159265f;
            Vertex vert;
            vert.Position = {std::sin(phi) * std::cos(theta) * 0.5f, std::cos(phi) * 0.5f,
                             std::sin(phi) * std::sin(theta) * 0.5f};
            vert.Normal = glm::normalize(vert.Position);
            vert.TexCoords = {u, v};
            verts.push_back(vert);
        }
    }
    const int stride = sectors + 1;
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < sectors; ++s) {
            const unsigned int a = (unsigned int)(r * stride + s);
            const unsigned int b = (unsigned int)((r + 1) * stride + s);
            idx.insert(idx.end(), {a, b, a + 1});
            idx.insert(idx.end(), {a + 1, b, b + 1});
        }
    }
}

float MaxRadius(const std::vector<Vertex>& verts) {
    float m = 0.0f;
    for (const Vertex& v : verts) m = std::max(m, glm::length(v.Position));
    return m;
}

} // namespace

TEST(simplify_reduces_triangles_and_keeps_shape) {
    std::vector<Vertex> verts;
    std::vector<unsigned int> idx;
    BuildSphere(verts, idx, 32, 48);
    const int sourceTris = (int)(idx.size() / 3);
    CHECK_TRUE(sourceTris > 2000);

    const sage::render::SimplifyResult r = sage::render::SimplifyToRatio(verts, idx, 0.25f);

    // Упрощение обязано именно УПРОЩАТЬ, а не возвращать исходник: это первое,
    // что ломается при неверной настройке сетки кластеров.
    CHECK_TRUE(r.Triangles < sourceTris);
    CHECK_TRUE(r.Triangles > 0);
    // Цель 25% — сетка кластеров рычаг грубый, но промах втрое означал бы, что
    // подбор сетки не работает вовсе.
    CHECK_TRUE(r.Ratio > 0.08f && r.Ratio < 0.75f);

    // Форма должна сохраниться: усреднение внутри ячейки чуть стягивает
    // поверхность внутрь, но не может ни раздуть модель, ни схлопнуть её.
    const float srcRadius = MaxRadius(verts);
    const float lodRadius = MaxRadius(r.Vertices);
    CHECK_TRUE(lodRadius < srcRadius * 1.05f);
    CHECK_TRUE(lodRadius > srcRadius * 0.7f);

    // Мусора в буфере остаться не должно: неиспользуемые вершины выкидываются,
    // иначе экономия была бы только по треугольникам, но не по памяти.
    CHECK_TRUE(r.Vertices.size() < verts.size());
    for (unsigned int i : r.Indices) CHECK_TRUE(i < r.Vertices.size());

    // Нулевых нормалей быть не может — в шейдере они дают чёрные пятна.
    for (const Vertex& v : r.Vertices) CHECK_TRUE(glm::length(v.Normal) > 0.5f);
}

TEST(simplify_is_monotonic_in_grid_size) {
    std::vector<Vertex> verts;
    std::vector<unsigned int> idx;
    BuildSphere(verts, idx, 24, 36);

    // Мельче сетка — больше треугольников. Без этого свойства двоичный поиск по
    // размеру сетки в SimplifyToRatio не имел бы смысла.
    const int coarse = sage::render::SimplifyByClustering(verts, idx, 4).Triangles;
    const int medium = sage::render::SimplifyByClustering(verts, idx, 10).Triangles;
    const int fine = sage::render::SimplifyByClustering(verts, idx, 24).Triangles;
    CHECK_TRUE(coarse < medium);
    CHECK_TRUE(medium < fine);
}

TEST(simplify_survives_degenerate_input) {
    // Пустой вход, один треугольник, вырожденный габарит (все вершины в точке) —
    // всё это встречается в реальных файлах, и падать на них нельзя.
    std::vector<Vertex> empty;
    std::vector<unsigned int> noIdx;
    CHECK_TRUE(sage::render::SimplifyToRatio(empty, noIdx, 0.5f).Triangles == 0);

    std::vector<Vertex> one(3);
    one[0].Position = {0.0f, 0.0f, 0.0f};
    one[1].Position = {1.0f, 0.0f, 0.0f};
    one[2].Position = {0.0f, 1.0f, 0.0f};
    for (Vertex& v : one) v.Normal = {0.0f, 0.0f, 1.0f};
    std::vector<unsigned int> tri = {0, 1, 2};
    CHECK_TRUE(sage::render::SimplifyToRatio(one, tri, 0.5f).Triangles <= 1);

    std::vector<Vertex> same(3);
    for (Vertex& v : same) { v.Position = {1.0f, 1.0f, 1.0f}; v.Normal = {0.0f, 1.0f, 0.0f}; }
    const sage::render::SimplifyResult r = sage::render::SimplifyToRatio(same, tri, 0.5f);
    CHECK_TRUE(r.Triangles <= 1); // не упало и не выдумало геометрию
}

TEST(lod_picks_level_by_projected_size) {
    sage::render::LodLevels levels;
    levels.Add(/*screenHeight=*/0.30f); // подробный
    levels.Add(/*screenHeight=*/0.10f);
    levels.Add(/*screenHeight=*/0.02f); // грубый
    levels.CullBelow = 0.005f;

    // Крупный объект в кадре — нулевой уровень.
    CHECK_TRUE(levels.Select(0.80f) == 0);
    CHECK_TRUE(levels.Select(0.31f) == 0);
    // Ровно на пороге берётся ещё подробный уровень: иначе объект «прыгал» бы
    // между уровнями от дрожания последнего бита.
    CHECK_TRUE(levels.Select(0.30f) == 0);
    CHECK_TRUE(levels.Select(0.29f) == 1);
    CHECK_TRUE(levels.Select(0.05f) == 2);
    // Совсем мелкий — не рисуется вовсе.
    CHECK_TRUE(levels.Select(0.001f) < 0);

    // Пустой набор уровней не должен ничего отсекать: это означает «LOD у
    // объекта нет», а не «объект невидим».
    sage::render::LodLevels none;
    CHECK_TRUE(none.Select(0.0001f) == 0);
}

TEST(projected_size_falls_with_distance) {
    // Проекционная высота сферы в долях экрана. Метрика, по которой выбирается
    // уровень: она учитывает и дальность, и угол обзора, в отличие от голой
    // дистанции — большой объект на сотне метров всё ещё крупный на экране.
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);

    const float near5 = sage::render::ProjectedScreenHeight(
        proj * glm::lookAt(glm::vec3(0, 0, 5), glm::vec3(0), glm::vec3(0, 1, 0)),
        glm::vec3(0.0f), 1.0f);
    const float far50 = sage::render::ProjectedScreenHeight(
        proj * glm::lookAt(glm::vec3(0, 0, 50), glm::vec3(0), glm::vec3(0, 1, 0)),
        glm::vec3(0.0f), 1.0f);

    CHECK_TRUE(near5 > far50);
    // Вдесятеро дальше — примерно вдесятеро мельче. Проверяем именно порядок:
    // точное совпадение испортила бы перспективная поправка у самой камеры.
    CHECK_TRUE(far50 * 8.0f < near5 && far50 * 12.0f > near5);

    // Больший радиус при той же дальности — крупнее на экране.
    const glm::mat4 vp = proj * glm::lookAt(glm::vec3(0, 0, 20), glm::vec3(0), glm::vec3(0, 1, 0));
    CHECK_TRUE(sage::render::ProjectedScreenHeight(vp, glm::vec3(0.0f), 2.0f) >
               sage::render::ProjectedScreenHeight(vp, glm::vec3(0.0f), 1.0f));
}
