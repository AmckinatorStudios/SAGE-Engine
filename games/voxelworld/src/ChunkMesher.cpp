#include "ChunkMesher.h"

#include <algorithm>

namespace voxel {
namespace {

// Грань: нормаль, начало и оси u, v (u × v = нормаль — обход против часовой
// снаружи, как у всей геометрии движка). Углы: o, o+u, o+u+v, o+v.
struct FaceDef {
    int N[3];
    int O[3];
    int U[3];
    int V[3];
};

const FaceDef kFaces[6] = {
    {{1, 0, 0}, {1, 0, 1}, {0, 0, -1}, {0, 1, 0}},  // +X
    {{-1, 0, 0}, {0, 0, 0}, {0, 0, 1}, {0, 1, 0}},  // -X
    {{0, 1, 0}, {0, 1, 1}, {1, 0, 0}, {0, 0, -1}},  // +Y
    {{0, -1, 0}, {0, 0, 0}, {1, 0, 0}, {0, 0, 1}},  // -Y
    {{0, 0, 1}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   // +Z
    {{0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}}, // -Z
};
const int kCornerUV[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

bool Occludes(uint8_t b) {
    const DrawKind k = Info(b).Draw;
    return k == DrawKind::Opaque || k == DrawKind::Cutout;
}

// Видна ли грань блока b, если за ней сосед n.
bool FaceVisible(uint8_t b, uint8_t n) {
    const DrawKind bk = Info(b).Draw, nk = Info(n).Draw;
    if (nk == DrawKind::Opaque) return false;
    switch (bk) {
        case DrawKind::Opaque:
        case DrawKind::Cutout: return true;
        case DrawKind::Glass: return n != b;
        case DrawKind::Liquid: return n != b && nk != DrawKind::Cutout && nk != DrawKind::Glass;
        default: return false;
    }
}

} // namespace

ChunkMeshes BuildChunkMesh(const World& world, int cx, int cz) {
    ChunkMeshes out;
    const int bx = cx * kChunkSize, bz = cz * kChunkSize;
    const Chunk* chunk = world.ChunkAt(cx, cz);
    if (!chunk) return out;

    for (int y = 0; y < kWorldHeight; ++y)
        for (int lz = 0; lz < kChunkSize; ++lz)
            for (int lx = 0; lx < kChunkSize; ++lx) {
                const uint8_t b = chunk->Blocks[Chunk::Index(lx, y, lz)];
                const BlockInfo& bi = Info(b);
                if (bi.Draw == DrawKind::None) continue;
                const int wx = bx + lx, wz = bz + lz;
                const bool liquidTop = bi.Draw == DrawKind::Liquid && world.Get(wx, y + 1, wz) != b;
                ChunkMeshData& mesh = (bi.Draw == DrawKind::Liquid || bi.Draw == DrawKind::Glass)
                                          ? out.Translucent
                                          : out.Opaque;

                for (int f = 0; f < 6; ++f) {
                    const FaceDef& fd = kFaces[f];
                    const int px = wx + fd.N[0], py = y + fd.N[1], pz = wz + fd.N[2];
                    const uint8_t n = world.Get(px, py, pz);
                    // Верх воды под воздухом рисуется всегда: он опущен и
                    // открывает щель, даже если сверху что-то прозрачное.
                    if (!FaceVisible(b, n) && !(liquidTop && f == 2 && n != b)) continue;

                    const uint32_t tile = f == 2 ? bi.TileTop : (f == 3 ? bi.TileBottom : bi.TileSide);
                    const uint32_t base = (uint32_t)(mesh.Vertices.size() / 2);
                    int ao[4];
                    for (int c = 0; c < 4; ++c) {
                        const int cu = kCornerUV[c][0], cv = kCornerUV[c][1];
                        const int du = cu ? 1 : -1, dv = cv ? 1 : -1;
                        auto at = [&](int su, int sv) {
                            return glm::ivec3(px + fd.U[0] * su + fd.V[0] * sv, py + fd.U[1] * su + fd.V[1] * sv,
                                              pz + fd.U[2] * su + fd.V[2] * sv);
                        };
                        const glm::ivec3 s1 = at(du, 0), s2 = at(0, dv), cc = at(du, dv);
                        const bool o1 = Occludes(world.Get(s1.x, s1.y, s1.z));
                        const bool o2 = Occludes(world.Get(s2.x, s2.y, s2.z));
                        const bool oc = Occludes(world.Get(cc.x, cc.y, cc.z));
                        ao[c] = (o1 && o2) ? 0 : 3 - ((int)o1 + (int)o2 + (int)oc);

                        // Сглаженный свет: среднее по прозрачным клеткам у угла.
                        int sky = world.SkyLight(px, py, pz), own = world.BlockLight(px, py, pz), cnt = 1;
                        const glm::ivec3 cells[3] = {s1, s2, cc};
                        const bool blocked[3] = {o1, o2, oc || (o1 && o2)};
                        for (int k = 0; k < 3; ++k) {
                            if (blocked[k]) continue;
                            sky += world.SkyLight(cells[k].x, cells[k].y, cells[k].z);
                            own += world.BlockLight(cells[k].x, cells[k].y, cells[k].z);
                            ++cnt;
                        }
                        // Своё свечение блока — на его же гранях (светокамень
                        // не может быть темнее собственного света).
                        const uint32_t sky4 = (uint32_t)std::min(60, sky * 4 / cnt);
                        const uint32_t own4 = (uint32_t)std::min(60, std::max(own * 4 / cnt, bi.Emission * 4));

                        const int vx = lx + fd.O[0] + fd.U[0] * cu + fd.V[0] * cv;
                        const int vy = y + fd.O[1] + fd.U[1] * cu + fd.V[1] * cv;
                        const int vz = lz + fd.O[2] + fd.U[2] * cu + fd.V[2] * cv;
                        uint32_t flags = 0;
                        if (liquidTop && vy == y + 1) flags |= kFlagLiquidTop;
                        if (bi.Draw == DrawKind::Cutout) flags |= kFlagSway;
                        const uint32_t w0 = (uint32_t)vx | ((uint32_t)vy << 5) | ((uint32_t)vz << 12) |
                                            ((uint32_t)f << 17) | ((uint32_t)c << 20) |
                                            ((uint32_t)ao[c] << 22) | (flags << 24);
                        const uint32_t w1 = tile | (sky4 << 8) | (own4 << 14);
                        mesh.Vertices.push_back(w0);
                        mesh.Vertices.push_back(w1);
                    }
                    // Диагональ квада — по AO: иначе затенённый угол «течёт»
                    // по треугольнику, и на ровной стене видна косая полоса.
                    if (ao[0] + ao[2] < ao[1] + ao[3]) {
                        for (uint32_t i : {1u, 2u, 3u, 3u, 0u, 1u}) mesh.Indices.push_back(base + i);
                    } else {
                        for (uint32_t i : {0u, 1u, 2u, 2u, 3u, 0u}) mesh.Indices.push_back(base + i);
                    }
                }
            }
    return out;
}

UnpackedVertex Unpack(uint32_t w0, uint32_t w1) {
    UnpackedVertex v;
    v.X = (int)(w0 & 31u);
    v.Y = (int)((w0 >> 5) & 127u);
    v.Z = (int)((w0 >> 12) & 31u);
    v.Face = (int)((w0 >> 17) & 7u);
    v.Corner = (int)((w0 >> 20) & 3u);
    v.AO = (int)((w0 >> 22) & 3u);
    v.Flags = (int)((w0 >> 24) & 3u);
    v.Tile = (int)(w1 & 255u);
    v.Sky = (float)((w1 >> 8) & 63u) / 4.0f;
    v.Own = (float)((w1 >> 14) & 63u) / 4.0f;
    return v;
}

} // namespace voxel
