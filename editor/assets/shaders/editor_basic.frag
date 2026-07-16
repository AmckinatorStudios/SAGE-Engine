#version 330 core
in vec3 vNormal;
out vec4 FragColor;

uniform vec3 uObjectColor;

// Простое направленное затенение — чтобы кубы в редакторе читались как объёмные,
// без полноценной модели освещения (это лишь превью сцены).
void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(vec3(0.4, 0.85, 0.35));
    float diff = max(dot(N, L), 0.0) * 0.75 + 0.25; // мягкий ambient-подмес
    FragColor = vec4(uObjectColor * diff, 1.0);
}
