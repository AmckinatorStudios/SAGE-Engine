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
    // Второй UV-канал — координаты в атласе лайтмапы (глобальное освещение,
    // см. sage/gi). Заполняется бейкером; у незапечённой геометрии (0,0).
    glm::vec2 TexCoords2{0.0f, 0.0f};
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
    // Непрозрачность инстанса. Живёт здесь, а не в юниформе, ровно потому же,
    // почему здесь цвет: полупрозрачные объекты обязаны рисоваться пачкой, иначе
    // стекло, вода и листва стоят по draw call'у на штуку.
    float Alpha = 1.0f;
    // Насколько инстанс пользуется плоским отражением сцены (0 — не
    // пользуется, 1 — зеркало). Здесь, а не в юниформе, по той же причине, что
    // и всё остальное в этой структуре: зеркальный пол и матовая плитка рядом
    // обязаны уходить на видеокарту одной пачкой.
    float PlanarReflectivity = 0.0f;
};

// Хранит геометрию на GPU (через rhi::Geometry) и умеет себя отрисовать.
// О графическом API ничего не знает — вся работа с буферами у бэкенда.
class Mesh {
public:
    // keepCpuData — сохранить копию вершин и индексов в оперативной памяти.
    // По умолчанию НЕТ: у обычного меша эти данные после заливки в GPU никому
    // не нужны, и держать их значило бы удваивать расход памяти на всю сцену.
    // Нужны они ровно одному потребителю — построению уровней детализации
    // (см. sage/render/MeshSimplify.h), поэтому и включаются точечно.
    Mesh(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices,
         bool keepCpuData = false);

    // Копия геометрии в оперативной памяти — nullptr, если её не просили.
    const std::vector<Vertex>* CpuVertices() const {
        return m_cpuVertices.empty() ? nullptr : &m_cpuVertices;
    }
    const std::vector<unsigned int>* CpuIndices() const {
        return m_cpuIndices.empty() ? nullptr : &m_cpuIndices;
    }
    size_t IndexCount() const { return m_indexCount; }
    size_t TriangleCount() const { return m_indexCount / 3; }

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
    // keepCpuData — см. конструктор: нужен тем, кому из этого меша ещё строить
    // уровни детализации.
    static Mesh CreateCube(bool keepCpuData = false);
    static Mesh CreateSphere(int rings = 24, int sectors = 32, bool keepCpuData = false); // UV-сфера r=0.5
    static Mesh CreatePlane(int subdivisions = 1, bool keepCpuData = false);              // 1x1 в плоскости XZ, нормаль +Y
    static Mesh CreateCylinder(int sectors = 32, bool keepCpuData = false);               // r=0.5, высота 1 (Y), с крышками
    static Mesh CreateCone(int sectors = 32, bool keepCpuData = false);                   // r=0.5 у основания, высота 1 (Y)

private:
    std::unique_ptr<sage::rhi::Geometry> m_geometry;
    std::vector<Vertex> m_cpuVertices;        // пусто, если копию не просили
    std::vector<unsigned int> m_cpuIndices;
    size_t m_indexCount = 0;
    glm::vec3 m_boundsCenter{0.0f};
    float m_boundsRadius = 0.0f;
};
