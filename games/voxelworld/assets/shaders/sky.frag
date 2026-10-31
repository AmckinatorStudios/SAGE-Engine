#version 330 core
#include <sage/common.glsl>
in vec2 vNdc;
out vec4 FragColor;

uniform mat4 uInvViewProj;
uniform vec3 uCameraPos;
uniform vec3 uZenith;
uniform vec3 uHorizon;
uniform vec3 uSunDir;   // к солнцу
uniform float uDaylight; // 0 ночь .. 1 день
uniform float uTime;

// Квадратное светило: блочному миру круглое солнце чужое.
float square_disc(vec3 dir, vec3 axis, float size) {
    vec3 up = abs(axis.y) > 0.95 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3 t = normalize(cross(up, axis));
    vec3 b = cross(axis, t);
    float d = dot(dir, axis);
    if (d <= 0.0) return 0.0;
    vec2 q = vec2(dot(dir, t), dot(dir, b)) / d;
    return step(max(abs(q.x), abs(q.y)), size);
}

void main() {
    vec4 far = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 dir = normalize(far.xyz / far.w - uCameraPos);

    float h = clamp(dir.y, -1.0, 1.0);
    vec3 col = mix(uHorizon, uZenith, pow(clamp(h, 0.0, 1.0), 0.55));
    if (h < 0.0) col = mix(uHorizon, uHorizon * 0.55, clamp(-h * 3.0, 0.0, 1.0));

    // Звёзды — на квантованных направлениях, только ночью.
    vec3 cell = floor(dir * 180.0);
    float star = step(0.9965, sage_hash12(cell.xy + cell.z * 17.0)) * (1.0 - uDaylight) * step(0.0, h);
    col += vec3(star * 0.9);

    col += vec3(1.0, 0.92, 0.7) * 3.0 * square_disc(dir, uSunDir, 0.07);
    col += vec3(0.75, 0.8, 0.95) * 1.2 * square_disc(dir, -uSunDir, 0.05);

    // Плоские блочные облака: пересечение луча с плоскостью и клетки 12×12.
    const float cloudY = 118.0;
    if (dir.y > 0.01) {
        float t = (cloudY - uCameraPos.y) / dir.y;
        vec2 p = uCameraPos.xz + dir.xz * t + vec2(uTime * 1.2, 0.0);
        vec2 c = floor(p / 12.0);
        float on = step(0.62, sage_hash12(c)) ;
        float fade = 1.0 - smoothstep(250.0, 700.0, t);
        vec3 cloud = mix(vec3(0.035, 0.04, 0.06), vec3(1.0), uDaylight);
        col = mix(col, cloud, on * fade * 0.85);
    }
    FragColor = vec4(col, 1.0);
}
