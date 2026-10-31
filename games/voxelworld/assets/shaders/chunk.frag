#version 330 core
in vec2 vUV;
in vec2 vLight;
in float vShade;
in vec3 vWorld;

out vec4 FragColor;

uniform sampler2D uAtlas;     // плитки блоков (sRGB -> линейный)
uniform sampler2D uLightmap;  // 16×16: (небо, свой) -> цвет света
uniform vec3 uCameraPos;
uniform vec3 uFogColor;
uniform vec2 uFog;            // начало и конец тумана, блоков
uniform int uTranslucent;     // 0 — глухой проход (вырез), 1 — вода и стекло
uniform int uUnderwater;

void main() {
    vec4 tex = texture(uAtlas, vUV);
    if (uTranslucent == 0 && tex.a < 0.5) discard;
    // Карта света — середины текселей: уровни 0..15 в координаты 0..1.
    vec3 light = texture(uLightmap, (vLight + 0.5) / 16.0).rgb;
    vec3 col = tex.rgb * light * vShade;

    float dist = length(vWorld - uCameraPos);
    vec2 fog = uUnderwater != 0 ? vec2(2.0, 18.0) : uFog;
    vec3 fogColor = uUnderwater != 0 ? vec3(0.03, 0.09, 0.18) : uFogColor;
    col = mix(col, fogColor, smoothstep(fog.x, fog.y, dist));
    FragColor = vec4(col, uTranslucent != 0 ? tex.a : 1.0);
}
