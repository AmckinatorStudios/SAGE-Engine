#pragma once

// Часть общего GLSL-блока PBR (см. PbrShader.h: kPbrSharedGlsl собирается из
// частей при компиляции, байт в байт). Разделён по смыслу, чтобы правка теней
// не терялась в тысяче строк вместе с туманом и кластерным светом.
namespace sage::render {

// Туман, окружение, запечённый GI, отражения, площадные и кластерные источники, ShadePBR.
inline constexpr char kPbrGlslShading[] = R"GLSL(// СВЕТ НЕБА ОТКАЛИБРОВАН ТАК ЖЕ, КАК СОЛНЦЕ (×π, см. PbrContrib).
//
// Когда солнце получило честный множитель π, небо осталось прежним — и тень
// провалилась в π раз: сила окружающего света 0.3, подобранная под старое
// солнце, стала означать «тень почти чёрная». Отсюда «непонятные тёмные
// участки» под кроной и на изнанке веток. Тот же множитель здесь возвращает
// прежнее соотношение неба и солнца: 0.3 значит то же, что значило всегда.
// ТУМАН. Высотный — экспоненциальный высотный туман: плотность
// d(h) = Density * exp(-Falloff * (h - BaseHeight)), и её интеграл вдоль луча
// от камеры (с отступом Start) до точки берётся аналитически. Против солнца
// дымка светится его цветом — узнаваемый «воздух» рассвета и заката.
vec3 ApplyFog(vec3 color, vec3 fragPos) {
    vec3 ray = fragPos - uViewPos;
    float dist = length(ray);
    if (uFogMode == 1) {
        vec3 V = ray / max(dist, 1e-4);
        float start = min(uFogStart, dist);
        vec3 from = uViewPos + V * start;
        float len = dist - start;
        float dy = V.y * len;
        float densityAtStart = uFogDensity * exp(-uFogFalloff * (from.y - uFogHeight));
        float fall = uFogFalloff * dy;
        float shape = abs(fall) > 1e-4 ? (1.0 - exp(-fall)) / fall : 1.0;
        float amount = min(1.0 - exp(-densityAtStart * len * shape), uFogMaxOpacity);
        float sun = pow(max(dot(V, normalize(-uSunDir)), 0.0), uFogSunExponent);
        vec3 inscatter = uFogColor + uSunColor * uSunIntensity * uFogSunScatter * sun;
        return mix(color, inscatter, clamp(amount, 0.0, 1.0));
    }
    float f = clamp((uFogEnd - dist) / max(uFogEnd - uFogStart, 0.0001), 0.0, 1.0);
    return mix(uFogColor, color, f);
}

vec3 CalcHemisphereAmbient(vec3 normal) {
    float w = normal.y * 0.5 + 0.5;
    return mix(uAmbientGround, uAmbientSky, w) * uAmbientStrength * PI;
}

// Нормаль, по которой ищется ТЕНЬ, — нормаль вершины, без карты нормалей.
//
// Карта нормалей — рельеф в пределах текселя, а карта теней знает только
// геометрию. Отступ выборки тени по «рельефной» нормали скачет от пикселя к
// пикселю, и вместе с ним скачет тень: кора и листья покрываются рябью и
// тёмными пятнами, которых нет ни на одном предмете вокруг. Шейдер, у которого
// есть обе нормали, кладёт сюда нормаль вершины; нулевой вектор — «такой нет»,
// тогда берётся нормаль освещения.
vec3 gShadowNormal = vec3(0.0);
vec3 ShadowNormal(vec3 N) {
    return dot(gShadowNormal, gShadowNormal) > 0.5 ? gShadowNormal : N;
}

// --- Запечённое GI (см. sage/gi): объём световых проб (L1 SH) -------------
// Одна 3D-текстура, по глубине три слоя — R, G, B; в текселе vec4 гармоник
// канала (c0, cx, cy, cz). Семплится per-pixel по мировой позиции: аппаратная
// трилинейная интерполяция между пробами. Значение — «непрямая освещённость/π»
// (та же конвенция, что CalcHemisphereAmbient) — прямой множитель albedo.
uniform bool uGIVolumeEnabled;
uniform sampler3D uGIVol;
uniform vec3 uGIVolMin;       // мировая позиция пробы (0,0,0)
uniform vec3 uGIVolInvExtent; // 1 / (CellSize * (Dims-1))
uniform vec3 uGIVolDims;      // число проб по осям (одного слоя)

vec3 EvalGIVolume(vec3 worldPos, vec3 n) {
    vec3 uvw = clamp((worldPos - uGIVolMin) * uGIVolInvExtent, 0.0, 1.0);
    uvw = (uvw * (uGIVolDims - 1.0) + 0.5) / uGIVolDims; // узлы -> центры текселей
    // z лежит между центрами крайних срезов слоя, поэтому интерполяция не
    // заходит в соседний слой: k-й слой — это (z + k) / 3 общей глубины.
    vec4 shR = texture(uGIVol, vec3(uvw.xy, uvw.z / 3.0));
    vec4 shG = texture(uGIVol, vec3(uvw.xy, (uvw.z + 1.0) / 3.0));
    vec4 shB = texture(uGIVol, vec3(uvw.xy, (uvw.z + 2.0) / 3.0));
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

// --- Отражение окружения (specular IBL) ------------------------------------
//
// До этого непрямой свет был ТОЛЬКО диффузным: ambient = indirect * albedo.
// Для металла это неверно вдвойне — у него нет диффузной составляющей вовсе, а
// весь его вид и есть отражение окружения. Хромовый шар в такой модели выходил
// плоским серым пятном, и никакая настройка света это не чинила.
//
// Считается по «split-sum» приближению Karis: интеграл освещения разбивается на
// (1) предварительно размытое окружение, выбранное по шероховатости, и
// (2) отклик BRDF, зависящий только от угла и шероховатости. Второй множитель
// обычно берут из запечённой LUT-текстуры; здесь вместо неё аналитическая
// аппроксимация Lazarov — она укладывается в несколько операций, не занимает
// текстурный юнит (их в GL 3.3 и так впритык из-за четырёх карт теней) и не
// требует шага запекания, без которого движок был бы неработоспособен «из
// коробки».
vec2 EnvBRDFApprox(float NdotV, float rough) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// Направление выборки куба с поправкой на коробку влияния зонда.
vec3 ParallaxCorrect(vec3 R, vec3 worldPos) {
    if (!uEnvBoxParallax) return R;
    // Пересечение луча с параллелепипедом (slab-метод). Луч всегда изнутри
    // коробки, поэтому нужен только ДАЛЬНИЙ корень по каждой оси.
    vec3 inv = 1.0 / max(abs(R), vec3(1e-5)) * sign(R + vec3(1e-9));
    vec3 tMax = (uEnvBoxMax - worldPos) * inv;
    vec3 tMin = (uEnvBoxMin - worldPos) * inv;
    vec3 t = max(tMax, tMin);
    float dist = min(min(t.x, t.y), t.z);
    if (dist <= 0.0) return R;      // точка вне коробки — поправлять нечего
    return normalize((worldPos + R * dist) - uEnvProbePos);
}

// Плоское отражение читается по ЭКРАННОЙ позиции: отражённая сцена снята той же
// камерой, только зеркально, поэтому пиксель отражения лежит там же, где сам
// фрагмент. Смещение uv по нормали даёт рябь на воде.
vec3 SamplePlanar(vec2 offset) {
    if (!uPlanarEnabled) return vec3(0.0);
    vec2 uv = gl_FragCoord.xy * uScreenTexel + offset;
    return texture(uPlanarMap, clamp(uv, vec2(0.001), vec2(0.999))).rgb;
}

// Отражённое окружение для фрагмента. Возвращает уже готовый цвет зеркальной
// составляющей непрямого света.
//
// Источников два, и они складываются по УБЫВАНИЮ точности: плоское отражение —
// настоящая геометрия, отрисованная зеркально, но существует только для своей
// плоскости; куб знает всё вокруг, включая то, что за камерой, но снят из одной
// точки. Куб — фундамент, плоское подставляется поверх там, где оно есть.
vec3 SpecularIBL(vec3 N, vec3 V, vec3 albedo, float metallic, float rough, float ao,
                 vec3 worldPos, vec3 planar, float planarWeight) {
    float NdotV = max(dot(N, V), 1e-4);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec2 ab = EnvBRDFApprox(NdotV, rough);
    vec3 brdf = F0 * ab.x + ab.y;

    vec3 env = vec3(0.0);
    if (uEnvEnabled) {
        vec3 Rw = reflect(-V, N);
        vec3 R = ParallaxCorrect(Rw, worldPos);
        env = textureLod(uEnvMap, R, rough * uEnvMaxLod).rgb * uEnvIntensity;
        // Затенение горизонтом. Карта нормалей наклоняет N, и отражённый луч
        // уходит ПОД поверхность: из куба тогда читается то, что под полом, и
        // на рельефе вспыхивают чужие пятна. Луч, ушедший за геометрическую
        // нормаль, гасится (Jimenez/Lagarde).
        vec3 Ng = ShadowNormal(N);
        float horizon = clamp(1.0 + 1.3 * dot(Rw, Ng), 0.0, 1.0);
        env *= horizon * horizon;
    }
    // Плоское — поверх куба: если оно есть, оно точнее.
    env = mix(env, planar, planarWeight);

    // Затенение отражения. Полный ambient occlusion гасить отражение не должен
    // (щель отражает небо не хуже открытой поверхности), но и не гасить нельзя:
    // тогда в углах загорается свет, которого там нет. Компромисс — гасим
    // частично, тем сильнее, чем поверхность шероховатее: у зеркала отражение
    // приходит из одного направления и щелью почти не перекрывается.
    float specAO = mix(1.0, ao, rough);
    return env * brdf * specAO;
}

// Вклад одного источника (Cook-Torrance): N — нормаль, V — к камере, L — к свету,
// radiance — цвет*интенсивность источника (с затуханием/тенью, посчитанными выше).
//
// ИНТЕНСИВНОСТЬ 1 — ЭТО «БЕЛАЯ ПОВЕРХНОСТЬ ПОД СОЛНЦЕМ БЕЛАЯ» (общепринятая
// калибровка): освещённость источника равна π·интенсивность, и π BRDF Ламберта
// сокращается. Раньше этого множителя не было, и полное солнце давало белой
// стене треть её цвета; тусклость прятала ошибка гаммы (текстуры читались без
// перевода из sRGB и выходили светлее, чем нарисованы). Исправив одно, нельзя
// было оставить другое: текстурированная сцена стала бы втрое темнее.
vec3 PbrContrib(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic, float rough) {
    vec3 H = normalize(V + L);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float NDF = DistributionGGX(N, H, rough);
    float G = GeometrySmith(N, V, L, rough);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 spec = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0), 1e-4);
    vec3 kd = (vec3(1.0) - F) * (1.0 - metallic);
    float NdotL = max(dot(N, L), 0.0);
    return (kd * albedo / PI + spec) * radiance * NdotL * PI;
}

// Мягкий край дальности: последняя пятая часть Range гаснет до нуля. Затухание
// по формуле Range к краю уже ~1% — край не виден, — но без него лампа светила
// бы бесконечно, и кластерам (render/LightClusters.h), режущим её шаром Range,
// досталась бы ступенька света на границе шара.
float RangeFade(float dist, float range) {
    return clamp((range - dist) / max(range * 0.2, 1e-3), 0.0, 1.0);
}

// Вклад точечного источника — одна функция на массивы и на кластеры.
vec3 PointLightContrib(PointLight pl, float range, vec3 N, vec3 V, vec3 fragPos, vec3 shN, float localRot,
                       vec3 albedo, float metallic, float rough) {
    vec3 tl = pl.position - fragPos;
    float dist = length(tl);
    vec3 L = tl / max(dist, 1e-4);
    float ndl = dot(N, L);
    float att = 1.0 / (pl.constant + pl.linear * dist + pl.quadratic * dist * dist);
    if (range > 0.0) att *= RangeFade(dist, range);
    // Тень считается только для граней, ОБРАЩЁННЫХ к лампе: отвёрнутая
    // грань всё равно получает от неё ноль (множитель max(dot(N,L),0)), а
    // выборка стоит восьми чтений атласа.
    float sh = 0.0;
    if (uLocalShadowsEnabled && pl.shadowSlot >= 0 && ndl > 0.0)
        sh = PointShadow(pl.shadowSlot, fragPos, shN, pl.position, ndl, localRot);
    return PbrContrib(N, V, L, pl.color * pl.intensity * att * (1.0 - sh), albedo, metallic, rough);
}

// Вклад прожектора (конус).
vec3 SpotLightContrib(SpotLight sl, float range, vec3 N, vec3 V, vec3 fragPos, vec3 shN, float localRot,
                      vec3 albedo, float metallic, float rough) {
    vec3 tl = sl.position - fragPos;
    float dist = length(tl);
    vec3 L = tl / max(dist, 1e-4);
    float theta = dot(L, normalize(-sl.direction));
    float eps = max(sl.cosInner - sl.cosOuter, 0.0001);
    float cone = clamp((theta - sl.cosOuter) / eps, 0.0, 1.0);
    if (cone <= 0.0) return vec3(0.0);
    float ndl = dot(N, L);
    float att = 1.0 / (sl.constant + sl.linear * dist + sl.quadratic * dist * dist);
    if (range > 0.0) att *= RangeFade(dist, range);
    float sh = 0.0;
    if (uLocalShadowsEnabled && sl.shadowSlot >= 0 && ndl > 0.0)
        sh = SpotShadow(sl.shadowSlot, fragPos, shN, ndl, dist, localRot);
    return PbrContrib(N, V, L, sl.color * sl.intensity * att * cone * (1.0 - sh), albedo, metallic, rough);
}

// --- Площадной (прямоугольный) источник ------------------------------------
//
// Рассеянный свет — ТОЧНО, формулой Ламберта для многоугольника: доля
// полусферы, которую панель закрывает из точки (форм-фактор). Отсюда и мягкость
// у края панели, и закон площади: панель вдвое больше светит вдвое сильнее, а
// не так же, как лампа в её центре. Копия на процессоре —
// sage::render::AreaFormFactor (по ней проверяется знак обхода).
//
// Блик — «представительной точкой» (Karis 2013): точка панели, ближайшая к
// отражённому лучу, и лепесток GGX, расширенный по видимому размеру панели с
// нормировкой энергии. Отсюда прямоугольный блик на глянце вместо точки.
float AreaEdge(vec3 a, vec3 b, vec3 N) {
    float theta = acos(clamp(dot(a, b), -1.0, 1.0));
    vec3 g = cross(a, b);
    float len = length(g);
    return len > 1e-6 ? theta * dot(g / len, N) : 0.0;
}
float AreaFormFactor(vec3 P, vec3 N, vec3 c0, vec3 c1, vec3 c2, vec3 c3) {
    vec3 v0 = normalize(c0 - P), v1 = normalize(c1 - P), v2 = normalize(c2 - P), v3 = normalize(c3 - P);
    float sum = AreaEdge(v0, v1, N) + AreaEdge(v1, v2, N) + AreaEdge(v2, v3, N) + AreaEdge(v3, v0, N);
    if (dot(cross(c1 - c0, c2 - c0), P - c0) > 0.0) sum = -sum;
    return max(sum / (2.0 * PI), 0.0);
}
vec3 AreaLightContrib(vec3 pos, vec3 dir, vec3 right, vec2 halfSize, float range, vec3 radiance,
                      vec3 N, vec3 V, vec3 P, vec3 albedo, float metallic, float rough) {
    vec3 toP = P - pos;
    float side = dot(toP, dir);
    if (side <= 0.0) return vec3(0.0);   // за панелью — темно: она светит одной стороной
    float fade = RangeFade(length(toP), range);
    if (fade <= 0.0) return vec3(0.0);
    vec3 up = normalize(cross(dir, right));
    vec3 c0 = pos + right * halfSize.x + up * halfSize.y;
    vec3 c1 = pos - right * halfSize.x + up * halfSize.y;
    vec3 c2 = pos - right * halfSize.x - up * halfSize.y;
    vec3 c3 = pos + right * halfSize.x - up * halfSize.y;
    float ff = AreaFormFactor(P, N, c0, c1, c2, c3);

    // Представительная точка блика: где отражённый луч пересекает плоскость
    // панели, зажатое в её прямоугольник (луч мимо плоскости — ближайшая
    // к точке P точка панели).
    vec3 R = reflect(-V, N);
    float rd = dot(R, -dir);
    vec3 hit = rd > 1e-4 ? P + R * (side / rd) : P - dir * side;
    vec3 local = hit - pos;
    vec3 Lp = pos + right * clamp(dot(local, right), -halfSize.x, halfSize.x)
                  + up * clamp(dot(local, up), -halfSize.y, halfSize.y);
    vec3 Lv = Lp - P;
    float dist2 = max(dot(Lv, Lv), 1e-4);
    vec3 L = Lv * inversesqrt(dist2);
    float NdotL = max(dot(N, L), 0.0);
    // Расширение лепестка по угловому размеру панели и нормировка энергии:
    // большой источник даёт широкий, но не более яркий блик.
    float a = max(rough * rough, 0.002);
    float srcRadius = sqrt(halfSize.x * halfSize.y);
    float a2 = clamp(a + srcRadius / (3.0 * sqrt(dist2)), 0.0, 1.0);
    float norm = (a / a2) * (a / a2);
    vec3 H = normalize(V + L);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float NDF = DistributionGGX(N, H, sqrt(a2));
    float G = GeometrySmith(N, V, L, rough);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 specBrdf = (NDF * G * F) / max(4.0 * max(dot(N, V), 0.0) * NdotL, 1e-4);
    // Телесный угол панели из точки (ограничен полусферой): то, что у точечной
    // лампы делает 1/d².
    float area = 4.0 * halfSize.x * halfSize.y;
    float omega = min(area * max(dot(-L, dir), 0.0) / dist2, PI);
    vec3 kd = (vec3(1.0) - F) * (1.0 - metallic);
    return (kd * albedo * ff + specBrdf * norm * NdotL * omega) * radiance * fade;
}

// --- Кластерный свет (см. render/LightClusters.h) --------------------------
// Одна текстура данных: источники по 6 текселей, ячейки (смещение, число),
// список номеров по четыре в текселе. Ширина 1024.
uniform bool uClustered;
uniform sampler2D uLightGrid;
uniform int uClusterGridBase;
uniform int uClusterIndexBase;
uniform mat4 uClusterView;
uniform mat4 uClusterProj;
uniform vec4 uClusterDepth;   // ближняя, дальняя, срезов на единицу, геометрические ли срезы
vec4 LightTexel(int t) { return texelFetch(uLightGrid, ivec2(t & 1023, t >> 10), 0); }
int ClusterCell(vec3 fragPos) {
    vec4 v = uClusterView * vec4(fragPos, 1.0);
    vec4 clip = uClusterProj * v;
    vec2 ndc = clip.xy / max(abs(clip.w), 1e-6);
    int x = clamp(int(floor((ndc.x * 0.5 + 0.5) * 16.0)), 0, 15);
    int y = clamp(int(floor((ndc.y * 0.5 + 0.5) * 9.0)), 0, 8);
    float d = -v.z;
    float t = uClusterDepth.w > 0.5 ? log(max(d, uClusterDepth.x) / uClusterDepth.x) * uClusterDepth.z
                                    : (d - uClusterDepth.x) * uClusterDepth.z;
    int z = clamp(int(floor(t)), 0, 23);
    return x + y * 16 + z * 144;
}
vec3 ClusteredLights(vec3 N, vec3 V, vec3 fragPos, vec3 shN, float localRot,
                     vec3 albedo, float metallic, float rough) {
    vec4 cell = LightTexel(uClusterGridBase + ClusterCell(fragPos));
    int offset = int(cell.x), count = int(cell.y);
    vec3 Lo = vec3(0.0);
    for (int i = 0; i < count; ++i) {
        int e = offset + i;
        vec4 quad = LightTexel(uClusterIndexBase + (e >> 2));
        int k = e & 3;
        int li = int(k == 0 ? quad.x : k == 1 ? quad.y : k == 2 ? quad.z : quad.w);
        int b = li * 6;
        vec4 t0 = LightTexel(b), t1 = LightTexel(b + 1), t2 = LightTexel(b + 2);
        vec4 t3 = LightTexel(b + 3), t4 = LightTexel(b + 4), t5 = LightTexel(b + 5);
        int type = int(t0.w + 0.5);
        if (type == 0) {
            PointLight pl;
            pl.position = t0.xyz; pl.color = t1.rgb; pl.intensity = t1.a;
            pl.constant = t2.x; pl.linear = t2.y; pl.quadratic = t2.z; pl.shadowSlot = int(t2.w);
            Lo += PointLightContrib(pl, t5.z, N, V, fragPos, shN, localRot, albedo, metallic, rough);
        } else if (type == 1) {
            SpotLight sl;
            sl.position = t0.xyz; sl.direction = t3.xyz; sl.color = t1.rgb; sl.intensity = t1.a;
            sl.constant = t2.x; sl.linear = t2.y; sl.quadratic = t2.z;
            sl.cosInner = t3.w; sl.cosOuter = t4.w; sl.shadowSlot = int(t2.w);
            Lo += SpotLightContrib(sl, t5.z, N, V, fragPos, shN, localRot, albedo, metallic, rough);
        } else {
            Lo += AreaLightContrib(t0.xyz, t3.xyz, t4.xyz, t5.xy, t5.z, t1.rgb * t1.a,
                                   N, V, fragPos, albedo, metallic, rough);
        }
    }
    return Lo;
}

// Полное PBR-освещение фрагмента с ЯВНОЙ непрямой составляющей (indirect —
// «освещённость/π»: лайтмапа, GI-объём или полусферический ambient; умножает
// albedo и ao). Прямой свет — realtime: солнце (с тенью) + точечные +
// прожекторы + площадные + туман. shadingMode 1/2 обрабатывает вызывающий main.
vec3 ShadePBRplanar(vec3 N, vec3 fragPos, vec3 albedo, float metallic, float rough, float ao,
                    vec3 indirect, vec3 planar, float planarWeight) {
    vec3 V = normalize(uViewPos - fragPos);

    // Диффузный непрямой свет теперь берёт только ту долю, что не ушла в
    // отражение, и у металла её нет вовсе. Раньше металл получал полный
    // диффузный ambient — то есть светился как гипс, и одновременно не отражал
    // ничего. Оба конца этой ошибки чинятся здесь и в SpecularIBL.
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 kD = (vec3(1.0) - FresnelSchlickRoughness(max(dot(N, V), 0.0), F0, rough)) *
              (1.0 - metallic);
    vec3 ambient = kD * indirect * albedo * ao;
    ambient += SpecularIBL(N, V, albedo, metallic, rough, ao, fragPos, planar, planarWeight);
    vec3 Lo = vec3(0.0);

    // Солнце (направленное) + PCF-тень.
    vec3 sunL = normalize(-uSunDir);
    vec3 shN = ShadowNormal(N);
    float shadow = uShadowsEnabled ? SunShadow(fragPos, shN, sunL) : 0.0;
    Lo += PbrContrib(N, V, sunL, uSunColor * uSunIntensity * (1.0 - shadow), albedo, metallic, rough);

    // Поворот диска выборки считается ОДИН раз на фрагмент и раздаётся всем
    // источникам: он зависит только от экранных координат.
    float localRot = ShadowRotation();

    // Локальные источники: кластерный список ячейки, если его построили для
    // этого вида, иначе — прежние массивы (свои шейдеры, превью, тесты).
    if (uClustered) {
        Lo += ClusteredLights(N, V, fragPos, shN, localRot, albedo, metallic, rough);
    } else {
        for (int i = 0; i < uNumPointLights; ++i)
            Lo += PointLightContrib(uPointLights[i], 0.0, N, V, fragPos, shN, localRot, albedo, metallic, rough);
        for (int i = 0; i < uNumSpotLights; ++i)
            Lo += SpotLightContrib(uSpotLights[i], 0.0, N, V, fragPos, shN, localRot, albedo, metallic, rough);
    }

    vec3 result = ambient + Lo;
    if (uFogEnabled) result = ApplyFog(result, fragPos);
    return result;
}

// Обычный путь: без плоского отражения. Отдельной обёрткой, чтобы «зеркало»
// оставалось осознанным выбором материала, а не тем, что случайно включилось.
vec3 ShadePBRgi(vec3 N, vec3 fragPos, vec3 albedo, float metallic, float rough, float ao, vec3 indirect) {
    return ShadePBRplanar(N, fragPos, albedo, metallic, rough, ao, indirect, vec3(0.0), 0.0);
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
