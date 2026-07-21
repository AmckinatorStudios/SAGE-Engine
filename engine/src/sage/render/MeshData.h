#pragma once
#include <vector>
#include "sage/render/Mesh.h"

// ---------------------------------------------------------------------------
// MeshData — CPU-представление геометрии (вершины + индексы) БЕЗ GPU-ресурсов
// и без GL-контекста. Нужно всем, кто работает с геометрией вне рендера:
// бейкер глобального освещения (sage/gi) трассирует лучи по этим треугольникам
// и пишет лайтмап-UV прямо в вершины, юнит-тесты гоняют генераторы headless.
//
// Генераторы Build* выдают ТУ ЖЕ геометрию, что и Mesh::Create* (Create*
// реализованы через них) — бейкер и рендер гарантированно видят одинаковые
// треугольники.
// ---------------------------------------------------------------------------
namespace sage::render {

struct MeshData {
    std::vector<Vertex> Vertices;
    std::vector<unsigned int> Indices;

    bool Empty() const { return Vertices.empty() || Indices.empty(); }
};

// Процедурные примитивы движка (параметры по умолчанию — как у Mesh::Create*,
// то есть как у ResourceManager::GetPrimitive).
MeshData BuildCube();
MeshData BuildSphere(int rings = 24, int sectors = 32);
MeshData BuildPlane(int subdivisions = 1);
MeshData BuildCylinder(int sectors = 32);
MeshData BuildCone(int sectors = 32);

} // namespace sage::render
