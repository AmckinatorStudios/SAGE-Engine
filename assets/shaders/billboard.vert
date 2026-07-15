#version 330 core
// Единичный квад в локальных координатах (-0.5..0.5) + его же UV
layout (location = 0) in vec2 aQuadPos;
layout (location = 1) in vec2 aUV;

out vec2 vUV;

uniform vec3 uWorldPos;   // мировая позиция центра билборда
uniform vec2 uSize;       // (ширина, высота) в мировых единицах
uniform vec2 uPivot;      // якорь квада: (0,0) центр, (0,-0.5) низ (стоит на земле), и т.д.
uniform float uRotation;  // поворот вокруг оси взгляда, радианы

uniform mat4 uView;
uniform mat4 uProjection;
// Право/верх камеры в мировых координатах — билборд всегда развёрнут
// к камере, без необходимости читать всю view-матрицу в шейдере
uniform vec3 uCameraRight;
uniform vec3 uCameraUp;

void main() {
    vUV = aUV;

    vec2 local = (aQuadPos + uPivot) * uSize;
    float s = sin(uRotation), c = cos(uRotation);
    vec2 rotated = vec2(local.x * c - local.y * s, local.x * s + local.y * c);

    vec3 worldOffset = uCameraRight * rotated.x + uCameraUp * rotated.y;
    gl_Position = uProjection * uView * vec4(uWorldPos + worldOffset, 1.0);
}
