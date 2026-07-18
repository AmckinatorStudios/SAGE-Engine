#include "ParticleSystem.h"

// Встроенный шейдер частиц — billboard-квад, разворачиваемый к камере через
// uCameraRight/uCameraUp, с формой (мягкий круг / квад) по uShape. Инстанс-поток
// (позиция/размер/цвет/поворот) совпадает с ParticleSystem::InstanceData.
namespace {

const char* kParticleVert = R"(#version 330 core
layout (location = 0) in vec2 aCorner;    // -0.5..0.5 угол квада
layout (location = 1) in vec3 iWorldPos;
layout (location = 2) in float iSize;
layout (location = 3) in vec4 iColor;
layout (location = 4) in float iRotation;

out vec4 vColor;
out vec2 vLocal; // -1..1 для формы во фрагменте

uniform mat4 uView;
uniform mat4 uProjection;
uniform vec3 uCameraRight;
uniform vec3 uCameraUp;

void main() {
    float c = cos(iRotation), s = sin(iRotation);
    vec2 r = vec2(aCorner.x * c - aCorner.y * s, aCorner.x * s + aCorner.y * c);
    vec3 world = iWorldPos + uCameraRight * (r.x * iSize) + uCameraUp * (r.y * iSize);
    gl_Position = uProjection * uView * vec4(world, 1.0);
    vColor = iColor;
    vLocal = aCorner * 2.0;
}
)";

const char* kParticleFrag = R"(#version 330 core
in vec4 vColor;
in vec2 vLocal;
out vec4 FragColor;

uniform int uShape; // 0 — мягкий круг, 1 — квад

void main() {
    float alpha = vColor.a;
    if (uShape == 0) {
        float d = length(vLocal);
        if (d > 1.0) discard;
        alpha *= smoothstep(1.0, 0.25, d); // мягкий край
    }
    FragColor = vec4(vColor.rgb, alpha);
}
)";

} // namespace

Shader& ParticleSystem::BuiltinShader() {
    // Намеренно не уничтожается (как встроенный скиннинг-шейдер): function-local
    // static Shader снёс бы GL-программу уже после разрушения контекста.
    static Shader* shader = new Shader(Shader::FromSource(kParticleVert, kParticleFrag, "ParticleSystem"));
    return *shader;
}
