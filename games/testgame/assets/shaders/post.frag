#version 330 core
in vec2 TexCoords;
out vec4 FragColor;

// HDR-текстура цвета сцены (см. Framebuffer, GL_RGBA16F)
uniform sampler2D uScene;

uniform float uExposure;   // множитель яркости перед тон-маппингом
uniform float uGamma;      // гамма-коррекция (~2.2)
uniform float uSaturation; // 1.0 — без изменений
uniform float uContrast;   // 1.0 — без изменений
uniform float uVignette;   // сила виньетки (0 — выкл)

// Плёночный тон-маппинг ACES (аппроксимация Narkowicz): переводит HDR в
// приятный LDR с мягким завалом ярких участков (highlight rolloff) вместо
// грубого клиппинга в белое.
vec3 ACESToneMap(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(uScene, TexCoords).rgb;

    // Экспозиция + плёночный тон-маппинг (HDR -> LDR)
    vec3 color = ACESToneMap(hdr * uExposure);

    // Насыщенность вокруг яркости (luma по коэффициентам Rec.709)
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(luma), color, uSaturation);

    // Контраст вокруг средне-серого
    color = (color - 0.5) * uContrast + 0.5;

    // Виньетка: затемнение к углам (квадрат расстояния от центра экрана)
    vec2 uv = TexCoords - 0.5;
    float vig = 1.0 - uVignette * dot(uv, uv) * 2.0;
    color *= clamp(vig, 0.0, 1.0);

    // Гамма-коррекция в самом конце
    color = pow(max(color, 0.0), vec3(1.0 / uGamma));

    FragColor = vec4(color, 1.0);
}
