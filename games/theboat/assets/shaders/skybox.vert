#version 330 core
layout (location = 0) in vec3 aPos;

out vec3 TexCoords;

uniform mat4 uView;
uniform mat4 uProjection;

void main() {
    TexCoords = aPos; // направление от центра куба = координата в cubemap
    vec4 pos = uProjection * uView * vec4(aPos, 1.0);
    // Трюк "z = w" гарантирует глубину 1.0 (максимально далеко) после
    // деления перспективы — skybox всегда будет позади любой другой геометрии
    gl_Position = pos.xyww;
}
