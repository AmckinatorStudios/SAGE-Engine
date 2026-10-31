#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "Blocks.h"

// ---------------------------------------------------------------------------
// Мир: чанки 16×16 по горизонтали на всю высоту, блоки и СВЕТ БЛОКОВ.
//
// Свет хранится в мире, а не считается в шейдере: у каждого блока два уровня
// 0..15 — небесный (сверху, гаснет в пещерах) и свой (от светокамня). Шейдер
// получает их в вершинах и переводит в цвет картой света (VoxelPipeline). Так
// смена дня и ночи — это перекраска карты 16×16, а не пересчёт мира.
//
// Распространение — поиск в ширину с потерей уровня на шаг. Правка блока
// гасит свет вокруг двумя очередями (снятие и повторное заполнение), а не
// пересчитывает мир целиком: иначе каждый поставленный блок стоил бы секунд.
// ---------------------------------------------------------------------------
namespace voxel {

constexpr int kChunkSize = 16;
constexpr int kWorldHeight = 96;
constexpr int kSeaLevel = 30;

struct Chunk {
    int CX = 0, CZ = 0;
    uint8_t Blocks[kChunkSize * kChunkSize * kWorldHeight] = {};
    uint8_t Light[kChunkSize * kChunkSize * kWorldHeight] = {}; // небо << 4 | свой
    bool Dirty = true; // нужен новый меш

    static int Index(int x, int y, int z) { return (y * kChunkSize + z) * kChunkSize + x; }
};

struct RayHit {
    bool Hit = false;
    glm::ivec3 Block{0};
    glm::ivec3 Normal{0}; // грань, в которую попали (куда ставить новый блок)
};

class World {
public:
    // Мир chunksX × chunksZ чанков, рельеф из seed.
    World(int chunksX, int chunksZ, uint32_t seed);

    int ChunksX() const { return m_cx; }
    int ChunksZ() const { return m_cz; }
    int SizeX() const { return m_cx * kChunkSize; }
    int SizeZ() const { return m_cz * kChunkSize; }

    uint8_t Get(int x, int y, int z) const;
    // Свет блока: небо (0..15) и свой (0..15). Вне мира сверху — полное небо.
    uint8_t SkyLight(int x, int y, int z) const;
    uint8_t BlockLight(int x, int y, int z) const;

    // Поставить блок и пересчитать свет вокруг. false — вне мира.
    bool Set(int x, int y, int z, uint8_t block);

    Chunk* ChunkAt(int cx, int cz);
    const Chunk* ChunkAt(int cx, int cz) const;
    std::vector<std::unique_ptr<Chunk>>& Chunks() { return m_chunks; }

    // Луч по сетке (DDA): первый твёрдый для луча блок не дальше maxDist.
    RayHit Raycast(glm::vec3 origin, glm::vec3 dir, float maxDist) const;

    // Высота поверхности (первый непрозрачный блок сверху) — для появления.
    int SurfaceHeight(int x, int z) const;

    // Свет посчитан для всего мира (после генерации) — для тестов и отладки.
    int LitBlocks() const;
    // Сколько клеток света разошлось с полным пересчётом (после пересчёта свет
    // верный). Ноль — правки блоков считали свет так же, как генерация.
    int LightMismatches();

private:
    void Generate(uint32_t seed);
    void PlantTree(int x, int y, int z, uint32_t h);
    void ComputeAllLight();

    void SetRaw(int x, int y, int z, uint8_t b);
    void SetSky(int x, int y, int z, uint8_t v);
    void SetOwn(int x, int y, int z, uint8_t v);
    void MarkDirty(int x, int z);

    struct Node {
        int X, Y, Z;
        uint8_t Level;
    };
    void Spread(std::deque<Node>& queue, bool sky);
    void Unspread(std::deque<Node>& removal, std::deque<Node>& refill, bool sky);

    int m_cx, m_cz;
    std::vector<std::unique_ptr<Chunk>> m_chunks;
};

} // namespace voxel
