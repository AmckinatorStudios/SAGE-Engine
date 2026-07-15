#version 330 core
in vec3 FragPos;
in vec4 FragPosLightSpace;
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

uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform float uSunIntensity;

uniform vec3 uViewPos;

uniform PointLight uPointLights[MAX_POINT_LIGHTS];
uniform int uNumPointLights;

uniform float uTime;
// Скорость "хода корабля": корабль в мире статичен, но узор волн скользит
// мимо него по +Z, создавая ощущение движения вперёд.
uniform float uScrollSpeed;

uniform sampler2D uShadowMap;
uniform bool uShadowsEnabled;

// Доля затенения солнцем (PCF 3x3 + bias) — чтобы тень корабля ложилась и
// на воду, где она заметнее всего. См. подробности в voxel.frag.
float CalcSunShadow(vec4 fragPosLightSpace, vec3 normal, vec3 sunDir) {
    vec3 proj = fragPosLightSpace.xyz / fragPosLightSpace.w;
    proj = proj * 0.5 + 0.5;
    if (proj.z > 1.0) return 0.0;
    float currentDepth = proj.z;
    float bias = max(0.0025 * (1.0 - dot(normal, sunDir)), 0.0008);

    float shadow = 0.0;
    vec2 texelSize = 1.0 / textureSize(uShadowMap, 0);
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float pcfDepth = texture(uShadowMap, proj.xy + vec2(x, y) * texelSize).r;
            shadow += (currentDepth - bias > pcfDepth) ? 1.0 : 0.0;
        }
    }
    return shadow / 9.0;
}

// Нормаль воды из производных двух бегущих синусоид — дёшево, но достаточно,
// чтобы блики солнца и фонарей "играли" на волнах.
vec3 WaveNormal(vec2 p, float t) {
    float scroll = t * uScrollSpeed;
    vec2 q = vec2(p.x, p.y + scroll);
    float dx = 0.35 * 0.6 * cos(q.x * 0.6 + t * 1.3)
             + 0.20 * 1.7 * cos(q.x * 1.7 - q.y * 0.6 + t * 2.1);
    float dz = 0.35 * 0.5 * cos(q.y * 0.5 - t * 1.7)
             + 0.20 * 1.3 * cos(q.y * 1.3 + q.x * 0.4 + t * 1.9);
    return normalize(vec3(-dx, 1.0, -dz));
}

void main() {
    vec3 deepColor    = vec3(0.05, 0.22, 0.38);
    vec3 shallowColor = vec3(0.12, 0.42, 0.58);

    vec3 normal = WaveNormal(FragPos.xz, uTime);
    vec3 viewDir = normalize(uViewPos - FragPos);

    // Френель: под острым углом вода темнее и прозрачнее, у горизонта — светлее
    float fresnel = pow(1.0 - max(dot(normal, viewDir), 0.0), 3.0);
    vec3 waterColor = mix(deepColor, shallowColor, fresnel);

    float skyWeight = normal.y * 0.5 + 0.5;
    vec3 ambient = mix(uAmbientGround, uAmbientSky, skyWeight) * uAmbientStrength;

    vec3 sunDir = normalize(-uSunDir);
    float sunDiff = max(dot(normal, sunDir), 0.0);
    vec3 sunReflect = reflect(-sunDir, normal);
    float sunSpec = pow(max(dot(viewDir, sunReflect), 0.0), 96.0);
    vec3 sunLight = sunDiff * 0.6 * uSunColor * uSunIntensity + sunSpec * uSunColor * uSunIntensity;

    // Тень корабля на воде: гасим вклад солнца (и диффуз, и блик)
    float shadow = uShadowsEnabled ? CalcSunShadow(FragPosLightSpace, normal, sunDir) : 0.0;
    sunLight *= (1.0 - shadow);

    // Фонари дают бликующие "дорожки" на воде рядом с кораблём
    vec3 pointGlints = vec3(0.0);
    for (int i = 0; i < uNumPointLights; ++i) {
        PointLight light = uPointLights[i];
        vec3 toLight = light.position - FragPos;
        float dist = length(toLight);
        vec3 lightDir = toLight / max(dist, 0.0001);
        vec3 reflectDir = reflect(-lightDir, normal);
        float spec = pow(max(dot(viewDir, reflectDir), 0.0), 64.0);
        float diff = max(dot(normal, lightDir), 0.0) * 0.25;
        float attenuation = 1.0 / (light.constant + light.linear * dist + light.quadratic * dist * dist);
        pointGlints += (spec + diff) * light.color * light.intensity * attenuation;
    }

    vec3 lit = (ambient + sunLight) * waterColor + pointGlints;
    FragColor = vec4(lit, 0.82);
}
