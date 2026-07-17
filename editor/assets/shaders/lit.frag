#version 330 core
in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoords;
in vec4 FragPosLightSpace;

out vec4 FragColor;

#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS 8

struct PointLight {
    vec3 position;
    vec3 color;
    float intensity;
    float constant;
    float linear;
    float quadratic;
};

// Прожектор: точечный источник, светящий конусом вдоль direction. cosInner/
// cosOuter — косинусы полууглов внутреннего/внешнего конуса (мягкий край).
struct SpotLight {
    vec3 position;
    vec3 direction; // КУДА светит конус
    vec3 color;
    float intensity;
    float constant;
    float linear;
    float quadratic;
    float cosInner;
    float cosOuter;
};

uniform vec3 uObjectColor;

uniform vec3 uAmbientSky;
uniform vec3 uAmbientGround;
uniform float uAmbientStrength;

uniform vec3 uSunDir;   // направление, КУДА летит свет
uniform vec3 uSunColor;
uniform float uSunIntensity;

uniform PointLight uPointLights[MAX_POINT_LIGHTS];
uniform int uNumPointLights;

uniform SpotLight uSpotLights[MAX_SPOT_LIGHTS];
uniform int uNumSpotLights;

uniform vec3 uViewPos;

uniform sampler2D uTexture;
uniform bool uUseTexture; // false — используем uObjectColor как раньше (обратная совместимость)

uniform sampler2D uShadowMap;
uniform bool uShadowsEnabled;

// Режим затенения: 0 = полное освещение, 1 = unlit (плоский базовый цвет),
// 2 = визуализация нормалей. Задаётся редактором (View > Render Mode).
uniform int uShadingMode;

// Линейный туман: фрагменты дальше uFogStart плавно уходят в uFogColor к
// uFogEnd. Атмосфера сцены (серийализуется в environment). Применяется только
// в режиме полного освещения.
uniform bool uFogEnabled;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;

// Доля затенения фрагмента солнцем: 0 — освещён, 1 — в тени (PCF 3x3 +
// slope-scaled bias). Тени только от солнца — точечные/прожекторы не затеняются.
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

// Полусферический ambient: верх объекта тянется к цвету неба, низ — к
// отражённому свету снизу (вода/палуба), переход плавный по normal.y.
vec3 CalcHemisphereAmbient(vec3 normal) {
    float skyWeight = normal.y * 0.5 + 0.5;
    return mix(uAmbientGround, uAmbientSky, skyWeight) * uAmbientStrength;
}

vec3 CalcPointLight(PointLight light, vec3 normal, vec3 fragPos, vec3 viewDir) {
    vec3 toLight = light.position - fragPos;
    float dist = length(toLight);
    vec3 lightDir = toLight / max(dist, 0.0001);

    float diff = max(dot(normal, lightDir), 0.0);

    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);

    float attenuation = 1.0 / (light.constant + light.linear * dist + light.quadratic * dist * dist);

    vec3 diffuse = diff * light.color * light.intensity;
    vec3 specular = 0.5 * spec * light.color * light.intensity;
    return (diffuse + specular) * attenuation;
}

vec3 CalcSpotLight(SpotLight light, vec3 normal, vec3 fragPos, vec3 viewDir) {
    vec3 toLight = light.position - fragPos;
    float dist = length(toLight);
    vec3 lightDir = toLight / max(dist, 0.0001);

    // Конус: угол между направлением НА свет и осью прожектора (обратной к
    // direction). theta=1 — точно по оси; плавный спад между cosInner..cosOuter.
    float theta = dot(lightDir, normalize(-light.direction));
    float epsilon = max(light.cosInner - light.cosOuter, 0.0001);
    float cone = clamp((theta - light.cosOuter) / epsilon, 0.0, 1.0);
    if (cone <= 0.0) return vec3(0.0);

    float diff = max(dot(normal, lightDir), 0.0);
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);

    float attenuation = 1.0 / (light.constant + light.linear * dist + light.quadratic * dist * dist);

    vec3 diffuse = diff * light.color * light.intensity;
    vec3 specular = 0.5 * spec * light.color * light.intensity;
    return (diffuse + specular) * attenuation * cone;
}

void main() {
    vec3 baseColor = uUseTexture ? texture(uTexture, TexCoords).rgb * uObjectColor : uObjectColor;
    vec3 norm = normalize(Normal);

    // Отладочные режимы затенения (без освещения/тумана).
    if (uShadingMode == 2) { FragColor = vec4(norm * 0.5 + 0.5, 1.0); return; } // нормали
    if (uShadingMode == 1) { FragColor = vec4(baseColor, 1.0); return; }         // unlit

    vec3 viewDir = normalize(uViewPos - FragPos);

    // ambient (полусферический — зависит от нормали, не плоский)
    vec3 ambient = CalcHemisphereAmbient(norm);

    // солнце: diffuse + specular
    vec3 sunDir = normalize(-uSunDir);
    float sunDiff = max(dot(norm, sunDir), 0.0);
    vec3 sunReflect = reflect(-sunDir, norm);
    float sunSpec = pow(max(dot(viewDir, sunReflect), 0.0), 32.0);
    vec3 sunLight = (sunDiff + 0.5 * sunSpec) * uSunColor * uSunIntensity;

    // Тень гасит только вклад солнца (ambient/фонари/прожекторы не затеняются)
    float shadow = uShadowsEnabled ? CalcSunShadow(FragPosLightSpace, norm, sunDir) : 0.0;
    sunLight *= (1.0 - shadow);

    // точечные источники (лампы, факелы)
    vec3 pointLight = vec3(0.0);
    for (int i = 0; i < uNumPointLights; ++i) {
        pointLight += CalcPointLight(uPointLights[i], norm, FragPos, viewDir);
    }

    // прожекторы (фонарики, лампы-споты, сценический свет)
    vec3 spotLight = vec3(0.0);
    for (int i = 0; i < uNumSpotLights; ++i) {
        spotLight += CalcSpotLight(uSpotLights[i], norm, FragPos, viewDir);
    }

    vec3 result = (ambient + sunLight + pointLight + spotLight) * baseColor;

    // Линейный туман: чем дальше фрагмент, тем сильнее уходит в цвет тумана.
    if (uFogEnabled) {
        float dist = length(uViewPos - FragPos);
        float f = clamp((uFogEnd - dist) / max(uFogEnd - uFogStart, 0.0001), 0.0, 1.0);
        result = mix(uFogColor, result, f);
    }

    FragColor = vec4(result, 1.0);
}
