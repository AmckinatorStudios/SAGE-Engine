#version 330 core
// Сэмплим текстуру спрайта и умножаем на цвет-тонирование (uTint, с альфой) —
// так одним шейдером рисуются и обычные иконки (белый тинт), и подкрашенные
// спрайты, и затухание по альфе.
in vec2 vUV;

uniform sampler2D uTexture;
uniform vec4 uTint;

out vec4 FragColor;

void main() {
    FragColor = texture(uTexture, vUV) * uTint;
}
