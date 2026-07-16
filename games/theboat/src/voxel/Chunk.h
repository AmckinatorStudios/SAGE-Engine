#pragma once
#include "Block.h"
#include "VoxelMesh.h"
#include <array>
#include <memory>
#include <glm/glm.hpp>

class World; // forward declaration — Chunk может спросить у World блок соседнего чанка

// Размер чанка по каждой оси. 16 — стандарт индустрии (Minecraft тоже так делает):
// достаточно большой, чтобы не плодить тысячи чанков, достаточно маленький,
// чтобы перегенерация меша при изменении одного блока была мгновенной.
constexpr int CHUNK_SIZE = 16;

// Чанк — кусок вокселей 16x16x16. Хранит СЫРЫЕ ДАННЫЕ (какой блок где стоит)
// отдельно от МЕША (готовой геометрии для рендера). Меш перегенерируется
// только когда блоки реально поменялись — не каждый кадр.
class Chunk {
public:
    // worldPos — позиция чанка в мире, в блоках (например {0,0,0} или {16,0,0} для соседнего)
    explicit Chunk(glm::ivec3 worldPos) : m_worldPos(worldPos) {
        m_blocks.fill(BlockType::Air);
    }

    BlockType GetBlock(int x, int y, int z) const {
        if (!InBounds(x, y, z)) return BlockType::Air;
        return m_blocks[Index(x, y, z)];
    }

    void SetBlock(int x, int y, int z, BlockType type) {
        if (!InBounds(x, y, z)) return;
        m_blocks[Index(x, y, z)] = type;
        m_dirty = true; // помечаем, что меш нужно перестроить
    }

    bool IsDirty() const { return m_dirty; }
    void MarkDirty() { m_dirty = true; }
    glm::ivec3 WorldPos() const { return m_worldPos; }

    // Перестраивает меш из текущих данных блоков (face culling: рисуем грань,
    // только если сосед с этой стороны — воздух или за пределами чанка).
    // Если передан world — соседние блоки ЗА ГРАНИЦЕЙ чанка запрашиваются у него
    // (чтобы не рисовать лишние грани на стыках соседних чанков). Без world
    // (nullptr) всё, что за границей, считается воздухом — подходит для
    // одиночных изолированных чанков вроде корабля.
    void RebuildMesh(World* world = nullptr);

    VoxelMesh& Mesh() { return m_mesh; }
    const VoxelMesh& Mesh() const { return m_mesh; }

    static bool InBounds(int x, int y, int z) {
        return x >= 0 && x < CHUNK_SIZE && y >= 0 && y < CHUNK_SIZE && z >= 0 && z < CHUNK_SIZE;
    }

private:
    static size_t Index(int x, int y, int z) {
        return static_cast<size_t>(x) + CHUNK_SIZE * (static_cast<size_t>(y) + CHUNK_SIZE * static_cast<size_t>(z));
    }

    glm::ivec3 m_worldPos;
    std::array<BlockType, CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE> m_blocks;
    VoxelMesh m_mesh;
    bool m_dirty = true;
};
