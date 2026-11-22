#version 330 core
// Проход глубины для карты теней: рисуем геометрию из точки зрения солнца,
// нужна только позиция. И у Vertex, и у VoxelVertex позиция лежит в
// location 0, поэтому один и тот же depth-шейдер годится для обоих типов
// мешей (обычных объектов и воксельных чанков).
layout (location = 0) in vec3 aPos;

uniform mat4 uLightSpace; // проекция * вид из точки зрения солнца
uniform mat4 uModel;

void main() {
    gl_Position = uLightSpace * uModel * vec4(aPos, 1.0);
}
