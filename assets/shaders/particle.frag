#version 330 core
in vec4 vColor;
in vec2 vQuadPos;
out vec4 FragColor;

// Пресет формы частицы: 0 = мягкий круг (искры/дым/капли), 1 = обычный квад
uniform int uShape;

void main() {
    vec4 color = vColor;
    if (uShape == 0) {
        float dist = length(vQuadPos) * 2.0; // 0 в центре, ~1.4 в углах квада
        float alpha = smoothstep(1.0, 0.0, dist); // мягкий спад к краю — вместо жёсткого круга
        color.a *= alpha;
    }
    if (color.a <= 0.003) discard; // не тратим фрагменты на полностью прозрачные пиксели
    FragColor = color;
}
