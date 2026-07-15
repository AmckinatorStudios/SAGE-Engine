#pragma once
#include "Chunk.h"
#include <unordered_map>
#include <memory>

// Хэш для glm::ivec3, чтобы использовать его как ключ unordered_map
struct IVec3Hash {
    size_t operator()(const glm::ivec3& v) const {
        size_t h1 = std::hash<int>()(v.x);
        size_t h2 = std::hash<int>()(v.y);
        size_t h3 = std::hash<int>()(v.z);
        return h1 ^ (h2 * 0x9e3779b9) ^ (h3 * 0x85ebca6b);
    }
};

// World хранит СЕТКУ чанков (ключ — координата чанка, а не блока) и даёт
// единый способ спросить/поставить блок по МИРОВЫМ координатам — сам
// разберётся, в каком чанке этот блок лежит. Именно через World чанки
// узнают, что творится у соседей, и не рисуют лишние грани на стыках.
class World {
public:
    static glm::ivec3 WorldToChunkCoord(int x, int y, int z) {
        auto floorDiv = [](int a, int b) { return (a >= 0) ? (a / b) : ((a - b + 1) / b); };
        return { floorDiv(x, CHUNK_SIZE), floorDiv(y, CHUNK_SIZE), floorDiv(z, CHUNK_SIZE) };
    }

    static glm::ivec3 WorldToLocal(int x, int y, int z) {
        auto mod = [](int a, int b) { int r = a % b; return r < 0 ? r + b : r; };
        return { mod(x, CHUNK_SIZE), mod(y, CHUNK_SIZE), mod(z, CHUNK_SIZE) };
    }

    Chunk* GetChunk(glm::ivec3 chunkCoord) {
        auto it = m_chunks.find(chunkCoord);
        return it != m_chunks.end() ? it->second.get() : nullptr;
    }

    Chunk& GetOrCreateChunk(glm::ivec3 chunkCoord) {
        auto it = m_chunks.find(chunkCoord);
        if (it != m_chunks.end()) return *it->second;
        auto chunk = std::make_unique<Chunk>(chunkCoord * CHUNK_SIZE);
        Chunk& ref = *chunk;
        m_chunks[chunkCoord] = std::move(chunk);
        return ref;
    }

    BlockType GetBlock(int worldX, int worldY, int worldZ) {
        Chunk* chunk = GetChunk(WorldToChunkCoord(worldX, worldY, worldZ));
        if (!chunk) return BlockType::Air;
        glm::ivec3 local = WorldToLocal(worldX, worldY, worldZ);
        return chunk->GetBlock(local.x, local.y, local.z);
    }

    void SetBlock(int worldX, int worldY, int worldZ, BlockType type) {
        Chunk& chunk = GetOrCreateChunk(WorldToChunkCoord(worldX, worldY, worldZ));
        glm::ivec3 local = WorldToLocal(worldX, worldY, worldZ);
        chunk.SetBlock(local.x, local.y, local.z, type);

        // Если блок на самой границе чанка — соседний чанк тоже должен
        // перестроить свой меш (у него могла появиться/исчезнуть видимая грань)
        if (local.x == 0) MarkDirtyIfExists({chunk.WorldPos().x / CHUNK_SIZE - 1, chunk.WorldPos().y / CHUNK_SIZE, chunk.WorldPos().z / CHUNK_SIZE});
        if (local.x == CHUNK_SIZE - 1) MarkDirtyIfExists({chunk.WorldPos().x / CHUNK_SIZE + 1, chunk.WorldPos().y / CHUNK_SIZE, chunk.WorldPos().z / CHUNK_SIZE});
        if (local.z == 0) MarkDirtyIfExists({chunk.WorldPos().x / CHUNK_SIZE, chunk.WorldPos().y / CHUNK_SIZE, chunk.WorldPos().z / CHUNK_SIZE - 1});
        if (local.z == CHUNK_SIZE - 1) MarkDirtyIfExists({chunk.WorldPos().x / CHUNK_SIZE, chunk.WorldPos().y / CHUNK_SIZE, chunk.WorldPos().z / CHUNK_SIZE + 1});
    }

    // Перестраивает меши всех чанков, помеченных как "грязные"
    void RebuildDirtyMeshes() {
        for (auto& [coord, chunk] : m_chunks) {
            if (chunk->IsDirty()) chunk->RebuildMesh(this);
        }
    }

    std::unordered_map<glm::ivec3, std::unique_ptr<Chunk>, IVec3Hash>& Chunks() { return m_chunks; }

private:
    void MarkDirtyIfExists(glm::ivec3 chunkCoord) {
        if (Chunk* c = GetChunk(chunkCoord)) c->MarkDirty();
    }

    std::unordered_map<glm::ivec3, std::unique_ptr<Chunk>, IVec3Hash> m_chunks;
};
