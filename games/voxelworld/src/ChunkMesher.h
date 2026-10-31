#pragma once
#include <cstdint>
#include <vector>

#include "World.h"

// ---------------------------------------------------------------------------
// Меш чанка: только видимые грани, сглаженный свет и затенение углов (AO).
//
// ВЕРШИНА — ДВА 32-БИТНЫХ ЦЕЛЫХ, а не дюжина float. Чанк из тысяч граней в
// float-формате занимал бы мегабайты; упаковка даёт 8 байт на вершину, и
// разбирает её вершинный шейдер (chunk.vert):
//   слово 0: x 5 | y 7 | z 5 | грань 3 | угол 2 | AO 2 | флаги 2
//   слово 1: плитка 8 | небо×4 6 | свой×4 6
// Свет — среднее по четырём клеткам у угла (×4, чтобы не терять дробную
// часть): так тень от уступа ложится плавно, а не ступенькой блока.
// ---------------------------------------------------------------------------
namespace voxel {

struct ChunkMeshData {
    std::vector<uint32_t> Vertices; // по два слова на вершину
    std::vector<uint32_t> Indices;
    int Faces() const { return (int)Indices.size() / 6; }
};

struct ChunkMeshes {
    ChunkMeshData Opaque;      // глухие блоки и листва (вырез по альфе)
    ChunkMeshData Translucent; // вода и стекло — смешиванием, по удалённости
};

// Флаги вершины.
constexpr uint32_t kFlagLiquidTop = 1u; // верх воды опущен и колышется
constexpr uint32_t kFlagSway = 2u;      // листва покачивается

ChunkMeshes BuildChunkMesh(const World& world, int cx, int cz);

// Разбор слова вершины (тот же, что в шейдере) — для тестов.
struct UnpackedVertex {
    int X, Y, Z, Face, Corner, AO, Flags, Tile;
    float Sky, Own; // 0..15
};
UnpackedVertex Unpack(uint32_t w0, uint32_t w1);

} // namespace voxel
