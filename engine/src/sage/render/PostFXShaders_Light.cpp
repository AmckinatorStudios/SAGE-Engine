#include "sage/render/PostFXDetail.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include <glm/gtc/matrix_inverse.hpp>

#include "sage/core/Log.h"
#include "sage/core/Profiler.h"
#include "sage/render/LensFlare.h"

using namespace sage::rhi;

// Шейдеры освещения и оптики пост-обработки (см. PostFXDetail.h).
namespace sage::render::postfx {

// ============================================================================
//  Встроенные шейдеры пост-обработки (лениво, не уничтожаются — как остальные
//  встроенные шейдеры движка: function-local static с деструктором Shader снёс
//  бы GL-программу уже после разрушения контекста).
// ============================================================================


// Полноэкранный треугольник из gl_VertexID (буфер вершин не нужен).
const char* kFsVert = R"(#version 330 core
out vec2 vUV;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// --- SSAO: реконструкция позиций/нормалей из глубины + полусферическая выборка ---
const char* kSsaoFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uDepth;
uniform mat4 uProj;
uniform mat4 uInvProj;
uniform float uRadius;
uniform vec2 uTexel;   // 1/размер кадра — по нему берутся соседи для нормали

vec3 ViewPos(vec2 uv) {
    float d = texture(uDepth, uv).r;
    vec4 c = uInvProj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return c.xyz / c.w;
}

// Нормаль из глубины по ЛУЧШИМ соседям, а не по производным.
//
// Раньше здесь стояло normalize(cross(dFdx(P), dFdy(P))). На ровной
// поверхности это верно, а на СИЛУЭТЕ — там, где в соседнем пикселе лежит
// объект в трёх метрах позади, — производная берётся между двумя разными
// поверхностями, и «нормаль» получается случайной. Дальше по этой нормали
// строится полусфера выборки, и она смотрит куда попало: вдоль каждого контура
// в кадре идёт кайма из шума. Ровно это и читается как «АО грязный» — грязь
// собирается не по площадям, а по краям предметов.
//
// Лечится тем, что из двух соседей по каждой оси берётся БЛИЖЕ ПО ГЛУБИНЕ к
// центральному: он почти наверняка лежит на той же поверхности, а тот, что
// перепрыгнул на другой объект, отбрасывается.
vec3 NormalFromDepth(vec2 uv, vec3 P) {
    vec3 l = ViewPos(uv - vec2(uTexel.x, 0.0));
    vec3 r = ViewPos(uv + vec2(uTexel.x, 0.0));
    vec3 d = ViewPos(uv - vec2(0.0, uTexel.y));
    vec3 u = ViewPos(uv + vec2(0.0, uTexel.y));
    vec3 dx = (abs(l.z - P.z) < abs(r.z - P.z)) ? (P - l) : (r - P);
    vec3 dy = (abs(d.z - P.z) < abs(u.z - P.z)) ? (P - d) : (u - P);
    vec3 n = cross(dx, dy);
    float len2 = dot(n, n);
    return len2 > 1e-12 ? n * inversesqrt(len2) : vec3(0.0, 0.0, 1.0);
}

void main() {
    float d = texture(uDepth, vUV).r;
    if (d >= 1.0) { FragColor = vec4(1.0); return; } // фон — без затенения

    vec3 P = ViewPos(vUV);
    vec3 N = NormalFromDepth(vUV, P);
    if (dot(N, -P) < 0.0) N = -N; // ориентируем к камере (камера в начале координат)

    // Пер-пиксельный угол дизеринга (interleaved gradient noise).
    float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float ang = ign * 6.2831853;
    float ca = cos(ang), sa = sin(ang);

    const int N_SAMPLES = 16;
    float occ = 0.0;
    for (int i = 0; i < N_SAMPLES; ++i) {
        // Детерминированное полусферическое направление (спиральное распределение).
        float fi = (float(i) + 0.5) / float(N_SAMPLES);
        float phi = fi * 6.2831853 * 4.0;
        float sinT = sqrt(fi);
        vec3 dir = vec3(cos(phi) * sinT, sin(phi) * sinT, sqrt(1.0 - fi));
        dir.xy = vec2(dir.x * ca - dir.y * sa, dir.x * sa + dir.y * ca); // дизеринг
        if (dot(dir, N) < 0.0) dir = -dir;                              // в полусферу N

        float scale = uRadius * (0.1 + 0.9 * fi * fi); // сэмплы гуще у поверхности
        vec3 sp = P + dir * scale;

        vec4 off = uProj * vec4(sp, 1.0);
        off.xyz /= off.w;
        vec2 suv = off.xy * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;

        float sampleZ = ViewPos(suv).z; // реальная глубина сцены в этой точке экрана
        // Затенён, если реальная поверхность ближе к камере, чем сэмпл (z больше).
        //
        // Допуск ПРОПОРЦИОНАЛЕН радиусу, а не постоянные два сантиметра. Радиус
        // задаётся в метрах и его правят: при 0.1 м фиксированный допуск съедал
        // пятую часть проб (мелкие щели переставали затеняться), при 3 м не
        // спасал от самозатенения на пологих поверхностях — те самые разводы,
        // которые видно на полу.
        //
        // И РАСТЁТ С РАССТОЯНИЕМ. Это вторая половина той же беды, и без неё
        // первая лечит только ближний план. Глубина хранится с конечной
        // точностью, а нормаль здесь восстанавливается ИЗ НЕЁ разностью
        // соседних пикселей: чем дальше поверхность, тем крупнее шаг глубины на
        // пиксель и тем сильнее дрожит нормаль. На большом полу под пологим
        // углом это давало чёрную рябь, густеющую к горизонту, — её видно и в
        // редакторе, и в собранной игре, а на обложке шаблона она выглядит как
        // сломанный движок. Постоянный допуск покрыть этого не может: у пола в
        // двух метрах и в сорока разная цена одного бита глубины.
        float bias = max(uRadius * 0.02, 0.005) + abs(P.z) * 0.0025;
        float rangeCheck = smoothstep(0.0, 1.0, uRadius / max(abs(P.z - sampleZ), 1e-4));
        occ += (sampleZ >= sp.z + bias ? 1.0 : 0.0) * rangeCheck;
    }
    float ao = 1.0 - occ / float(N_SAMPLES);
    // Линейная глубина в альфу — по ней размытие ниже отличает свою поверхность
    // от чужой. Отдельный проход ради этого не нужен: канал уже есть и пустует.
    FragColor = vec4(vec3(ao), -P.z);
}
)";

// Размытие AO с УЧЁТОМ ГЛУБИНЫ (bilateral), крестом 4+4.
//
// Что было не так с простым боксом. Во-первых, он усреднял через силуэты:
// затенение из-под ножки стула размазывалось на стену в трёх метрах позади, и
// вокруг каждого предмета появлялся серый ореол — второй по заметности дефект
// после шума. Во-вторых, окно `for (x = -2; x < 2)` даёт смещения -2,-1,0,1 —
// оно НЕ СИММЕТРИЧНО, и всё изображение AO уезжало на полпикселя вверх-влево.
// На градиенте это видно как тонкая тёмная кайма с одной стороны предмета и
// светлая с другой.
//
// Разделённый крест (сначала по горизонтали, потом по вертикали) даёт то же
// сглаживание за 9 выборок вместо 16 — при этом отбрасывая соседей, чья
// глубина далеко от центральной.
const char* kAoBlurFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uAO;
uniform vec2 uTexel;
uniform vec2 uDirection;   // (1,0) — горизонтальный проход, (0,1) — вертикальный
uniform float uStrength;       // во что возводится затенение (см. звено «Ambient Occlusion»)
uniform int uApplyStrength;    // 1 — применять; 0 — не трогать (горизонтальный проход)

void main() {
    // Затенение в R, линейная глубина в ALPHA (см. проход SSAO выше). Читать
    // надо именно .ra: в .g лежит та же величина затенения, и фильтр по ней
    // сравнивал бы AO сам с собой — то есть бережно сохранял бы ровно тот шум,
    // ради которого его и завели.
    vec2 c = texture(uAO, vUV).ra;
    float centerDepth = c.y;
    float sum = c.x;
    float weight = 1.0;

    // Порог «та же поверхность» пропорционален глубине: на десяти метрах
    // сантиметровая разница — это та же стена, на десяти сантиметрах — уже
    // другая. Постоянный порог работал бы ровно на одной дистанции.
    float tolerance = max(centerDepth * 0.02, 0.02);

    for (int i = 1; i <= 4; ++i) {
        vec2 step = uDirection * uTexel * float(i);
        for (int s = -1; s <= 1; s += 2) {
            vec2 t = texture(uAO, vUV + step * float(s)).ra;
            float w = abs(t.y - centerDepth) < tolerance ? 1.0 : 0.0;
            sum += t.x * w;
            weight += w;
        }
    }
    // СИЛА ЗАТЕНЕНИЯ ПРИМЕНЯЕТСЯ ЗДЕСЬ, а не тем, кто эту карту читает.
    //
    // Раньше её возводил в степень общий composite — то есть настройка одного
    // эффекта лежала в звене другого. Теперь у каждого звена свои параметры:
    // убрали из тракта «Ambient Occlusion» — уехали и радиус, и сила, и
    // возводить в степень стало нечего и некому.
    //
    // Порядок при этом НЕ изменился: в степени возводится уже размытое
    // затенение ровно так же, как это делал composite. pow(blur(x)) и
    // blur(pow(x)) — разные картинки, и первая здесь сохранена намеренно.
    float ao = clamp(sum / weight, 0.0, 1.0);
    if (uApplyStrength == 1) ao = pow(ao, uStrength);
    FragColor = vec4(vec3(ao), centerDepth);
}
)";

// --- Экранное глобальное освещение (SSGI): отражённый свет соседних поверхностей ---
//
// Непрямой свет сцены без запекания — то, чего кадру не хватало, чтобы стать
// «настоящим»: красная стена не подкрашивала пол, освещённый солнцем пол не
// подсвечивал низ предметов, и всё, что вне прямого света, заливалось одним
// ровным ambient. Здесь для каждого пикселя трассируются лучи по полусфере
// нормали В ЭКРАННОМ ПРОСТРАНСТВЕ: луч идёт по глубине кадра, и там, где он
// упирается в поверхность, берётся её цвет из HDR-кадра — это и есть свет,
// который она отражает на нас. Лучи распределены по косинусу (Ламберт), поэтому
// среднее найденного — уже освещённость, без лишних весов.
//
// Чего метод НЕ видит — того, чего нет на экране (свет за краем кадра или за
// предметом). Поэтому он — добавка к окружающему свету и запечённому GI, а не
// их замена: промахнувшийся луч не вносит ничего, а не «чёрное».
//
// Позиция и нормаль — из глубины тем же приёмом, что у SSAO (сосед, ближний по
// глубине): на силуэтах производная дала бы случайную нормаль и шумную кайму.
const char* kSsgiFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uDepth;
uniform sampler2D uScene;
uniform mat4 uProj;
uniform mat4 uInvProj;
uniform vec2 uTexel;
uniform float uRadius;     // как далеко ищется отражающая поверхность, м
uniform float uThickness;  // за сколько метров за поверхностью луч ещё считается попавшим
uniform int uRays;
uniform int uSteps;
// Тонкий G-буфер кадра (rhi::RenderTargetDesc::GBuffer): нормаль поверхности
// в мире, упакованная в [0,1]; альфа 1 — поверхность записана. Есть он — нормаль
// берётся оттуда: из глубины она на краях и на мелкой геометрии угадывалась
// с ошибкой, и GI шёл пятнами по швам.
uniform bool uHasGBuffer;
uniform sampler2D uGNormal;
uniform mat4 uViewRot;     // мир -> вид (берётся поворот)

vec3 ViewPos(vec2 uv) {
    float d = texture(uDepth, uv).r;
    vec4 c = uInvProj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return c.xyz / c.w;
}
vec3 NormalFromDepthOnly(vec2 uv, vec3 P);
vec3 NormalFromDepth(vec2 uv, vec3 P) {
    if (uHasGBuffer) {
        vec4 g = texture(uGNormal, uv);
        if (g.a > 0.5) return normalize(mat3(uViewRot) * (g.xyz * 2.0 - 1.0));
    }
    return NormalFromDepthOnly(uv, P);
}
vec3 NormalFromDepthOnly(vec2 uv, vec3 P) {
    vec3 l = ViewPos(uv - vec2(uTexel.x, 0.0));
    vec3 r = ViewPos(uv + vec2(uTexel.x, 0.0));
    vec3 d = ViewPos(uv - vec2(0.0, uTexel.y));
    vec3 u = ViewPos(uv + vec2(0.0, uTexel.y));
    vec3 dx = (abs(l.z - P.z) < abs(r.z - P.z)) ? (P - l) : (r - P);
    vec3 dy = (abs(d.z - P.z) < abs(u.z - P.z)) ? (P - d) : (u - P);
    vec3 n = cross(dx, dy);
    float len2 = dot(n, n);
    return len2 > 1e-12 ? n * inversesqrt(len2) : vec3(0.0, 0.0, 1.0);
}

void main() {
    float d = texture(uDepth, vUV).r;
    if (d >= 1.0) { FragColor = vec4(0.0, 0.0, 0.0, 1e6); return; }   // небо не освещается
    vec3 P = ViewPos(vUV);
    vec3 N = NormalFromDepth(vUV, P);
    if (dot(N, -P) < 0.0) N = -N;

    // Базис полусферы и шум: свой поворот лучей и сдвиг шага у каждого пикселя,
    // иначе одинаковый узор лучей складывается в видимую решётку. Шум потом
    // гасит размытие с учётом глубины.
    vec3 T = normalize(abs(N.z) < 0.999 ? cross(N, vec3(0.0, 0.0, 1.0)) : cross(N, vec3(0.0, 1.0, 0.0)));
    vec3 B = cross(N, T);
    float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));

    vec3 sum = vec3(0.0);
    for (int i = 0; i < uRays; ++i) {
        float u1 = (float(i) + ign) / float(uRays);
        float phi = 6.2831853 * fract(float(i) * 0.618034 + ign * 0.5);
        float sr = sqrt(u1);
        vec3 dir = T * (cos(phi) * sr) + B * (sin(phi) * sr) + N * sqrt(max(1.0 - u1, 0.0));
        for (int s = 0; s < uSteps; ++s) {
            // Шаги гуще у поверхности: ближние соседи отражают больше всего.
            float f = (float(s) + 0.5 + 0.5 * ign) / float(uSteps);
            vec3 sp = P + dir * (uRadius * f * f + 0.02);
            vec4 clip = uProj * vec4(sp, 1.0);
            if (clip.w <= 0.0) break;
            vec2 suv = clip.xy / clip.w * 0.5 + 0.5;
            if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) break;
            if (texture(uDepth, suv).r >= 1.0) continue;   // над лучом небо — дальше
            vec3 Q = ViewPos(suv);
            float behind = Q.z - sp.z;                      // > 0: поверхность ближе к камере, чем точка луча
            if (behind > 0.0 && behind < uThickness) {
                vec3 L = Q - P;
                float dist2 = dot(L, L);
                // Обращённость отражающей поверхности к нам: изнанка не светит.
                vec3 Nq = NormalFromDepth(suv, Q);
                if (dot(Nq, -Q) < 0.0) Nq = -Nq;   // видимая грань смотрит на камеру
                float facing = clamp(dot(Nq, -L) * inversesqrt(max(dist2, 1e-6)) * 2.0 + 0.5, 0.0, 1.0);
                // Без затухания «по квадрату»: его уже даёт телесный угол — далёкая
                // стена занимает меньше лучей. Мягко гасим только у границы
                // радиуса, чтобы край поиска не читался кольцом.
                float falloff = 1.0 - smoothstep(0.6 * uRadius, uRadius, sqrt(dist2));
                sum += max(texture(uScene, suv).rgb, vec3(0.0)) * facing * falloff;
                break;
            }
        }
    }
    // Линейная глубина — в альфу: по ней размытие не переносит свет через силуэт.
    FragColor = vec4(sum / float(uRays), -P.z);
}
)";

// Размытие отражённого света с учётом глубины, крестом: тот же фильтр, что у
// затенения (kAoBlurFrag), только по трём каналам цвета.
const char* kSsgiBlurFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uGI;
uniform vec2 uTexel;
uniform vec2 uDirection;
void main() {
    vec4 c = texture(uGI, vUV);
    float centerDepth = c.a;
    vec3 sum = c.rgb;
    float weight = 1.0;
    float tolerance = max(centerDepth * 0.03, 0.03);
    for (int i = 1; i <= 6; ++i) {
        for (int s = -1; s <= 1; s += 2) {
            vec4 t = texture(uGI, vUV + uDirection * uTexel * float(i * s) * 1.5);
            float w = abs(t.a - centerDepth) < tolerance ? 1.0 - float(i) / 7.0 : 0.0;
            sum += t.rgb * w;
            weight += w;
        }
    }
    FragColor = vec4(sum / weight, centerDepth);
}
)";

// Подмешивание: отражённый свет падает на поверхность и отражается ЕЁ цветом.
//
// Цвет поверхности — из G-буфера, где он записан (альбедо материала как есть).
// Где не записан (свой шейдер материала, кадр без G-буфера), он оценивается
// по самому кадру: оттенок — нормированный цвет пикселя, светлота — его
// яркость, сжатая к единице. Оценка путала освещённость с цветом: тёмный угол
// белой комнаты считался тёмной поверхностью и почти не получал отражённого
// света — ровно там, где его больше всего видно.
const char* kSsgiApplyFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform sampler2D uGI;
uniform float uIntensity;
uniform float uSaturation;   // насколько отражённый свет сохраняет цвет поверхности-источника
uniform bool uHasGBuffer;
uniform sampler2D uGAlbedo;
void main() {
    vec3 c = max(texture(uScene, vUV).rgb, vec3(0.0));
    vec3 gi = texture(uGI, vUV).rgb;
    float giLuma = dot(gi, vec3(0.2126, 0.7152, 0.0722));
    gi = max(mix(vec3(giLuma), gi, uSaturation), vec3(0.0));
    vec3 albedo;
    vec4 g = uHasGBuffer ? texture(uGAlbedo, vUV) : vec4(0.0);
    if (g.a > 0.5) {
        albedo = g.rgb;
    } else {
        float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
        float peak = max(max(c.r, c.g), max(c.b, 1e-4));
        albedo = (c / peak) * (l / (l + 0.2));
    }
    FragColor = vec4(c + gi * albedo * uIntensity, 1.0);
}
)";

// Bright-pass: выделяет яркие участки выше порога (мягкий knee).
const char* kBrightFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform float uThreshold;
void main() {
    vec3 c = texture(uScene, vUV).rgb;
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float k = max(luma - uThreshold, 0.0) / max(luma, 1e-4);
    FragColor = vec4(c * k, 1.0);
}
)";

// Separable-размытие по Гауссу (направление uDir в UV-единицах).
const char* kBlurFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec2 uDir;
uniform float uScale;       // во сколько раз умножить результат (сила свечения)
uniform int uApplyScale;    // 1 — умножать; 0 — не трогать
void main() {
    float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    vec3 c = texture(uTex, vUV).rgb * w[0];
    for (int i = 1; i < 5; ++i) {
        c += texture(uTex, vUV + uDir * float(i)).rgb * w[i];
        c += texture(uTex, vUV - uDir * float(i)).rgb * w[i];
    }
    // Сила свечения применяется на ПОСЛЕДНЕМ проходе размытия, чтобы она
    // принадлежала звену «Bloom», а не тому, кто это свечение подмешивает (см.
    // пояснение у силы затенения в kAoBlurFrag).
    if (uApplyScale == 1) c *= uScale;
    FragColor = vec4(c, 1.0);
}
)";

// --- Глубина резкости: ближний и дальний планы раздельно ----------------------
//
// ЧТО БЫЛО НЕ ТАК. Один круг нерезкости на пиксель и одно смешивание «резкое
// или размытое» по СОБСТВЕННОЙ глубине пикселя. Ближний размытый предмет на
// фоне резкой стены обрывался резкой кромкой: пиксель стены рядом с ним
// резкий, и размытие переднего плана за свой силуэт не выходило, — а в
// настоящей оптике передний план расплывается ПОВЕРХ фона. Плюс ореолы:
// половинная размытая картинка поднималась билинейно и на силуэтах тащила
// чужой цвет. И настроек было три — фокус, диафрагма, потолок радиуса.
//
// Теперь как у кинематографической глубины резкости:
//   1. prep   (1/2): цвет 2x2 + ЗНАКОВЫЙ круг нерезкости (минус — ближний план)
//   2. near max (1/2, два прохода): наибольший ближний круг в округе — чтобы
//      знать, куда дотягивается размытие переднего плана
//   3. far    (1/2): сбор дальнего плана; ближние сэмплы в него не идут, и
//      резкий передний предмет не растекается по размытому фону
//   4. near   (1/2): сбор ближнего плана С РАДИУСОМ ОКРУГИ и долей покрытия в
//      альфе — это слой, который ложится поверх всего остального
//   5. compose (1/1): резкое -> дальнее по своему кругу -> ближний слой поверх
//
// Круг нерезкости — по тонкой линзе, как у камеры: фокусное расстояние (мм),
// диафрагма (f-число), дистанция фокуса и глубина резкого слоя, раздельная
// сила ближнего и дальнего размытия и потолок радиуса. Фокус — вручную или
// по центру кадра.
const char* kDofCommon = R"(
uniform sampler2D uDepth;
uniform mat4 uInvProj;
uniform float uFocus;        // м
uniform int uAutoFocus;      // 1 — фокус по центру кадра
uniform float uFocusRange;   // м: толщина резкого слоя вокруг фокуса
uniform float uAperture;     // f-число
uniform float uFocalLength;  // мм
uniform float uNearScale, uFarScale;
uniform float uMaxRadius;    // потолок радиуса, пиксели ПОЛНОГО кадра
uniform float uFullHeight;   // высота полного кадра, пиксели

float LinearDepthAt(vec2 uv) {
    float d = texture(uDepth, uv).r;
    vec4 c = uInvProj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return -c.z / c.w;
}

float FocusDistance() {
    if (uAutoFocus == 0) return uFocus;
    // Ближайшее из пяти точек центра: фокус на предмете в центре, а не на
    // фоне, проглядывающем между его краями.
    float d = LinearDepthAt(vec2(0.5));
    d = min(d, LinearDepthAt(vec2(0.48, 0.5)));
    d = min(d, LinearDepthAt(vec2(0.52, 0.5)));
    d = min(d, LinearDepthAt(vec2(0.5, 0.48)));
    d = min(d, LinearDepthAt(vec2(0.5, 0.52)));
    return d;
}

// Знаковый радиус круга нерезкости в пикселях полного кадра (минус — ближе
// фокуса). Тонкая линза: c = A·f·(d − s) / (d·(s − f)), A = f / N; кадр
// высотой 24 мм, как у полнокадровой камеры.
float SignedCoc(float depth, float focus) {
    float f = max(uFocalLength, 1.0) * 0.001;
    float s = max(focus, f * 1.5);
    float band = max(uFocusRange, 0.0) * 0.5;
    // Резкий слой: всё, что ближе к фокусу чем band, в фокусе.
    float d = depth < s ? min(depth + band, s) : max(depth - band, s);
    float A = f / max(uAperture, 0.5);
    float c = A * f * (d - s) / (max(d, 1e-3) * (s - f));
    float px = c / 0.024 * uFullHeight * 0.5;
    px *= px < 0.0 ? uNearScale : uFarScale;
    return clamp(px, -uMaxRadius, uMaxRadius);
}
)";

const char* kDofPrepFrag = R"(
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform vec2 uFullTexel;
void main() {
    // Префильтр 2x2 по углам своего текселя: без него мелкая текстура и
    // кромки под размытием «звенят».
    vec2 o = uFullTexel * 0.5;
    vec3 c = texture(uScene, vUV + vec2(-o.x, -o.y)).rgb
           + texture(uScene, vUV + vec2( o.x, -o.y)).rgb
           + texture(uScene, vUV + vec2(-o.x,  o.y)).rgb
           + texture(uScene, vUV + vec2( o.x,  o.y)).rgb;
    float focus = FocusDistance();
    // Круг — БЛИЖАЙШЕЙ из четырёх точек: на силуэте переднего предмета
    // половинный пиксель обязан принадлежать ему, а не фону за ним.
    float d = min(min(LinearDepthAt(vUV + vec2(-o.x, -o.y)), LinearDepthAt(vUV + vec2(o.x, -o.y))),
                  min(LinearDepthAt(vUV + vec2(-o.x, o.y)), LinearDepthAt(vUV + vec2(o.x, o.y))));
    FragColor = vec4(c * 0.25, SignedCoc(d, focus));
}
)";

// Наибольший ближний круг в округе — раздельным максимумом (сначала по
// горизонтали, потом по вертикали). Радиус окна — потолок радиуса.
const char* kDofNearMaxFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSrc;
uniform vec2 uStep;       // тексель вдоль оси
uniform int uReach;       // сколько текселей в каждую сторону
uniform int uFromPrep;    // 1 — источник prep (знаковый круг в альфе)
void main() {
    float m = 0.0;
    for (int i = -32; i <= 32; ++i) {
        if (i < -uReach || i > uReach) continue;
        vec4 s = texture(uSrc, vUV + uStep * float(i));
        // Ближний круг — в ПОЛОВИННЫХ пикселях, как читает сбор.
        float n = uFromPrep == 1 ? max(-s.a, 0.0) * 0.5 : s.r;
        // Дотягивается ли круг этого текселя сюда.
        if (n >= abs(float(i))) m = max(m, n);
    }
    FragColor = vec4(m, 0.0, 0.0, 1.0);
}
)";

// Сбор по диску на половинном разрешении: спираль по золотому углу, число
// выборок растёт с радиусом (плотность шага не хуже текселя), с потолком по
// качеству.
const char* kDofGatherFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uPrep;     // цвет + знаковый круг (полные пиксели)
uniform sampler2D uNearMax;  // наибольший ближний круг округи (половинные пиксели)
uniform vec2 uTexel;         // тексель половинного буфера
uniform int uNear;           // 0 — дальний план, 1 — ближний
uniform int uMaxSamples;
void main() {
    vec4 center = texture(uPrep, vUV);
    float radius = uNear == 1 ? texture(uNearMax, vUV).r
                              : max(center.a, 0.0) * 0.5;
    if (radius < 0.5) {
        FragColor = uNear == 1 ? vec4(0.0) : vec4(center.rgb, 1.0);
        return;
    }
    int n = clamp(int(radius * radius * 1.2), 12, uMaxSamples);
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    if (uNear == 0) { sum = center.rgb; wsum = 1.0; }
    const float kGolden = 2.39996323;
    for (int i = 0; i < 128; ++i) {
        if (i >= n) break;
        float fi = (float(i) + 0.5) / float(n);
        float ang = float(i) * kGolden;
        float r = sqrt(fi) * radius;
        vec2 suv = clamp(vUV + vec2(cos(ang), sin(ang)) * r * uTexel,
                         uTexel * 0.5, vec2(1.0) - uTexel * 0.5);
        vec4 s = texture(uPrep, suv);
        // Свой круг сэмпла (половинные пиксели) — и только своего плана:
        // резкий передний предмет не растекается по размытому фону, а фон не
        // попадает в слой переднего плана.
        float own = uNear == 1 ? max(-s.a, 0.0) * 0.5 : max(s.a, 0.0) * 0.5;
        float w = clamp(own - r + 1.0, 0.0, 1.0);
        sum += s.rgb * w;
        wsum += w;
    }
    if (uNear == 1) {
        // Доля покрытия — это и есть прозрачность слоя переднего плана: на
        // кромке размытого предмета половина выборок его, половина нет.
        float cover = clamp(wsum / float(n) * 1.5, 0.0, 1.0);
        // ПРЕДУМНОЖЕННЫЙ цвет: слой половинный и поднимается билинейно, и на
        // его краю пустые тексели (цвет 0, покрытие 0) иначе подмешивали бы
        // чёрное к цвету — по границе ближнего плана шла тёмная полоса.
        FragColor = vec4(wsum > 1e-4 ? sum / wsum * cover : vec3(0.0), cover);
    } else {
        FragColor = vec4(sum / max(wsum, 1e-4), 1.0);
    }
}
)";

// Сведение на полном разрешении.
const char* kDofCompositeFrag = R"(
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;   // резкая картинка, полное разрешение
uniform sampler2D uFar;     // дальний план, половинное
uniform sampler2D uNearLayer; // ближний слой (rgb + покрытие), половинное
uniform int uShowFocus;
uniform vec2 uHalfTexel;    // тексель половинных слоёв

// Подъём половинного слоя ЧЕТЫРЬМЯ билинейными выборками (шатёр 4x4), а не
// одной: одна выборка поднимает его блоками 2x2, и на косой границе двух
// размытых планов (пол и стена вдали) шла лесенка.
vec4 Upsample(sampler2D tex) {
    vec2 o = uHalfTexel * 0.5;
    return 0.25 * (texture(tex, vUV + vec2(-o.x, -o.y)) + texture(tex, vUV + vec2(o.x, -o.y)) +
                   texture(tex, vUV + vec2(-o.x, o.y)) + texture(tex, vUV + vec2(o.x, o.y)));
}

void main() {
    vec3 sharp = texture(uScene, vUV).rgb;
    float coc = SignedCoc(LinearDepthAt(vUV), FocusDistance());
    // Дальний план — по СОБСТВЕННОМУ кругу пикселя, переход растянут на пару
    // пикселей: резкость плоскости фокуса не теряется, ступеньки нет.
    float t = smoothstep(1.0, 3.0, max(coc, 0.0));
    vec3 col = mix(sharp, Upsample(uFar).rgb, t);
    // Ближний слой — ПОВЕРХ всего: передний план расплывается за свой силуэт.
    vec4 nearLayer = Upsample(uNearLayer);   // предумноженный
    col = col * (1.0 - nearLayer.a) + nearLayer.rgb;
    if (uShowFocus == 1) {
        // Разбор: в фокусе — зелёный, ближе — синий, дальше — оранжевый.
        float k = clamp(abs(coc) / 8.0, 0.0, 1.0);
        vec3 tint = coc < -0.5 ? vec3(0.2, 0.4, 1.0) : (coc > 0.5 ? vec3(1.0, 0.55, 0.1) : vec3(0.2, 1.0, 0.3));
        col = mix(col, tint * max(dot(col, vec3(0.333)), 0.15) * 2.0, mix(0.35, 0.7, k));
    }
    FragColor = vec4(col, 1.0);
}
)";

// --- Motion blur: камерный, по репроекции глубины матрицей прошлого кадра ---
//
// Мировая позиция пикселя восстанавливается из глубины, затем проецируется
// матрицей ПРОШЛОГО кадра. Разница экранных координат — вектор скорости; вдоль
// него и усредняется цвет. Так смазывается движение и поворот камеры, чего
// достаточно для съёмки; смаз от движения самих объектов потребовал бы
// отдельного буфера скоростей на этапе геометрии.
const char* kMotionFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uScene;
uniform sampler2D uDepth;
uniform sampler2D uVelocity;  // буфер скоростей (если uUseVelocity == 1)
uniform mat4 uInvViewProj;   // обратная view-projection ЭТОГО кадра
uniform mat4 uPrevViewProj;  // view-projection ПРОШЛОГО кадра
uniform float uAmount;
uniform int uSamples;
uniform int uUseVelocity;    // 1 — брать вектор из буфера скоростей
uniform float uMaxLen;       // потолок длины смаза, доля кадра

// Движение камеры для пикселя: мировая точка из глубины, спроецированная
// матрицей прошлого кадра. Мир при этом считается неподвижным.
bool CameraVelocity(out vec2 v) {
    float d = texture(uDepth, vUV).r;
    vec4 clip = vec4(vUV * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProj * clip;
    if (abs(world.w) < 1e-6) return false;
    world /= world.w;
    vec4 prevClip = uPrevViewProj * world;
    if (prevClip.w < 1e-6) return false;   // за камерой
    vec2 prevUV = (prevClip.xy / prevClip.w) * 0.5 + 0.5;
    v = vUV - prevUV;
    return true;
}

void main() {
    vec2 velocity = vec2(0.0);

    if (uUseVelocity == 1) {
        // Готовый вектор из прохода геометрии: он уже учитывает и движение
        // камеры, и собственное движение объекта. Синий = 1 — геометрии тут
        // нет (небо, фон): берём движение камеры, иначе небо при повороте
        // оставалось бы резким, а края предметов на нём — смазанными.
        vec4 v = texture(uVelocity, vUV);
        if (v.b > 0.5) {
            if (!CameraVelocity(velocity)) { FragColor = texture(uScene, vUV); return; }
        } else {
            velocity = v.xy;
        }
    } else if (!CameraVelocity(velocity)) {
        // Запасной путь без буфера скоростей: смаз только от камеры.
        FragColor = texture(uScene, vUV);
        return;
    }
    velocity *= uAmount;

    // Потолок длины: резкий рывок камеры иначе размазал бы кадр целиком, и
    // вместо смаза получилась бы каша.
    float kMaxLen = max(uMaxLen, 1e-4);
    float len = length(velocity);
    if (len < 1e-5) { FragColor = texture(uScene, vUV); return; }
    if (len > kMaxLen) velocity *= kMaxLen / len;

    vec3 sum = vec3(0.0);
    int n = max(uSamples, 2);
    for (int i = 0; i < n; ++i) {
        // Центрируем выборку на текущем пикселе: смаз идёт симметрично в обе
        // стороны, объект не «съезжает» с реальной позиции.
        float t = float(i) / float(n - 1) - 0.5;
        sum += texture(uScene, vUV + velocity * t).rgb;
    }
    FragColor = vec4(sum / float(n), 1.0);
}
)";

// Финальный composite: scene*AO + bloom -> экспозиция -> ACES -> насыщенность/
// контраст -> хроматическая аберрация -> виньетка -> гамма.

} // namespace sage::render::postfx
