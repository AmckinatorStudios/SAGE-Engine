// Общее для шейдеров воксельного мира: разбор упакованной вершины чанка.
// Раскладка битов — та же, что в src/ChunkMesher.h.
struct VoxelVertex {
    vec3 Pos;      // внутри чанка
    int Face;      // 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z
    vec2 Corner;   // угол грани (0/1, 0/1)
    float AO;      // 0..3
    uint Flags;
    uint Tile;
    vec2 Light;    // небо, свой — 0..15
};

VoxelVertex voxel_unpack(uint w0, uint w1) {
    VoxelVertex v;
    v.Pos = vec3(float(w0 & 31u), float((w0 >> 5) & 127u), float((w0 >> 12) & 31u));
    v.Face = int((w0 >> 17) & 7u);
    uint c = (w0 >> 20) & 3u;
    v.Corner = vec2(c == 1u || c == 2u ? 1.0 : 0.0, c >= 2u ? 1.0 : 0.0);
    v.AO = float((w0 >> 22) & 3u);
    v.Flags = (w0 >> 24) & 3u;
    v.Tile = w1 & 255u;
    v.Light = vec2(float((w1 >> 8) & 63u), float((w1 >> 14) & 63u)) * 0.25;
    return v;
}

// Плитка атласа 8×8; строки текстуры снизу вверх, плитки считаются сверху.
vec2 voxel_tile_uv(uint tile, vec2 corner) {
    float tx = float(tile % 8u), ty = float(tile / 8u);
    return vec2((tx + corner.x) / 8.0, 1.0 - (ty + 1.0 - corner.y) / 8.0);
}

// Освещённость граней по сторонам света — блоки читаются объёмом даже там,
// где свет одинаков со всех сторон (пасмурно, пещера).
float voxel_face_shade(int face) {
    if (face == 2) return 1.0;
    if (face == 3) return 0.5;
    if (face == 4 || face == 5) return 0.8;
    return 0.62;
}
