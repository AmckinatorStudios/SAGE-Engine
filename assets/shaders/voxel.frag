#version 330 core
in vec3 FragPos;
in vec3 Normal;
in vec3 VertexColor;
in vec2 TexCoords;

out vec4 FragColor;

#define MAX_POINT_LIGHTS 8

struct PointLight {
    vec3 position;
    vec3 color;
    float intensity;
    float constant;
    float linear;
    float quadratic;
};

uniform vec3 uAmbientSky;
uniform vec3 uAmbientGround;
uniform float uAmbientStrength;

uniform vec3 uSunDir;   // направление, КУДА летит свет
uniform vec3 uSunColor;
uniform float uSunIntensity;

uniform PointLight uPointLights[MAX_POINT_LIGHTS];
uniform int uNumPointLights;

uniform sampler2D uAtlas;
uniform bool uUseTexture; // false — старое поведение: только цвет вершины (обратная совместимость)

vec3 CalcPointLight(PointLight light, vec3 normal, vec3 fragPos) {
    vec3 toLight = light.position - fragPos;
    float dist = length(toLight);
    vec3 lightDir = toLight / max(dist, 0.0001);
    float diff = max(dot(normal, lightDir), 0.0);
    float attenuation = 1.0 / (light.constant + light.linear * dist + light.quadratic * dist * dist);
    return diff * light.color * light.intensity * attenuation;
}

// Полусферический ambient: грани, смотрящие вверх, освещены uAmbientSky
// (цвет неба), смотрящие вниз — uAmbientGround (отражённый свет снизу).
// Между ними — плавный переход по normal.y, а не жёсткая ступенька.
vec3 CalcHemisphereAmbient(vec3 normal) {
    float skyWeight = normal.y * 0.5 + 0.5; // -1..1 -> 0..1
    return mix(uAmbientGround, uAmbientSky, skyWeight) * uAmbientStrength;
}

void main() {
    vec3 texel = uUseTexture ? texture(uAtlas, TexCoords).rgb : vec3(1.0);
    // VertexColor уже несёт в себе баковый ambient occlusion углов вокселя
    // (см. Chunk::RebuildMesh) — не только цвет блока.
    vec3 baseColor = texel * VertexColor;

    vec3 norm = normalize(Normal);

    vec3 ambient = CalcHemisphereAmbient(norm);

    vec3 sunDir = normalize(-uSunDir);
    float sunDiff = max(dot(norm, sunDir), 0.0);
    vec3 sunLight = sunDiff * uSunColor * uSunIntensity;

    vec3 pointLight = vec3(0.0);
    for (int i = 0; i < uNumPointLights; ++i) {
        pointLight += CalcPointLight(uPointLights[i], norm, FragPos);
    }

    vec3 result = (ambient + sunLight + pointLight) * baseColor;
    FragColor = vec4(result, 1.0);
}
