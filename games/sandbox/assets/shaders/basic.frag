#version 330 core
in vec3 vNormal;
out vec4 FragColor;

uniform vec3 uObjectColor;

// Простое направленное затенение (без теней/пост-процесса/точечных
// источников) — этого достаточно, чтобы кубы читались как объёмные. Игра со
// своими требованиями к освещению пишет свой шейдер/подключает подсистемы
// движка (ShadowMap, PostProcess, LightingUpload) так, как это делает
// TheBoatLayer в истории проекта (см. git log) — sandbox сознательно этого
// не показывает, чтобы оставаться минимальным примером.
void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(vec3(0.4, 0.85, 0.35));
    float diff = max(dot(N, L), 0.0) * 0.75 + 0.25; // мягкий ambient-подмес
    FragColor = vec4(uObjectColor * diff, 1.0);
}
