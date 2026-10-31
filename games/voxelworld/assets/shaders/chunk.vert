#version 330 core
#include "voxel_common.glsl"

layout(location = 0) in uint aWord0;
layout(location = 1) in uint aWord1;

uniform mat4 uViewProj;
uniform vec3 uChunkOrigin;
uniform float uTime;

out vec2 vUV;
out vec2 vLight;
out float vShade;
out vec3 vWorld;

void main() {
    VoxelVertex v = voxel_unpack(aWord0, aWord1);
    vec3 p = uChunkOrigin + v.Pos;
    // Верх воды опущен и колышется; листва чуть качается на ветру.
    if ((v.Flags & 1u) != 0u)
        p.y -= 0.14 + 0.05 * sin(uTime * 1.6 + p.x * 0.7 + p.z * 0.45);
    if ((v.Flags & 2u) != 0u)
        p.xz += 0.035 * vec2(sin(uTime * 1.3 + p.y + p.z), cos(uTime * 1.1 + p.x));

    vUV = voxel_tile_uv(v.Tile, v.Corner);
    vLight = v.Light;
    // AO: 0 — угол зажат с двух сторон, 3 — открыт.
    vShade = voxel_face_shade(v.Face) * mix(0.42, 1.0, v.AO / 3.0);
    vWorld = p;
    gl_Position = uViewProj * vec4(p, 1.0);
}
