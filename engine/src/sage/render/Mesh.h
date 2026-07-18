#pragma once
#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include "sage/rhi/Resources.h"

struct Vertex {
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec2 TexCoords;
    // Касательная для normal mapping (TBN). w — знак ориентации (handedness):
    // бинормаль в шейдере = cross(N, T.xyz) * T.w. Без него зеркальные по UV
    // грани получали бы инвертированный рельеф нормал-карты.
    glm::vec4 Tangent{1.0f, 0.0f, 0.0f, 1.0f};
};

// Per-instance данные для инстансной отрисовки (батчинг): модельная матрица +
// базовый цвет + PBR-параметры (metallic/roughness). Layout совпадает с
// instance-атрибутами геометрии Mesh (loc 4..7 — строки mat4, loc 8 — цвет,
// loc 9 — metallic, loc 10 — roughness).
struct MeshInstance {
    glm::mat4 Model{1.0f};
    glm::vec3 Color{1.0f};
    float Metallic = 0.0f;
    float Roughness = 0.5f;
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

    // --- Инстансный батчинг (много одинаковых мешей за один вызов) ---
    // Заливает per-instance поток (MeshInstance[]) и рисует count инстансов
    // одним draw call'ом. Данные перезаливаются каждый кадр (STREAM).
    void SetInstances(const MeshInstance* data, size_t count) const;
    void DrawInstances(size_t count) const;

    // --- Ограничивающая сфера в ЛОКАЛЬНЫХ координатах (для отсечения по
    //     фрустуму). Мировая сфера = центр*model, радиус*max|scale|. ---
    glm::vec3 BoundsCenter() const { return m_boundsCenter; }
    float BoundsRadius() const { return m_boundsRadius; }

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
    glm::vec3 m_boundsCenter{0.0f};
    float m_boundsRadius = 0.0f;
};
