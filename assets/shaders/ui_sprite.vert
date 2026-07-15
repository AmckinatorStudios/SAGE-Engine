#version 330 core
// Шейдер UI-спрайтов: позиция уже в экранных пикселях, ортопроекция
// переводит её в клип-пространство (как в debug_text.vert). Плюс UV для
// сэмплинга текстуры спрайта.
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;

uniform mat4 uProjection;

out vec2 vUV;

void main() {
    gl_Position = uProjection * vec4(aPos, 0.0, 1.0);
    vUV = aUV;
}
