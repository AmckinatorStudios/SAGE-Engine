#pragma once
#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include "sage/rhi/Resources.h"

struct Vertex {
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec2 TexCoords;
};

// Хранит геометрию на GPU (через rhi::Geometry) и умеет себя отрисовать.
// О графическом API ничего не знает — вся работа с буферами у бэкенда.
class Mesh {
public:
    Mesh(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices);

    // GPU-ресурсом владеет единолично (unique_ptr) — копирование запрещено,
    // перемещение безопасно и разрешено.
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&&) noexcept = default;
    Mesh& operator=(Mesh&&) noexcept = default;

    void Draw() const;

    // Процедурные примитивы движка (единичный масштаб, с нормалями и UV).
    // Все вписаны в габарит ~1 вокруг начала координат, чтобы Transform.Scale
    // работал предсказуемо (куб от -0.5 до 0.5, сфера радиуса 0.5 и т.д.).
    static Mesh CreateCube();
    static Mesh CreateSphere(int rings = 24, int sectors = 32); // UV-сфера r=0.5
    static Mesh CreatePlane(int subdivisions = 1);              // 1x1 в плоскости XZ, нормаль +Y
    static Mesh CreateCylinder(int sectors = 32);               // r=0.5, высота 1 (Y), с крышками
    static Mesh CreateCone(int sectors = 32);                   // r=0.5 у основания, высота 1 (Y)

private:
    std::unique_ptr<sage::rhi::Geometry> m_geometry;
    size_t m_indexCount = 0;
};
