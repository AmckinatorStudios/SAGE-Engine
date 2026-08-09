#pragma once
#include <string>
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

// Submesh (разметка геометрии по материалам) объявлен в Mesh.h — там же, где
// Vertex: его держит и CPU-представление, и GPU-меш, а Mesh.h включается
// раньше.

struct MeshData {
    std::vector<Vertex> Vertices;
    std::vector<unsigned int> Indices;
    // Разметка по материалам. ПУСТО — законное состояние: вся геометрия
    // считается одним подмешем. Так процедурные примитивы и старые форматы
    // ничего про подмеши не знают и знать не обязаны.
    std::vector<Submesh> Submeshes;

    bool Empty() const { return Vertices.empty() || Indices.empty(); }

    // Разметка, годная для отрисовки: если её нет — один подмеш на всё.
    // Единственное место, где записано это правило: разложить его по
    // потребителям значило бы получить столько трактовок пустого списка,
    // сколько потребителей.
    std::vector<Submesh> SubmeshesOrWhole() const {
        if (!Submeshes.empty()) return Submeshes;
        return {Submesh{{}, 0u, (unsigned int)Indices.size(), -1}};
    }
};

// Процедурные примитивы движка (параметры по умолчанию — как у Mesh::Create*,
// то есть как у ResourceManager::GetPrimitive).
MeshData BuildCube();
MeshData BuildSphere(int rings = 24, int sectors = 32);
MeshData BuildPlane(int subdivisions = 1);
MeshData BuildCylinder(int sectors = 32);
MeshData BuildCone(int sectors = 32);

} // namespace sage::render
