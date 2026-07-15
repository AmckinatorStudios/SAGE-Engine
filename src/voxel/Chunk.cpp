#include "Chunk.h"
#include "World.h"

// 6 направлений граней куба: смещение соседа + нормаль + 4 вершины грани
// (в локальных координатах блока 0..1, порядок против часовой стрелки при взгляде снаружи)
// + локальные UV (0..1) для каждой вершины, в том же порядке
namespace {
    struct FaceDef {
        glm::ivec3 neighborOffset;
        glm::vec3 normal;
        glm::vec3 verts[4];
    };

    const glm::vec2 kQuadUV[4] = { {0,0}, {1,0}, {1,1}, {0,1} };

    const FaceDef kFaces[6] = {
        // +X (право)
        { {1,0,0}, {1,0,0}, { {1,0,0},{1,1,0},{1,1,1},{1,0,1} } },
        // -X (лево)
        { {-1,0,0}, {-1,0,0}, { {0,0,1},{0,1,1},{0,1,0},{0,0,0} } },
        // +Y (верх)
        { {0,1,0}, {0,1,0}, { {0,1,0},{0,1,1},{1,1,1},{1,1,0} } },
        // -Y (низ)
        { {0,-1,0}, {0,-1,0}, { {0,0,1},{0,0,0},{1,0,0},{1,0,1} } },
        // +Z (перёд)
        { {0,0,1}, {0,0,1}, { {1,0,1},{1,1,1},{0,1,1},{0,0,1} } },
        // -Z (зад)
        { {0,0,-1}, {0,0,-1}, { {0,0,0},{0,1,0},{1,1,0},{1,0,0} } },
    };
}

void Chunk::RebuildMesh(World* world) {
    std::vector<VoxelVertex> vertices;
    std::vector<unsigned int> indices;
    vertices.reserve(1024);
    indices.reserve(1536);

    // Сплошной ли блок в ЛОКАЛЬНЫХ координатах чанка (могут выходить за
    // границы [0..CHUNK_SIZE) на любое число блоков) — с fallback на World
    // для соседних чанков, как и обычный neighbour-запрос ниже.
    auto isSolidAt = [&](int lx, int ly, int lz) -> bool {
        if (InBounds(lx, ly, lz)) return IsSolid(GetBlock(lx, ly, lz));
        if (world) {
            glm::ivec3 wp = m_worldPos + glm::ivec3(lx, ly, lz);
            return IsSolid(world->GetBlock(wp.x, wp.y, wp.z));
        }
        return false;
    };

    // Классический алгоритм vertex AO (см. 0fps.net "Ambient occlusion for
    // minecraft-like worlds"): для каждой вершины грани смотрим на 2 блока
    // "по бокам" от неё в плоскости грани + 1 диагональный "угловой" блок.
    // Если оба боковых блока сплошные — угол считается максимально тёмным
    // (даже если диагональный блок пуст — иначе сквозь щель протекал бы свет).
    auto vertexAO = [&](const glm::ivec3& farCell, const glm::ivec3& t1, const glm::ivec3& t2) -> float {
        bool side1 = isSolidAt(farCell.x + t1.x, farCell.y + t1.y, farCell.z + t1.z);
        bool side2 = isSolidAt(farCell.x + t2.x, farCell.y + t2.y, farCell.z + t2.z);
        bool corner = isSolidAt(farCell.x + t1.x + t2.x, farCell.y + t1.y + t2.y, farCell.z + t1.z + t2.z);

        int occlusion = (side1 && side2) ? 3 : (int)side1 + (int)side2 + (int)corner;
        static const float kBrightness[4] = { 1.0f, 0.8f, 0.6f, 0.45f };
        return kBrightness[occlusion];
    };

    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int y = 0; y < CHUNK_SIZE; ++y) {
            for (int x = 0; x < CHUNK_SIZE; ++x) {
                BlockType block = GetBlock(x, y, z);
                if (!IsSolid(block)) continue;

                glm::vec3 color = GetBlockColor(block);

                // Атлас — сетка 4x2 тайла. Переводим индекс тайла блока в
                // прямоугольник UV-координат (u0,v0)-(u1,v1) внутри атласа.
                constexpr int ATLAS_COLS = 4, ATLAS_ROWS = 3;
                int atlasIndex = GetBlockAtlasIndex(block);
                glm::vec2 tileOrigin(
                    (atlasIndex % ATLAS_COLS) / float(ATLAS_COLS),
                    (atlasIndex / ATLAS_COLS) / float(ATLAS_ROWS)
                );
                glm::vec2 tileSize(1.0f / ATLAS_COLS, 1.0f / ATLAS_ROWS);

                for (const FaceDef& face : kFaces) {
                    int nx = x + face.neighborOffset.x;
                    int ny = y + face.neighborOffset.y;
                    int nz = z + face.neighborOffset.z;

                    // Грань рисуем, только если сосед не сплошной (воздух или за краем чанка).
                    // Если сосед за границей ЭТОГО чанка — спрашиваем World (там может быть
                    // сосед из другого чанка). Без World (одиночный чанк, например корабль)
                    // считаем всё за границей воздухом.
                    BlockType neighbor;
                    if (InBounds(nx, ny, nz)) {
                        neighbor = GetBlock(nx, ny, nz);
                    } else if (world) {
                        glm::ivec3 wp = m_worldPos + glm::ivec3(nx, ny, nz);
                        neighbor = world->GetBlock(wp.x, wp.y, wp.z);
                    } else {
                        neighbor = BlockType::Air;
                    }
                    if (IsSolid(neighbor)) continue;

                    // Ось нормали (0=X,1=Y,2=Z) и две касательные оси грани —
                    // нужны, чтобы для каждой вершины узнать, по какую сторону
                    // блока (в плоскости грани) она находится.
                    int axisN = (face.normal.x != 0.0f) ? 0 : (face.normal.y != 0.0f ? 1 : 2);
                    int axisA = (axisN == 0) ? 1 : 0;
                    int axisB = (axisN == 2) ? 1 : 2;
                    glm::ivec3 tangentA(0), tangentB(0);
                    tangentA[axisA] = 1;
                    tangentB[axisB] = 1;
                    glm::ivec3 farCell(nx, ny, nz); // "воздушная" клетка за этой гранью

                    unsigned int baseIndex = static_cast<unsigned int>(vertices.size());
                    for (int i = 0; i < 4; ++i) {
                        int signA = (face.verts[i][axisA] > 0.5f) ? 1 : -1;
                        int signB = (face.verts[i][axisB] > 0.5f) ? 1 : -1;
                        float ao = vertexAO(farCell, tangentA * signA, tangentB * signB);

                        VoxelVertex vertex;
                        vertex.Position = glm::vec3(x, y, z) + face.verts[i];
                        vertex.Normal = face.normal;
                        vertex.Color = color * ao;
                        vertex.UV = tileOrigin + kQuadUV[i] * tileSize;
                        vertices.push_back(vertex);
                    }
                    indices.push_back(baseIndex + 0);
                    indices.push_back(baseIndex + 1);
                    indices.push_back(baseIndex + 2);
                    indices.push_back(baseIndex + 2);
                    indices.push_back(baseIndex + 3);
                    indices.push_back(baseIndex + 0);
                }
            }
        }
    }

    m_mesh.Upload(vertices, indices);
    m_dirty = false;
}
