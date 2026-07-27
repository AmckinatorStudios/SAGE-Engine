#pragma once

// ---------------------------------------------------------------------------
// Общий GLSL-блок физически-корректного освещения (PBR, metallic-roughness,
// Cook-Torrance GGX). Встраивается во ВСЕ шейдеры, освещающие геометрию
// (инстансный батч, текстурный PBR, скиннинг) — единственный источник правды о
// модели освещения, чтобы всё выглядело одинаково. НЕ содержит #version, in/out
// и main — их даёт конкретный шейдер; здесь только uniform'ы освещения (имена
// совпадают с UploadLighting/UploadShadowUniforms), BRDF-функции и ShadePBR().
// ---------------------------------------------------------------------------
namespace sage::render {

inline const char* kPbrSharedGlsl = R"GLSL(
#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS 8
const float PI = 3.14159265359;

struct PointLight { vec3 position; vec3 color; float intensity; float constant; float linear; float quadratic; };
struct SpotLight  { vec3 position; vec3 direction; vec3 color; float intensity; float constant; float linear; float quadratic; float cosInner; float cosOuter; };

uniform vec3 uAmbientSky;
uniform vec3 uAmbientGround;
uniform float uAmbientStrength;
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform float uSunIntensity;
uniform PointLight uPointLights[MAX_POINT_LIGHTS];
uniform int uNumPointLights;
uniform SpotLight uSpotLights[MAX_SPOT_LIGHTS];
uniform int uNumSpotLights;
uniform vec3 uViewPos;
// Карты теней: по одной на каскад. Массив сэмплеров тут не годится — GLSL 330
// требует индексировать его константой, а каскад выбирается на лету. Поэтому
// четыре именованных сэмплера и развёрнутый выбор ниже (см. CalcSunShadow).
uniform sampler2D uShadowMap;   // каскад 0 — самый ближний и подробный
uniform sampler2D uShadowMap1;
uniform sampler2D uShadowMap2;
uniform sampler2D uShadowMap3;
uniform mat4 uShadowLightSpace[4];
uniform int uShadowCascades;    // сколько каскадов реально заполнено (1..4)
uniform bool uShadowsEnabled;
uniform int uShadingMode; // 0 lit / 1 unlit / 2 normals (решает вызывающий main)
uniform bool uFogEnabled;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;

float DistributionGGX(vec3 N, vec3 H, float rough) {
    float a = rough * rough;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}
float GeometrySchlickGGX(float NdotV, float rough) {
    float r = rough + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}
float GeometrySmith(vec3 N, vec3 V, vec3 L, float rough) {
    return GeometrySchlickGGX(max(dot(N, V), 0.0), rough) *
           GeometrySchlickGGX(max(dot(N, L), 0.0), rough);
}
vec3 FresnelSchlick(float cosT, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosT, 0.0, 1.0), 5.0);
}

// Одно сравнение глубины: 1 — в тени, 0 — на свету.
float ShadowTest(sampler2D map, vec2 uv, float depth, float bias) {
    return (depth - bias > texture(map, uv).r) ? 1.0 : 0.0;
}

// Билинейная фильтрация СРАВНЕНИЙ, а не глубин. Усреднять глубины соседних
// текселей бессмысленно: между ступенькой и полом среднее даёт глубину, какой
// в сцене нет, и получается ореол. Сравнивать надо в каждом из четырёх
// текселей, а смешивать уже результаты — 0 и 1. Ровно это делает аппаратный
// sampler2DShadow; здесь то же самое руками, без изменения формата текстуры.
float ShadowBilinear(sampler2D map, vec2 uv, float depth, float bias, vec2 texel) {
    vec2 grid = uv / texel - 0.5;
    vec2 frac = fract(grid);
    vec2 base = (floor(grid) + 0.5) * texel;
    float s00 = ShadowTest(map, base, depth, bias);
    float s10 = ShadowTest(map, base + vec2(texel.x, 0.0), depth, bias);
    float s01 = ShadowTest(map, base + vec2(0.0, texel.y), depth, bias);
    float s11 = ShadowTest(map, base + texel, depth, bias);
    return mix(mix(s00, s10, frac.x), mix(s01, s11, frac.x), frac.y);
}

// Тень по одной карте. pc — координаты фрагмента в пространстве этого каскада,
// уже приведённые к [0,1].
float ShadowPCF(sampler2D map, vec3 pc, vec3 normal, float ndl) {
    vec2 texel = 1.0 / vec2(textureSize(map, 0));

    // Смещение по нормали, а не только по глубине. Наклонная поверхность
    // покрывается текселями по диагонали, и «правильная» глубина внутри одного
    // текселя меняется — отсюда полосы самозатенения. Сдвиг точки выборки
    // ВДОЛЬ НОРМАЛИ на размер текселя убирает их, не отрывая тень от предмета,
    // как это делает большой постоянный bias.
    float slope = clamp(1.0 - abs(ndl), 0.0, 1.0);
    vec2 uv = pc.xy + normal.xz * texel * (0.75 + 2.0 * slope);

    float bias = max(0.0018 * slope, 0.0004);

    // Сетка 3x3 билинейных тапов: мягкая кромка шириной около трёх текселей.
    // Дороже одного тапа в девять раз, но именно это отличает тень с ровным
    // краем от лесенки, и платить тут стоит.
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            shadow += ShadowBilinear(map, uv + vec2(x, y) * texel, pc.z, bias, texel);
    return shadow / 9.0;
}

// Координаты фрагмента в пространстве каскада, приведённые к [0,1].
vec3 CascadeCoords(int cascade, vec3 worldPos) {
    vec4 lp = uShadowLightSpace[cascade] * vec4(worldPos, 1.0);
    return (lp.xyz / lp.w) * 0.5 + 0.5;
}

// Влезает ли фрагмент в каскад с запасом на ядро фильтра. Запас обязателен:
// на самом краю карты тапы 3x3 уходят за её пределы и приносят оттуда мусор,
// который выглядит как рамка ложной тени по границе каскада.
bool InsideCascade(vec3 pc, float margin) {
    return pc.z <= 1.0 &&
           all(greaterThanEqual(pc.xy, vec2(margin))) &&
           all(lessThanEqual(pc.xy, vec2(1.0 - margin)));
}

// Насколько фрагмент близок к краю каскада: 0 — в середине, 1 — на границе.
// По этому числу ближний и дальний каскады смешиваются в узкой полосе, иначе
// на стыке видна ступенька: слева тень мягкая и подробная, справа — грубая.
float CascadeEdge(vec3 pc, float band) {
    vec2 d = min(pc.xy, vec2(1.0) - pc.xy); // расстояние до ближайшего края
    float m = min(d.x, d.y);
    return 1.0 - clamp(m / band, 0.0, 1.0);
}

float CalcSunShadow(vec3 worldPos, vec3 normal, vec3 sunDir) {
    float ndl = dot(normal, sunDir);
    const float kMargin = 0.02; // запас под ядро 3x3 у края карты
    const float kBand = 0.10;   // ширина полосы смешивания каскадов

    // Каскады перебираются от ближнего к дальнему, и берётся ПЕРВЫЙ
    // подходящий: он самый подробный из тех, что накрывают эту точку.
    //
    // Выбор развёрнут вручную и выглядит длинно. Причина не в лени: GLSL 330
    // запрещает индексировать массив сэмплеров переменной, а sampler нельзя
    // положить в локальную переменную. Цикл здесь физически невозможен.
    vec3 pc0 = CascadeCoords(0, worldPos);
    if (InsideCascade(pc0, kMargin)) {
        float s = ShadowPCF(uShadowMap, pc0, normal, ndl);
        if (uShadowCascades > 1) {
            float w = CascadeEdge(pc0, kBand);
            if (w > 0.0) {
                vec3 pc1 = CascadeCoords(1, worldPos);
                float far = InsideCascade(pc1, kMargin) ? ShadowPCF(uShadowMap1, pc1, normal, ndl) : 0.0;
                s = mix(s, far, w);
            }
        }
        return s;
    }
    if (uShadowCascades > 1) {
        vec3 pc1 = CascadeCoords(1, worldPos);
        if (InsideCascade(pc1, kMargin)) {
            float s = ShadowPCF(uShadowMap1, pc1, normal, ndl);
            if (uShadowCascades > 2) {
                float w = CascadeEdge(pc1, kBand);
                if (w > 0.0) {
                    vec3 pc2 = CascadeCoords(2, worldPos);
                    float far = InsideCascade(pc2, kMargin) ? ShadowPCF(uShadowMap2, pc2, normal, ndl) : 0.0;
                    s = mix(s, far, w);
                }
            }
            return s;
        }
    }
    if (uShadowCascades > 2) {
        vec3 pc2 = CascadeCoords(2, worldPos);
        if (InsideCascade(pc2, kMargin)) {
            float s = ShadowPCF(uShadowMap2, pc2, normal, ndl);
            if (uShadowCascades > 3) {
                float w = CascadeEdge(pc2, kBand);
                if (w > 0.0) {
                    vec3 pc3 = CascadeCoords(3, worldPos);
                    float far = InsideCascade(pc3, kMargin) ? ShadowPCF(uShadowMap3, pc3, normal, ndl) : 0.0;
                    s = mix(s, far, w);
                }
            }
            return s;
        }
    }
    if (uShadowCascades > 3) {
        vec3 pc3 = CascadeCoords(3, worldPos);
        if (InsideCascade(pc3, kMargin)) return ShadowPCF(uShadowMap3, pc3, normal, ndl);
    }
    // Дальше последнего каскада тени нет — там всё освещено. Тянуть её краем
    // карты значило бы рисовать полосу ложной тени до горизонта.
    return 0.0;
}

vec3 CalcHemisphereAmbient(vec3 normal) {
    float w = normal.y * 0.5 + 0.5;
    return mix(uAmbientGround, uAmbientSky, w) * uAmbientStrength;
}

// --- Запечённое GI (см. sage/gi): объём световых проб (L1 SH) -------------
// Три 3D-текстуры — по одной на цветовой канал, в каждой vec4 гармоник
// (c0, cx, cy, cz). Семплится per-pixel по мировой позиции: аппаратная
// трилинейная интерполяция между пробами. Значение — «непрямая освещённость/π»
// (та же конвенция, что CalcHemisphereAmbient) — прямой множитель albedo.
uniform bool uGIVolumeEnabled;
uniform sampler3D uGIVolR;
uniform sampler3D uGIVolG;
uniform sampler3D uGIVolB;
uniform vec3 uGIVolMin;       // мировая позиция пробы (0,0,0)
uniform vec3 uGIVolInvExtent; // 1 / (CellSize * (Dims-1))
uniform vec3 uGIVolDims;      // число проб по осям

vec3 EvalGIVolume(vec3 worldPos, vec3 n) {
    vec3 uvw = clamp((worldPos - uGIVolMin) * uGIVolInvExtent, 0.0, 1.0);
    uvw = (uvw * (uGIVolDims - 1.0) + 0.5) / uGIVolDims; // узлы -> центры текселей
    vec4 shR = texture(uGIVolR, uvw);
    vec4 shG = texture(uGIVolG, uvw);
    vec4 shB = texture(uGIVolB, uvw);
    const float w0 = 0.282095;           // Y00 (свёртка косинусом, /π)
    const float w1 = 0.325735;           // (2/3)·0.488603
    vec4 basis = vec4(w0, w1 * n.x, w1 * n.y, w1 * n.z);
    return max(vec3(dot(shR, basis), dot(shG, basis), dot(shB, basis)), vec3(0.0));
}

// Непрямая составляющая по умолчанию: GI-объём, если запечён, иначе прежний
// полусферический ambient. Лайтмапнутая статика подставляет свою лайтмапу
// напрямую через ShadePBRgi (см. RenderBatch).
vec3 DefaultIndirect(vec3 worldPos, vec3 n) {
    return uGIVolumeEnabled ? EvalGIVolume(worldPos, n) : CalcHemisphereAmbient(n);
}

// Вклад одного источника (Cook-Torrance): N — нормаль, V — к камере, L — к свету,
// radiance — цвет*интенсивность источника (с затуханием/тенью, посчитанными выше).
vec3 PbrContrib(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic, float rough) {
    vec3 H = normalize(V + L);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float NDF = DistributionGGX(N, H, rough);
    float G = GeometrySmith(N, V, L, rough);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 spec = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0), 1e-4);
    vec3 kd = (vec3(1.0) - F) * (1.0 - metallic);
    float NdotL = max(dot(N, L), 0.0);
    return (kd * albedo / PI + spec) * radiance * NdotL;
}

// Полное PBR-освещение фрагмента с ЯВНОЙ непрямой составляющей (indirect —
// «освещённость/π»: лайтмапа, GI-объём или полусферический ambient; умножает
// albedo и ao). Прямой свет — realtime: солнце (с тенью) + точечные +
// прожекторы + туман. shadingMode 1/2 обрабатывает вызывающий main.
vec3 ShadePBRgi(vec3 N, vec3 fragPos, vec3 albedo, float metallic, float rough, float ao, vec3 indirect) {
    vec3 V = normalize(uViewPos - fragPos);
    vec3 ambient = indirect * albedo * ao;
    vec3 Lo = vec3(0.0);

    // Солнце (направленное) + PCF-тень.
    vec3 sunL = normalize(-uSunDir);
    float shadow = uShadowsEnabled ? CalcSunShadow(fragPos, N, sunL) : 0.0;
    Lo += PbrContrib(N, V, sunL, uSunColor * uSunIntensity * (1.0 - shadow), albedo, metallic, rough);

    // Точечные.
    for (int i = 0; i < uNumPointLights; ++i) {
        vec3 tl = uPointLights[i].position - fragPos;
        float dist = length(tl);
        vec3 L = tl / max(dist, 1e-4);
        float att = 1.0 / (uPointLights[i].constant + uPointLights[i].linear * dist + uPointLights[i].quadratic * dist * dist);
        Lo += PbrContrib(N, V, L, uPointLights[i].color * uPointLights[i].intensity * att, albedo, metallic, rough);
    }
    // Прожекторы (конус).
    for (int i = 0; i < uNumSpotLights; ++i) {
        vec3 tl = uSpotLights[i].position - fragPos;
        float dist = length(tl);
        vec3 L = tl / max(dist, 1e-4);
        float theta = dot(L, normalize(-uSpotLights[i].direction));
        float eps = max(uSpotLights[i].cosInner - uSpotLights[i].cosOuter, 0.0001);
        float cone = clamp((theta - uSpotLights[i].cosOuter) / eps, 0.0, 1.0);
        if (cone <= 0.0) continue;
        float att = 1.0 / (uSpotLights[i].constant + uSpotLights[i].linear * dist + uSpotLights[i].quadratic * dist * dist);
        Lo += PbrContrib(N, V, L, uSpotLights[i].color * uSpotLights[i].intensity * att * cone, albedo, metallic, rough);
    }

    vec3 result = ambient + Lo;
    if (uFogEnabled) {
        float dist = length(uViewPos - fragPos);
        float f = clamp((uFogEnd - dist) / max(uFogEnd - uFogStart, 0.0001), 0.0, 1.0);
        result = mix(uFogColor, result, f);
    }
    return result;
}

// Совместимая обёртка: непрямой свет по умолчанию (GI-объём или полусферический
// ambient). Прежний контракт для путей без собственной лайтмапы.
vec3 ShadePBRao(vec3 N, vec3 fragPos, vec3 albedo, float metallic, float rough, float ao) {
    return ShadePBRgi(N, fragPos, albedo, metallic, rough, ao, DefaultIndirect(fragPos, N));
}

// Совместимая обёртка без AO (ao = 1.0) — для путей без ambient-occlusion карты.
vec3 ShadePBR(vec3 N, vec3 fragPos, vec3 albedo, float metallic, float rough) {
    return ShadePBRao(N, fragPos, albedo, metallic, rough, 1.0);
}
)GLSL";

} // namespace sage::render
