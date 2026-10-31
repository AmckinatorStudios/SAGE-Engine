#include "World.h"

#include <algorithm>
#include <cmath>

namespace voxel {
namespace {

// Таблица блоков. LightCost: 0 — глухой; 1 — воздух и стекло; листва и вода
// гасят свет сильнее, поэтому под деревом и на дне озера темнее.
const BlockInfo kBlocks[BlockCount] = {
    {"Воздух", DrawKind::None, 0, 1, 0, 0, 0, false},
    {"Дёрн", DrawKind::Opaque, 0, 0, TileTurfTop, TileSoil, TileTurfSide, true},
    {"Земля", DrawKind::Opaque, 0, 0, TileSoil, TileSoil, TileSoil, true},
    {"Гранит", DrawKind::Opaque, 0, 0, TileGranite, TileGranite, TileGranite, true},
    {"Песок", DrawKind::Opaque, 0, 0, TileDune, TileDune, TileDune, true},
    {"Вода", DrawKind::Liquid, 0, 3, TileWater, TileWater, TileWater, false},
    {"Ствол", DrawKind::Opaque, 0, 0, TileTrunkTop, TileTrunkTop, TileTrunkSide, true},
    {"Листва", DrawKind::Cutout, 0, 2, TileFoliage, TileFoliage, TileFoliage, true},
    {"Доски", DrawKind::Opaque, 0, 0, TileBoards, TileBoards, TileBoards, true},
    {"Светокамень", DrawKind::Opaque, 15, 0, TileLumen, TileLumen, TileLumen, true},
    {"Снег", DrawKind::Opaque, 0, 0, TileFrost, TileSoil, TileFrostSide, true},
    {"Кирпич", DrawKind::Opaque, 0, 0, TileClay, TileClay, TileClay, true},
    {"Стекло", DrawKind::Glass, 0, 1, TileCrystal, TileCrystal, TileCrystal, true},
    {"Базальт", DrawKind::Opaque, 0, 0, TileBasalt, TileBasalt, TileBasalt, true},
    {"Мшистый камень", DrawKind::Opaque, 0, 0, TileMoss, TileMoss, TileMoss, true},
    {"Тлеющий блок", DrawKind::Opaque, 10, 0, TileEmber, TileEmber, TileEmber, true},
};

uint32_t Hash(int x, int y, int z, uint32_t seed) {
    uint32_t h = seed ^ 0x9E3779B9u;
    h ^= (uint32_t)x * 0x85EBCA6Bu;
    h = (h << 13) | (h >> 19);
    h ^= (uint32_t)y * 0xC2B2AE35u;
    h = (h << 11) | (h >> 21);
    h ^= (uint32_t)z * 0x27D4EB2Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

float Rand01(int x, int y, int z, uint32_t seed) { return (Hash(x, y, z, seed) & 0xFFFFFF) / 16777215.0f; }

float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }

// Сглаженный шум значений: дёшево и без таблиц, рельефу больше не нужно.
float Noise2(float x, float z, uint32_t seed) {
    const int xi = (int)std::floor(x), zi = (int)std::floor(z);
    const float fx = Smooth(x - xi), fz = Smooth(z - zi);
    const float a = Rand01(xi, 0, zi, seed), b = Rand01(xi + 1, 0, zi, seed);
    const float c = Rand01(xi, 0, zi + 1, seed), d = Rand01(xi + 1, 0, zi + 1, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fz;
}

float Noise3(float x, float y, float z, uint32_t seed) {
    const int xi = (int)std::floor(x), yi = (int)std::floor(y), zi = (int)std::floor(z);
    const float fx = Smooth(x - xi), fy = Smooth(y - yi), fz = Smooth(z - zi);
    float v[2][2][2];
    for (int dx = 0; dx < 2; ++dx)
        for (int dy = 0; dy < 2; ++dy)
            for (int dz = 0; dz < 2; ++dz) v[dx][dy][dz] = Rand01(xi + dx, yi + dy, zi + dz, seed);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float x00 = lerp(v[0][0][0], v[1][0][0], fx), x10 = lerp(v[0][1][0], v[1][1][0], fx);
    const float x01 = lerp(v[0][0][1], v[1][0][1], fx), x11 = lerp(v[0][1][1], v[1][1][1], fx);
    return lerp(lerp(x00, x10, fy), lerp(x01, x11, fy), fz);
}

float Fbm2(float x, float z, uint32_t seed, int octaves) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += Noise2(x, z, seed + (uint32_t)i * 101u) * amp;
        norm += amp;
        amp *= 0.5f;
        x *= 2.03f;
        z *= 2.03f;
    }
    return sum / norm;
}

const int kDirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

} // namespace

const BlockInfo& Info(uint8_t block) { return kBlocks[block < BlockCount ? block : 0]; }

World::World(int chunksX, int chunksZ, uint32_t seed) : m_cx(chunksX), m_cz(chunksZ) {
    for (int cz = 0; cz < m_cz; ++cz)
        for (int cx = 0; cx < m_cx; ++cx) {
            auto c = std::make_unique<Chunk>();
            c->CX = cx;
            c->CZ = cz;
            m_chunks.push_back(std::move(c));
        }
    Generate(seed);
    ComputeAllLight();
}

Chunk* World::ChunkAt(int cx, int cz) {
    if (cx < 0 || cz < 0 || cx >= m_cx || cz >= m_cz) return nullptr;
    return m_chunks[(size_t)(cz * m_cx + cx)].get();
}

const Chunk* World::ChunkAt(int cx, int cz) const {
    if (cx < 0 || cz < 0 || cx >= m_cx || cz >= m_cz) return nullptr;
    return m_chunks[(size_t)(cz * m_cx + cx)].get();
}

uint8_t World::Get(int x, int y, int z) const {
    if (y < 0) return Granite; // под миром — твердь: грань дна не рисуется
    if (y >= kWorldHeight) return Air;
    const Chunk* c = ChunkAt(x >> 4, z >> 4);
    if (!c || x < 0 || z < 0) return Air;
    return c->Blocks[Chunk::Index(x & 15, y, z & 15)];
}

uint8_t World::SkyLight(int x, int y, int z) const {
    if (y >= kWorldHeight) return 15;
    if (y < 0) return 0;
    const Chunk* c = ChunkAt(x >> 4, z >> 4);
    if (!c || x < 0 || z < 0) return 15; // за краем мира — открытое небо
    return c->Light[Chunk::Index(x & 15, y, z & 15)] >> 4;
}

uint8_t World::BlockLight(int x, int y, int z) const {
    if (y < 0 || y >= kWorldHeight) return 0;
    const Chunk* c = ChunkAt(x >> 4, z >> 4);
    if (!c || x < 0 || z < 0) return 0;
    return c->Light[Chunk::Index(x & 15, y, z & 15)] & 15;
}

void World::MarkDirty(int x, int z) {
    // Меш чанка читает свет и блоки соседей на краю — край портит и соседа.
    const int cx = x >> 4, cz = z >> 4, lx = x & 15, lz = z & 15;
    if (Chunk* c = ChunkAt(cx, cz)) c->Dirty = true;
    if (lx == 0) if (Chunk* c = ChunkAt(cx - 1, cz)) c->Dirty = true;
    if (lx == 15) if (Chunk* c = ChunkAt(cx + 1, cz)) c->Dirty = true;
    if (lz == 0) if (Chunk* c = ChunkAt(cx, cz - 1)) c->Dirty = true;
    if (lz == 15) if (Chunk* c = ChunkAt(cx, cz + 1)) c->Dirty = true;
}

void World::SetRaw(int x, int y, int z, uint8_t b) {
    Chunk* c = ChunkAt(x >> 4, z >> 4);
    if (!c || x < 0 || z < 0 || y < 0 || y >= kWorldHeight) return;
    c->Blocks[Chunk::Index(x & 15, y, z & 15)] = b;
}

void World::SetSky(int x, int y, int z, uint8_t v) {
    Chunk* c = ChunkAt(x >> 4, z >> 4);
    uint8_t& l = c->Light[Chunk::Index(x & 15, y, z & 15)];
    l = (uint8_t)((v << 4) | (l & 15));
}

void World::SetOwn(int x, int y, int z, uint8_t v) {
    Chunk* c = ChunkAt(x >> 4, z >> 4);
    uint8_t& l = c->Light[Chunk::Index(x & 15, y, z & 15)];
    l = (uint8_t)((l & 0xF0) | v);
}

void World::Generate(uint32_t seed) {
    const int sx = SizeX(), sz = SizeZ();
    for (int x = 0; x < sx; ++x) {
        for (int z = 0; z < sz; ++z) {
            // Материки — крупный шум, холмы — мелкий, горы — его степень.
            const float continent = Fbm2(x * 0.006f, z * 0.006f, seed, 3);
            const float hills = Fbm2(x * 0.03f, z * 0.03f, seed + 7u, 4);
            const float ridge = std::pow(Fbm2(x * 0.012f, z * 0.012f, seed + 13u, 3), 3.0f);
            const int h = std::clamp((int)(18.0f + continent * 22.0f + hills * 10.0f + ridge * 70.0f),
                                     4, kWorldHeight - 12);
            for (int y = 0; y <= std::max(h, kSeaLevel); ++y) {
                uint8_t b = Air;
                if (y <= h) {
                    if (y < 6 + (int)(Rand01(x, y, z, seed) * 3.0f)) b = Basalt;
                    else if (y < h - 3) b = Granite;
                    else if (y < h) b = h <= kSeaLevel + 1 ? Dune : Soil;
                    else if (h <= kSeaLevel + 1) b = Dune;
                    else if (h > 58) b = Frost;
                    else b = Turf;
                } else if (y <= kSeaLevel) {
                    b = Water;
                }
                // Пещеры: трёхмерный шум «червями», не выше двух блоков под
                // поверхностью — иначе дыры в рельефе вместо входов.
                if (b != Air && b != Water && y > 2 && y < h - 2) {
                    const float n = Noise3(x * 0.07f, y * 0.11f, z * 0.07f, seed + 31u);
                    const float m = Noise3(x * 0.05f, y * 0.08f, z * 0.05f, seed + 37u);
                    if (std::fabs(n - 0.5f) < 0.06f && std::fabs(m - 0.5f) < 0.12f) b = Air;
                }
                SetRaw(x, y, z, b);
            }
        }
    }
    // Убранство: деревья, мох и светокамни в пещерах.
    for (int x = 2; x < sx - 2; ++x) {
        for (int z = 2; z < sz - 2; ++z) {
            const int top = SurfaceHeight(x, z);
            if (top > 0 && Get(x, top, z) == Turf && Rand01(x, 1, z, seed + 5u) < 0.012f &&
                Fbm2(x * 0.02f, z * 0.02f, seed + 77u, 2) > 0.45f)
                PlantTree(x, top + 1, z, Hash(x, top, z, seed));
            for (int y = 3; y < top - 3; ++y) {
                if (Get(x, y, z) != Granite || Get(x, y + 1, z) != Air) continue;
                const float r = Rand01(x, y, z, seed + 9u);
                if (r < 0.004f) SetRaw(x, y, z, Lumen);
                else if (r < 0.006f) SetRaw(x, y, z, Ember);
                else if (r < 0.08f) SetRaw(x, y, z, Moss);
            }
        }
    }
}

void World::PlantTree(int x, int y, int z, uint32_t h) {
    const int height = 4 + (int)(h % 3u);
    for (int i = 0; i < height; ++i) SetRaw(x, y + i, z, Trunk);
    const int top = y + height;
    for (int dy = -2; dy <= 1; ++dy) {
        const int r = dy >= 0 ? 1 : 2;
        for (int dx = -r; dx <= r; ++dx)
            for (int dz = -r; dz <= r; ++dz) {
                if (std::abs(dx) == r && std::abs(dz) == r && ((h >> (dx + dz + 8)) & 1u)) continue;
                if (Get(x + dx, top + dy, z + dz) == Air) SetRaw(x + dx, top + dy, z + dz, Foliage);
            }
    }
}

int World::SurfaceHeight(int x, int z) const {
    for (int y = kWorldHeight - 1; y >= 0; --y)
        if (Info(Get(x, y, z)).Draw == DrawKind::Opaque || Get(x, y, z) == Water) return y;
    return 0;
}

void World::ComputeAllLight() {
    const int sx = SizeX(), sz = SizeZ();
    // 1) Прямое небо: столб света сверху до первого глухого блока. Листва и
    //    вода гасят его по LightCost — под кроной и на глубине темнее.
    for (int x = 0; x < sx; ++x)
        for (int z = 0; z < sz; ++z) {
            int level = 15;
            for (int y = kWorldHeight - 1; y >= 0 && level > 0; --y) {
                const uint8_t b = Get(x, y, z);
                const BlockInfo& bi = Info(b);
                if (bi.LightCost == 0) break;
                // То же правило, что у рассеяния (Spread): без потерь вниз идёт
                // только полное небо; под листвой и водой воздух тоже гасит.
                if (b != Air || level < 15) level = std::max(0, level - bi.LightCost);
                SetSky(x, y, z, (uint8_t)level);
            }
        }
    // 2) Рассеяние: в очередь — только клетки, у которых есть прозрачный
    //    сосед темнее, чем дала бы им эта клетка. Остальные освещены столбом
    //    и ничего не меняют, а класть в очередь весь воздух мира — это
    //    миллионы лишних шагов. Соседа смотрим и сбоку, и снизу: под кроной
    //    свет приходит СБОКУ, с открытого столба на той же высоте.
    std::deque<Node> sky;
    for (int x = 0; x < sx; ++x)
        for (int z = 0; z < sz; ++z)
            for (int y = 0; y < kWorldHeight; ++y) {
                const uint8_t l = SkyLight(x, y, z);
                if (l <= 1) continue;
                for (int d = 0; d < 6; ++d) {
                    if (d == 2) continue;
                    const int nx = x + kDirs[d][0], ny = y + kDirs[d][1], nz = z + kDirs[d][2];
                    if (nx < 0 || nz < 0 || ny < 0 || nx >= sx || nz >= sz) continue;
                    const BlockInfo& ni = Info(Get(nx, ny, nz));
                    if (ni.LightCost == 0) continue;
                    if (SkyLight(nx, ny, nz) + ni.LightCost < l) {
                        sky.push_back({x, y, z, l});
                        break;
                    }
                }
            }
    Spread(sky, true);

    // 3) Свой свет блоков.
    std::deque<Node> own;
    for (int x = 0; x < sx; ++x)
        for (int z = 0; z < sz; ++z)
            for (int y = 0; y < kWorldHeight; ++y) {
                const uint8_t e = Info(Get(x, y, z)).Emission;
                if (e > 0) {
                    SetOwn(x, y, z, e);
                    own.push_back({x, y, z, e});
                }
            }
    Spread(own, false);
}

void World::Spread(std::deque<Node>& queue, bool sky) {
    while (!queue.empty()) {
        const Node n = queue.front();
        queue.pop_front();
        // Узел светит, только если его уровень всё ещё тот, с которым он попал
        // в очередь. При снятии света клетка уходит в очередь заполнения как
        // «свой источник», а позже гаснет — её свет держался на снятом через
        // соседа. Без этой проверки она светила бы из очереди старым уровнем,
        // и у снятого светокамня оставалось бы призрачное свечение.
        if ((sky ? SkyLight(n.X, n.Y, n.Z) : BlockLight(n.X, n.Y, n.Z)) != n.Level) continue;
        for (int d = 0; d < 6; ++d) {
            const int x = n.X + kDirs[d][0], y = n.Y + kDirs[d][1], z = n.Z + kDirs[d][2];
            if (y < 0 || y >= kWorldHeight || x < 0 || z < 0 || x >= SizeX() || z >= SizeZ()) continue;
            const uint8_t b = Get(x, y, z);
            const BlockInfo& bi = Info(b);
            if (bi.LightCost == 0) continue;
            int level = n.Level - bi.LightCost;
            // Небо вниз по воздуху не слабеет — столб света под снятым блоком.
            if (sky && d == 3 && n.Level == 15 && b == Air) level = 15;
            if (level <= 0) continue;
            const uint8_t cur = sky ? SkyLight(x, y, z) : BlockLight(x, y, z);
            if (level <= cur) continue;
            if (sky) SetSky(x, y, z, (uint8_t)level);
            else SetOwn(x, y, z, (uint8_t)level);
            MarkDirty(x, z);
            queue.push_back({x, y, z, (uint8_t)level});
        }
    }
}

void World::Unspread(std::deque<Node>& removal, std::deque<Node>& refill, bool sky) {
    while (!removal.empty()) {
        const Node n = removal.front();
        removal.pop_front();
        for (int d = 0; d < 6; ++d) {
            const int x = n.X + kDirs[d][0], y = n.Y + kDirs[d][1], z = n.Z + kDirs[d][2];
            if (y < 0 || y >= kWorldHeight || x < 0 || z < 0 || x >= SizeX() || z >= SizeZ()) continue;
            const uint8_t cur = sky ? SkyLight(x, y, z) : BlockLight(x, y, z);
            if (cur == 0) continue;
            const bool downColumn = sky && d == 3 && n.Level == 15;
            if (cur < n.Level || downColumn) {
                // Свет держался на снятом источнике — гасим и идём дальше.
                if (sky) SetSky(x, y, z, 0);
                else SetOwn(x, y, z, 0);
                MarkDirty(x, z);
                removal.push_back({x, y, z, cur});
                // Светящийся блок гасить нельзя: его свет — свой, а не
                // пришедший от снятого. Он светит заново из очереди заполнения.
                if (const uint8_t e = Info(Get(x, y, z)).Emission; !sky && e > 0) {
                    SetOwn(x, y, z, e);
                    refill.push_back({x, y, z, e});
                }
            } else {
                // Свой источник ярче — он и зальёт освободившееся место.
                refill.push_back({x, y, z, cur});
            }
        }
    }
    // Источники своего света, оказавшиеся в погашенной зоне, светят заново.
    Spread(refill, sky);
}

bool World::Set(int x, int y, int z, uint8_t block) {
    if (x < 0 || z < 0 || x >= SizeX() || z >= SizeZ() || y < 0 || y >= kWorldHeight) return false;
    SetRaw(x, y, z, block);
    MarkDirty(x, z);

    for (int pass = 0; pass < 2; ++pass) {
        const bool sky = pass == 0;
        std::deque<Node> removal, refill;
        const uint8_t old = sky ? SkyLight(x, y, z) : BlockLight(x, y, z);
        if (old > 0) {
            if (sky) SetSky(x, y, z, 0);
            else SetOwn(x, y, z, 0);
            removal.push_back({x, y, z, old});
            Unspread(removal, refill, sky);
        }
        const BlockInfo& bi = Info(block);
        if (!sky && bi.Emission > 0) {
            SetOwn(x, y, z, bi.Emission);
            refill.push_back({x, y, z, bi.Emission});
        }
        if (bi.LightCost > 0) {
            // В освободившуюся клетку свет приходит от соседей.
            for (int d = 0; d < 6; ++d) {
                const int nx = x + kDirs[d][0], ny = y + kDirs[d][1], nz = z + kDirs[d][2];
                const uint8_t l = sky ? SkyLight(nx, ny, nz) : BlockLight(nx, ny, nz);
                if (l > 0 && ny >= 0 && ny < kWorldHeight && nx >= 0 && nz >= 0 && nx < SizeX() &&
                    nz < SizeZ())
                    refill.push_back({nx, ny, nz, l});
            }
            if (sky && y == kWorldHeight - 1) {
                SetSky(x, y, z, 15);
                refill.push_back({x, y, z, 15});
            }
        }
        Spread(refill, sky);
    }
    return true;
}

RayHit World::Raycast(glm::vec3 origin, glm::vec3 dir, float maxDist) const {
    RayHit hit;
    if (glm::length(dir) < 1e-6f) return hit;
    dir = glm::normalize(dir);
    glm::ivec3 cell((int)std::floor(origin.x), (int)std::floor(origin.y), (int)std::floor(origin.z));
    const glm::ivec3 step(dir.x > 0 ? 1 : -1, dir.y > 0 ? 1 : -1, dir.z > 0 ? 1 : -1);
    glm::vec3 tMax, tDelta;
    for (int i = 0; i < 3; ++i) {
        const float d = dir[i];
        tDelta[i] = std::fabs(d) < 1e-6f ? 1e30f : std::fabs(1.0f / d);
        const float boundary = step[i] > 0 ? (float)(cell[i] + 1) : (float)cell[i];
        tMax[i] = std::fabs(d) < 1e-6f ? 1e30f : (boundary - origin[i]) / d;
    }
    glm::ivec3 normal(0);
    float t = 0.0f;
    while (t <= maxDist) {
        const uint8_t b = Get(cell.x, cell.y, cell.z);
        if (b != Air && b != Water && cell.y >= 0) {
            hit.Hit = true;
            hit.Block = cell;
            hit.Normal = normal;
            return hit;
        }
        int axis = 0;
        if (tMax.y < tMax[axis]) axis = 1;
        if (tMax.z < tMax[axis]) axis = 2;
        t = tMax[axis];
        tMax[axis] += tDelta[axis];
        cell[axis] += step[axis];
        normal = glm::ivec3(0);
        normal[axis] = -step[axis];
    }
    return hit;
}

int World::LightMismatches() {
    std::vector<uint8_t> incremental;
    for (const auto& c : m_chunks) incremental.insert(incremental.end(), c->Light, c->Light + sizeof(c->Light));
    for (auto& c : m_chunks) std::fill(std::begin(c->Light), std::end(c->Light), (uint8_t)0);
    ComputeAllLight();
    int diff = 0;
    size_t i = 0;
    for (const auto& c : m_chunks)
        for (uint8_t l : c->Light) diff += l != incremental[i++];
    return diff;
}

int World::LitBlocks() const {
    int n = 0;
    for (const auto& c : m_chunks)
        for (uint8_t l : c->Light)
            if (l) ++n;
    return n;
}

} // namespace voxel
