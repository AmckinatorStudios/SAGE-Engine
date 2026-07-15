#version 330 core
layout (location = 0) in vec3 aPos;

out vec3 FragPos;
out vec4 FragPosLightSpace; // позиция фрагмента в пространстве света (для теней)

uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uLightSpace; // проекция * вид из точки зрения солнца (карта теней)
// Плоскость воды "бесконечная": квад фиксированного размера просто следует
// за игроком по XZ, чтобы горизонт никогда не кончался.
uniform vec2 uCenter;
uniform float uTime;

void main() {
    vec3 world = aPos + vec3(uCenter.x, 0.0, uCenter.y);
    // Лёгкая вертикальная зыбь на вершинах — заметна на близких волнах.
    // Волна привязана к мировым координатам, поэтому у неё есть "течение".
    world.y += 0.10 * sin(world.x * 0.35 + uTime * 1.1)
             + 0.08 * sin(world.z * 0.28 - uTime * 1.6);
    FragPos = world;
    FragPosLightSpace = uLightSpace * vec4(world, 1.0);
    gl_Position = uProjection * uView * vec4(world, 1.0);
}
