#version 330 core
// Геометрия одной частицы: unit-квад в локальных координатах (-0.5..0.5),
// общий для всех частиц — своя позиция/размер/цвет приходят per-instance.
layout (location = 0) in vec2 aQuadPos;

// Per-instance атрибуты (см. Particle::ToInstanceData в ParticleSystem.h)
layout (location = 1) in vec3 iWorldPos;
layout (location = 2) in float iSize;
layout (location = 3) in vec4 iColor;
layout (location = 4) in float iRotation;

out vec4 vColor;
out vec2 vQuadPos;

uniform mat4 uView;
uniform mat4 uProjection;
// Право/верх камеры в мировых координатах — растягиваем квад так, чтобы
// он всегда смотрел на камеру (billboard), не читая всю view-матрицу в GLSL
uniform vec3 uCameraRight;
uniform vec3 uCameraUp;

void main() {
    vColor = iColor;
    vQuadPos = aQuadPos;

    float s = sin(iRotation), c = cos(iRotation);
    vec2 rotated = vec2(aQuadPos.x * c - aQuadPos.y * s, aQuadPos.x * s + aQuadPos.y * c);

    vec3 worldOffset = (uCameraRight * rotated.x + uCameraUp * rotated.y) * iSize;
    gl_Position = uProjection * uView * vec4(iWorldPos + worldOffset, 1.0);
}
