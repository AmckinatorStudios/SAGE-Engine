#pragma once
#include <vector>
#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// BVH — иерархия ограничивающих объёмов по треугольникам статичной геометрии.
// Ускоряющая структура path tracer'а бейкера GI (sage/gi/LightBaker): без неё
// каждый луч перебирал бы все треугольники сцены. Построение — рекурсивный
// median-split по самой длинной оси; обход — итеративный (явный стек, без
// рекурсии в горячем цикле). Чистый CPU, юнит-тестируется без GL.
// ---------------------------------------------------------------------------
namespace sage::gi {

struct Tri {
    glm::vec3 A, B, C;
    glm::vec3 N;        // геометрическая нормаль (нормированная)
    glm::vec3 Albedo;   // цвет поверхности для переноса света (bounce)
};

struct RayHit {
    float T = 0.0f;
    glm::vec3 Pos{0.0f};
    glm::vec3 N{0.0f};      // нормаль грани, развёрнутая ПРОТИВ луча не считается — как есть
    glm::vec3 Albedo{0.0f};
    bool BackFace = false;  // луч пришёл с изнанки (dot(dir, N) > 0)
};

class BVH {
public:
    // Строит дерево по копии треугольников. Пустой список допустим —
    // Raycast/Occluded тогда всегда «мимо».
    void Build(std::vector<Tri> tris);

    bool Empty() const { return m_tris.empty(); }
    size_t TriangleCount() const { return m_tris.size(); }

    // Ближайшее пересечение луча в (tMin, tMax). dir должен быть нормирован.
    bool Raycast(const glm::vec3& orig, const glm::vec3& dir, float tMax, RayHit& out) const;

    // Есть ли ЛЮБОЕ пересечение в (tMin, tMax) — теневые лучи (быстрее Raycast).
    bool Occluded(const glm::vec3& orig, const glm::vec3& dir, float tMax) const;

private:
    struct Node {
        glm::vec3 Min{0.0f}, Max{0.0f};
        int Left = -1;   // индекс левого ребёнка (-1 — лист)
        int Right = -1;
        int First = 0;   // лист: диапазон в m_order
        int Count = 0;
    };

    int BuildNode(int first, int count, std::vector<glm::vec3>& centroids, int depth);

    std::vector<Tri> m_tris;
    std::vector<int> m_order;   // индексы треугольников, переставляемые сплитами
    std::vector<Node> m_nodes;
};

} // namespace sage::gi
