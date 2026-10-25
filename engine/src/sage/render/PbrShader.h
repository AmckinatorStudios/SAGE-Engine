#pragma once
#include <string>

#include "sage/core/StaticText.h"
#include "sage/render/PbrShaderLighting.h"
#include "sage/render/PbrShaderShading.h"

// ---------------------------------------------------------------------------
// Общий GLSL-блок физически-корректного освещения (PBR, metallic-roughness,
// Cook-Torrance GGX). Встраивается во ВСЕ шейдеры, освещающие геометрию
// (инстансный батч, текстурный PBR, скиннинг) — единственный источник правды о
// модели освещения, чтобы всё выглядело одинаково. НЕ содержит #version, in/out
// и main — их даёт конкретный шейдер; здесь только uniform'ы освещения (имена
// совпадают с UploadLighting/UploadShadowUniforms), BRDF-функции и ShadePBR().
// ---------------------------------------------------------------------------
namespace sage::render {

// Склейка частей — при компиляции (sage/core/StaticText.h).
inline constexpr auto kPbrSharedGlslStorage = sage::text::Concat(kPbrGlslLighting, kPbrGlslShading);
inline const char* kPbrSharedGlsl = kPbrSharedGlslStorage.data();


// ПОВТОР РАЗВЁРТКИ — В ВЕРШИННОМ ШЕЙДЕРЕ, ПО ГРАНИ.
//
// Режим «на метр» (MaterialRender::TilingMode::WorldSize) умножает повтор на
// размер объекта — но размер у каждой ГРАНИ свой: у куба 4 x 1 x 2 передняя
// грань тянется на 4 x 1, боковая на 2 x 1, верхняя на 4 x 2. Одно число на
// весь объект, посчитанное на процессоре, было верным лишь для одной стороны.
// Здесь пара осей выбирается по нормали вершины в пространстве модели (доли
// |n|, чтобы у скруглённых форм пары плавно перетекали), а масштаб — из самой
// матрицы модели. Копия на процессоре — sage::render::TilingFactor.
//
// Встраивается в вершинный шейдер, у которого есть uModel (или своя матрица).
inline const char* kUVTilingGlsl = R"GLSL(
// Повтор материала: одно число на обе оси или пара (BaseTiling).
uniform vec2 uUVScale;
// true — повтор «на метр»: умножается на размер грани в мире.
uniform bool uUVWorld;
vec2 TiledUV(vec2 uv, vec3 localNormal, mat4 model) {
    vec2 k = uUVScale;
    if (uUVWorld) {
        vec3 s = max(vec3(length(model[0].xyz), length(model[1].xyz), length(model[2].xyz)), vec3(0.0001));
        vec3 w = abs(localNormal);
        w /= max(w.x + w.y + w.z, 0.0001);
        k *= w.x * s.zy + w.y * s.xz + w.z * s.xy;
    }
    return uv * k;
}
)GLSL";

// ФРАГМЕНТНАЯ СТАДИЯ ТЕКСТУРНОГО PBR — ОДНА НА ВЕСЬ ДВИЖОК.
//
// Её собирают ДВА пути: статический текстурный меш (ecs/RenderBatch) и
// скелетная модель (render/SkinnedModel). И это не «удобно», а обязательно:
// пока у скина был свой урезанный фрагмент, анимированный персонаж и такой же
// статический меш с тем же материалом освещались по-разному — у скина не было
// ни карт металличности и шероховатости, ни нормалей, ни затенения, ни
// свечения, ни прозрачности. Разница между скином и статикой обязана быть
// ровно одна: КАК получается позиция и нормаль вершины. Всё, что дальше, —
// один и тот же код.
//
// Вход (его обязан дать вершинный шейдер): FragPos, Normal, TexCoords, TBN,
// vUV2. Выход — цвет с альфой материала.
inline std::string TexturedPbrFragSource() {
    return std::string(R"(#version 330 core
in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoords;
in mat3 TBN;
in vec2 vUV2;
layout(location = 0) out vec4 FragColor;
// Тонкий G-буфер: альбедо и нормаль в мире (см. rhi::RenderTargetDesc::GBuffer).
// Пишутся, только пока движок включил их запись: иначе вывод просто отбрасывается.
layout(location = 1) out vec4 oGAlbedo;
layout(location = 2) out vec4 oGNormal;

// Лайтмапа GI текущей сущности (uLightmapEnabled — сущность запечена).
uniform bool uLightmapEnabled;
uniform sampler2D uLightmap;

uniform vec3 uAlbedoFactor;
// Повтор текстуры приходит УЖЕ в TexCoords: его считает вершинный шейдер
// (kUVTilingGlsl), потому что зависит он от грани. Вторая развёртка
// (лайтмапа) повтора не знает и знать не должна — она уникальна на объект.
// Сдвиг развёртки (доли текстуры). Складывается ПОСЛЕ умножения на повтор:
// сдвиг задают в плитках («на треть плитки правее»), а не в долях грани, —
// иначе смысл числа менялся бы при каждой смене повтора.
uniform vec2 uUVOffset;
uniform float uMetallic;
uniform float uRoughness;
uniform sampler2D uAlbedoMap;
uniform bool uHasAlbedo;
uniform sampler2D uNormalMap;
uniform bool uHasNormal;
uniform sampler2D uMetallicMap;
uniform bool uHasMetallic;
uniform sampler2D uRoughnessMap;
uniform bool uHasRoughness;
uniform sampler2D uAOMap;
uniform bool uHasAO;
// ИЗ КАКОГО КАНАЛА карты брать значение. Маска, а не индекс: скалярное
// произведение работает в любом GLSL и не требует динамической индексации
// вектора.
//
// Зачем это вообще. glTF пакует в ОДНУ текстуру три величины: R — затенение,
// G — шероховатость, B — металличность. Разложить её на три отдельные картинки
// при загрузке значило бы держать в памяти и в кэше три копии карты 4K вместо
// одной — сотню мегабайт на модель на ровном месте. Читаем из одной, просто
// разными каналами. У материалов движка (.sagemat) карта у каждого своя, и
// маска у них R — то есть прежнее поведение.
uniform vec4 uMetallicMask;
uniform vec4 uRoughnessMask;
uniform vec4 uAOMask;
uniform float uOpacity;
// Порог отсечения по альфе (glTF alphaMode=MASK). Ноль — режима нет и альфа
// текстуры не смотрится вовсе: у обычного материала в ней бывает что угодно,
// и внезапно начать по ней отсекать значило бы продырявить готовые сцены.
uniform float uAlphaCutoff;
// Просвечивание (MaterialRender::Translucency): доля солнца, проходящая сквозь
// тонкую поверхность. Ноль — обычная поверхность. Программа общая на все
// материалы кадра, поэтому ставится ВСЕГДА, в том числе нулём.
uniform float uTranslucency;
// Материал без освещения (MaterialRender::Unlit): цвет как есть.
uniform bool uUnlit;
uniform vec3 uEmissive;
uniform sampler2D uEmissiveMap;
uniform bool uHasEmissive;
)") + kPbrSharedGlsl + R"(
void main() {
    vec2 uv = TexCoords + uUVOffset;
    vec4 base = vec4(uAlbedoFactor, 1.0);
    if (uHasAlbedo) base *= texture(uAlbedoMap, uv);
    // Отсечение по альфе — ДО всей остальной работы: отброшенный пиксель не
    // должен стоить ни выборок карт, ни освещения.
    if (uAlphaCutoff > 0.0 && base.a < uAlphaCutoff) discard;
    vec3 albedo = base.rgb;
    oGAlbedo = vec4(clamp(albedo, 0.0, 1.0), 1.0);
    oGNormal = vec4(normalize(gl_FrontFacing ? Normal : -Normal) * 0.5 + 0.5, 1.0);
    // БЕЗ ОСВЕЩЕНИЯ — раньше всего остального: ни карты нормалей, ни тени,
    // ни туман мультяшной заливке не нужны, и платить за них незачем.
    if (uUnlit && uShadingMode == 0) {
        vec3 glow = uEmissive;
        if (uHasEmissive) glow *= texture(uEmissiveMap, uv).rgb;
        FragColor = vec4(albedo + glow, uOpacity);
        return;
    }

    vec3 N = normalize(Normal);
    if (uHasNormal) {
        vec3 n = texture(uNormalMap, uv).rgb * 2.0 - 1.0;
        vec3 mapped = TBN * n;
        // ВЫРОЖДЕННЫЙ БАЗИС НЕ ИМЕЕТ ПРАВА ГАСИТЬ ПИКСЕЛЬ. normalize нулевого
        // вектора даёт NaN, а NaN в цвете — чёрная точка, которая переживает
        // всё: и освещение, и пост-обработку. Базис вырождается там, где
        // развёртка сходится в точку или касательные соседних вершин смотрят
        // в разные стороны (шов), — и на модели это выглядит как россыпь
        // чёрных крапин, которую не объяснить ни светом, ни материалом.
        if (dot(mapped, mapped) > 1e-8) N = normalize(mapped);
    }
    // ЗАДНЯЯ ГРАНЬ СМОТРИТ НАЗАД. У двустороннего материала (cull=none, а у
    // моделей со сканов и выгрузок это ПОЧТИ ВСЕГДА так) в кадр попадает
    // изнанка поверхности, а нормаль у неё осталась от лицевой стороны — то
    // есть направлена ОТ зрителя. Свет на такую грань не попадает по
    // построению: скалярное произведение с направлением на источник
    // отрицательное, и поверхность выходит ЧЁРНОЙ при полном освещении вокруг.
    //
    // Односторонним материалам разворот не мешает: их задние грани отсекаются
    // и до сюда не доходят.
    if (!gl_FrontFacing) N = -N;
    oGNormal = vec4(N * 0.5 + 0.5, 1.0);   // уже с картой нормалей
    gShadowNormal = normalize(gl_FrontFacing ? Normal : -Normal);
    // metallic/roughness/ao — из своих карт (канал задаёт маска) × фактор,
    // иначе только фактор.
    float metallic = uMetallic;
    if (uHasMetallic) metallic *= dot(texture(uMetallicMap, uv), uMetallicMask);
    float rough = uRoughness;
    if (uHasRoughness) rough *= dot(texture(uRoughnessMap, uv), uRoughnessMask);
    float ao = uHasAO ? dot(texture(uAOMap, uv), uAOMask) : 1.0;

    if (uShadingMode != 0) {
        vec4 dbg;
        vec3 dbgEmissive = uEmissive;
        if (uHasEmissive) dbgEmissive *= texture(uEmissiveMap, uv).rgb;
        if (DebugShade(uShadingMode, N, FragPos, albedo, metallic, rough, ao, dbgEmissive,
                       CalcSunShadow(FragPos, N, normalize(-uSunDir)), dbg)) {
            FragColor = vec4(dbg.rgb, uOpacity);
            return;
        }
    }
    vec3 indirect = uLightmapEnabled ? texture(uLightmap, vUV2).rgb
                                     : DefaultIndirect(FragPos, N);
    // НЕБО СКВОЗЬ ЛИСТ. Тонкая поверхность получает окружающий свет с обеих
    // сторон: лист, повёрнутый к земле, всё равно светится небом над собой.
    // Без этого нижние листья кроны брали только тёмный «свет земли» и крона
    // выходила пятнистой — тёмные провалы там, где у живого дерева мягкая
    // зелень.
    if (uTranslucency > 0.0 && !uLightmapEnabled)
        indirect += DefaultIndirect(FragPos, -N) * uTranslucency;
    vec3 emissive = uEmissive;
    if (uHasEmissive) emissive *= texture(uEmissiveMap, uv).rgb;
    vec3 lit = ShadePBRgi(N, FragPos, albedo, metallic, rough, ao, indirect);
    // ПРОСВЕЧИВАНИЕ ЛИСТВЫ. Две добавки к солнцу, обе только для тонких
    // поверхностей (uTranslucency > 0):
    //   • свет С ОБРАТНОЙ СТОРОНЫ: лист, повёрнутый к солнцу изнанкой,
    //     пропускает его насквозь — окрашенным в свой цвет. Без этого такой
    //     лист освещался одним окружением, и крона выходила тёмной и пятнистой;
    //   • «обёрнутый» диффуз у границы тени: у тонкой поверхности свет не
    //     обрывается на терминаторе, а мягко сходит на нет.
    // Тень солнца учитывается: лист внутри кроны, до которого солнце не
    // доходит, светиться не должен — иначе крона стала бы плоской.
    if (uTranslucency > 0.0) {
        vec3 L = normalize(-uSunDir);
        float ndl = dot(N, L);
        float back = max(-ndl, 0.0);
        float wrap = max((ndl + 0.5) / 1.5, 0.0) - max(ndl, 0.0);
        float vis = uShadowsEnabled ? 1.0 - SunShadow(FragPos, ndl < 0.0 ? -gShadowNormal : gShadowNormal, L) : 1.0;
        // Та же калибровка, что у PbrContrib: интенсивность 1 — лист под
        // солнцем принимает весь свет (π сокращается с π Ламберта).
        lit += albedo * uSunColor * uSunIntensity * uTranslucency * (back + wrap) * vis;
    }
    FragColor = vec4(emissive + lit, uOpacity);
}
)";
}

} // namespace sage::render
