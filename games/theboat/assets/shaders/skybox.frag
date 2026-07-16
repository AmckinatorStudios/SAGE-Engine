#version 330 core
in vec3 TexCoords;
out vec4 FragColor;

uniform samplerCube uSkybox;
// Тонирование неба под время суток: белый днём, тёмно-синий ночью,
// оранжевый на рассвете/закате. Умножается на статичную cubemap-текстуру.
uniform vec3 uTint;

void main() {
    FragColor = texture(uSkybox, TexCoords) * vec4(uTint, 1.0);
}
