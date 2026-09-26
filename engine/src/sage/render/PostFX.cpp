#include "sage/render/PostFX.h"

#include <chrono>
#include <cmath>

#include "sage/core/Log.h"
#include "sage/core/Profiler.h"

#include <algorithm>
#include <string>
#include <vector>

#include <glm/gtc/matrix_inverse.hpp>

#include "sage/render/LensFlare.h"
#include "sage/render/Shader.h"
#include "sage/rhi/GraphicsDevice.h"

using namespace sage::rhi;

namespace sage::render {

int SceneSamples(const sage::EngineConfig& cfg) {
    // Ступени растеризатора: всё, что между ними, округляем ВНИЗ — обещать
    // человеку 8x и молча дать 4x хуже, чем дать ровно то, что он выбрал из
    // доступного. Выше 8 не поднимаемся: выигрыш уже неразличим, а цена растёт
    // линейно по памяти буфера.
    const int m = cfg.Msaa;
    if (m >= 8) return 8;
    if (m >= 4) return 4;
    if (m >= 2) return 2;
    return 1;
}

// ============================================================================
//  Встроенные шейдеры пост-обработки (лениво, не уничтожаются — как остальные
//  встроенные шейдеры движка: function-local static с деструктором Shader снёс
//  бы GL-программу уже после разрушения контекста).
// ============================================================================
namespace {

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

// --- FXAA ------------------------------------------------------------------
// Сглаживание кромок по яркости готовой картинки (Timothy Lottes, FXAA 3.11,
// сокращённый вариант). Работает ПОСЛЕ тон-маппинга: пороги здесь заданы в
// воспринимаемой яркости, и в линейном HDR они не имеют смысла.
//
// Идея: найти локальный контраст по четырём соседям, определить направление
// кромки (вертикаль/горизонталь) и сместить выборку ВДОЛЬ неё — так ступенька
// заменяется плавным переходом, а плоские области остаются нетронутыми.
const char* kFxaaFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec2 uTexel;         // 1/размер текстуры
uniform float uContrastThreshold;

// Воспринимаемая яркость. Полноценная формула здесь избыточна: FXAA нужен
// монотонный признак «светлее/темнее», а не колориметрия.
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    vec3 rgbM = texture(uTex, vUV).rgb;
    float lM = luma(rgbM);
    float lN = luma(texture(uTex, vUV + vec2(0.0, -uTexel.y)).rgb);
    float lS = luma(texture(uTex, vUV + vec2(0.0,  uTexel.y)).rgb);
    float lW = luma(texture(uTex, vUV + vec2(-uTexel.x, 0.0)).rgb);
    float lE = luma(texture(uTex, vUV + vec2( uTexel.x, 0.0)).rgb);

    float lMin = min(lM, min(min(lN, lS), min(lW, lE)));
    float lMax = max(lM, max(max(lN, lS), max(lW, lE)));
    float range = lMax - lMin;

    // Плоский участок — не трогаем вовсе. Без этого порога FXAA мылит текстуры.
    if (range < max(uContrastThreshold, lMax * 0.125)) {
        FragColor = vec4(rgbM, 1.0);
        return;
    }

    // Диагональные соседи нужны, чтобы отличить вертикальную кромку от
    // горизонтальной: по одним «крестом» направление неоднозначно.
    float lNW = luma(texture(uTex, vUV + vec2(-uTexel.x, -uTexel.y)).rgb);
    float lNE = luma(texture(uTex, vUV + vec2( uTexel.x, -uTexel.y)).rgb);
    float lSW = luma(texture(uTex, vUV + vec2(-uTexel.x,  uTexel.y)).rgb);
    float lSE = luma(texture(uTex, vUV + vec2( uTexel.x,  uTexel.y)).rgb);

    float edgeH = abs((lNW + lNE) - 2.0 * lN) * 2.0 +
                  abs((lW  + lE ) - 2.0 * lM) * 4.0 +
                  abs((lSW + lSE) - 2.0 * lS) * 2.0;
    float edgeV = abs((lNW + lSW) - 2.0 * lW) * 2.0 +
                  abs((lN  + lS ) - 2.0 * lM) * 4.0 +
                  abs((lNE + lSE) - 2.0 * lE) * 2.0;
    bool horizontal = edgeH >= edgeV;

    // Шаг делаем в сторону БОЛЬШЕГО перепада: туда, где лежит другая сторона
    // кромки, — иначе смешивали бы пиксель сам с собой.
    float l1 = horizontal ? lN : lW;
    float l2 = horizontal ? lS : lE;
    float grad1 = abs(l1 - lM);
    float grad2 = abs(l2 - lM);
    // Положительный шаг ведёт к l2 (S или E) — туда и идём, когда перепад
    // больше именно там. Знак здесь легко перепутать, и ошибка тихая: FXAA
    // продолжает работать, но смешивает пиксель с СОБСТВЕННОЙ стороной кромки
    // и почти ничего не меняет.
    float stepLen = horizontal ? uTexel.y : uTexel.x;
    float lOther = l2;
    if (grad1 > grad2) { stepLen = -stepLen; lOther = l1; }

    // --- ПОИСК ВДОЛЬ КРОМКИ ---------------------------------------------------
    //
    // Без него всё вышенаписанное сглаживает ровно ОДИН пиксель: берётся сосед
    // через полтекселя и подмешивается по силе локального контраста. Ступенька
    // при этом остаётся ступенькой — она длиной в несколько пикселей, а
    // подмешали мы одинаково по всей её длине, — зато вся картинка получает
    // лёгкое размытие. Ровно это и читается как «сглаживание не работает, а
    // картинка мыльная»: цена заплачена, эффекта нет.
    //
    // Настоящий FXAA сначала идёт ВДОЛЬ кромки в обе стороны и ищет, где она
    // кончается. Зная расстояние до обоих концов и то, с какой стороны стоит
    // текущий пиксель, можно посчитать, КАКУЮ ЧАСТЬ пикселя кромка накрывает, —
    // и подмешать ровно её. Тогда длинная пологая ступенька превращается в
    // ровный градиент вдоль всей своей длины, а плоские места не трогаются
    // вовсе.
    vec2 edgeDir = horizontal ? vec2(uTexel.x, 0.0) : vec2(0.0, uTexel.y);
    vec2 halfStep = horizontal ? vec2(0.0, stepLen * 0.5) : vec2(stepLen * 0.5, 0.0);
    // Яркость НА САМОЙ кромке (между двумя её сторонами) и допуск, по которому
    // мы считаем, что кромка ещё продолжается.
    float lEdge = (lM + lOther) * 0.5;
    float gradScaled = 0.25 * max(abs(lOther - lM), 1e-5);

    vec2 posN = vUV + halfStep - edgeDir;
    vec2 posP = vUV + halfStep + edgeDir;
    float endN = 0.0, endP = 0.0;
    bool doneN = false, doneP = false;

    // Двенадцати шагов хватает: кромка длиннее двенадцати пикселей на экране
    // почти горизонтальна, и остаток всё равно неразличим. Потолок обязателен —
    // цикл без него на «кромке» через весь экран стоил бы сотен выборок.
    const int kMaxSteps = 12;
    for (int i = 1; i <= kMaxSteps; ++i) {
        if (!doneN) {
            endN = luma(texture(uTex, posN).rgb) - lEdge;
            if (abs(endN) >= gradScaled) doneN = true; else posN -= edgeDir;
        }
        if (!doneP) {
            endP = luma(texture(uTex, posP).rgb) - lEdge;
            if (abs(endP) >= gradScaled) doneP = true; else posP += edgeDir;
        }
        if (doneN && doneP) break;
    }

    float distN = horizontal ? (vUV.x - posN.x) : (vUV.y - posN.y);
    float distP = horizontal ? (posP.x - vUV.x) : (posP.y - vUV.y);
    float distMin = min(distN, distP);
    float spanLen = distN + distP;

    // С какой стороны кромки мы стоим. Если ближний конец «повернул» в ту же
    // сторону, что и текущий пиксель, — мы на светлой (или тёмной) стороне, и
    // подмешивать не надо: сглаживается только та половина, что кромка режет.
    bool lumaMLess = (lM - lEdge) < 0.0;
    bool nearEndFlips = ((distN < distP) ? endN : endP) < 0.0;
    float pixelBlend = (nearEndFlips == lumaMLess) ? 0.0
                                                  : (0.5 - distMin / max(spanLen, 1e-5));
    pixelBlend = max(pixelBlend, 0.0);

    // Второй кандидат — старая оценка по локальному контрасту. Она нужна там,
    // где поиск не нашёл концов (кромка ушла за потолок шагов): без неё такие
    // места остались бы вовсе несглаженными.
    float lAvg = (2.0 * (lN + lS + lW + lE) + lNW + lNE + lSW + lSE) / 12.0;
    float localBlend = clamp(abs(lAvg - lM) / max(range, 1e-5), 0.0, 1.0);
    localBlend = localBlend * localBlend * 0.75;

    float blend = max(pixelBlend, localBlend);
    if (blend <= 0.0) { FragColor = vec4(rgbM, 1.0); return; }

    vec2 offset = horizontal ? vec2(0.0, stepLen * blend) : vec2(stepLen * blend, 0.0);
    FragColor = vec4(texture(uTex, vUV + offset).rgb, 1.0);
}
)";

// ============================================================================
//  ЗВЕНЬЯ ГОТОВОГО КАДРА — КАЖДОЕ СВОИМ ПРОХОДОМ
//
// Раньше экспозиция, цвет, тон-маппинг, виньетка и аберрация жили в ОДНОМ
// шейдере (kCompositeFrag): порядок между ними был зашит в его текст, включить
// одно без другого было нельзя, а «виньетка» была параметром тон-маппинга —
// то есть эффектом, которого нет в списке эффектов.
//
// Теперь каждое из них — отдельное звено со своим этапом (PostStage), своим
// выключателем и своими настройками. Цена — полноэкранный проход на звено;
// плата честная: выключенное звено не исполняется вовсе, а порядок виден в
// инспекторе ровно такой, каким он будет выполнен.
// ============================================================================

// --- Экспозиция: сколько света собрал кадр. HDR -> HDR ----------------------
// В СТУПЕНЯХ (EV), а не множителем: ступень — это «вдвое», и привычка к ней
// приходит из фотографии. Множитель 1.7 не говорит ничего, +0.75 EV говорит.
const char* kExposureFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform float uEV;
void main() { FragColor = vec4(texture(uScene, vUV).rgb * exp2(uEV), 1.0); }
)";

// --- Автоэкспозиция (адаптация глаза) ----------------------------------------
// Замер — ГИСТОГРАММА log2-яркости по сетке 32x32 с весом к центру
// («центрально-взвешенный» замер фотокамеры: небо по краям кадра не должно
// затемнять героя в середине). Из неё берётся среднее между процентилями
// «Low/High Percent»: самые тёмные углы и самые яркие блики (лампа, солнце в
// кадре) не раскачивают экспозицию всего кадра.
//
// ЧЁРНОЕ В ЗАМЕР НЕ ВХОДИТ. Раньше мерилось простое среднее, и пустота (чёрное
// небо, фон без неба) тянула его вниз: сцена, где 70 % кадра — чёрное небо, а
// остальное освещено лампой, считалась «очень тёмной», глаз вытягивал её до
// предела — в сотни раз, — и всё освещённое выгорало в белое, а свечение
// обводило каждый предмет светящейся каймой. У пустоты нет яркости, к которой
// можно привыкнуть: пиксели темнее «Black Level» просто не голосуют. Кадр,
// где не голосует никто, оставляет адаптацию как была.
//
// Результат плавно догоняет прошлое значение: светлеет глаз быстрее, чем
// темнеет, как у человека.
const char* kAutoExposureMeterFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform sampler2D uPrev;
uniform int uHasPrev;
uniform float uDt;
uniform float uSpeedUp;
uniform float uSpeedDown;
uniform float uMinEV;
uniform float uMaxEV;
uniform float uLow;        // доля самых тёмных, что отбрасывается (0..1)
uniform float uHigh;       // доля, выше которой — отбрасывается (0..1)
uniform float uBlack;      // log2 яркости, ниже которой пиксель не голосует
uniform float uNeutral;    // значение адаптации «без поправки» (для пустого кадра)
const int kBins = 64;
const float kHistMin = -16.0, kHistMax = 16.0;
void main() {
    float hist[kBins];
    for (int i = 0; i < kBins; ++i) hist[i] = 0.0;
    float total = 0.0;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            vec2 uv = (vec2(float(x), float(y)) + 0.5) / 32.0;
            vec3 c = textureLod(uScene, uv, 0.0).rgb;
            float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
            if (!(l > 0.0)) continue;            // и NaN тоже
            float ev = log2(l);
            if (ev < uBlack) continue;
            vec2 d = uv - 0.5;
            float w = exp(-dot(d, d) * 5.0);
            float t = clamp((ev - kHistMin) / (kHistMax - kHistMin), 0.0, 0.9999);
            hist[int(t * float(kBins))] += w;
            total += w;
        }
    }
    float prev = uHasPrev == 1 ? texture(uPrev, vec2(0.5)).r : uNeutral;
    float target = prev;
    if (total > 1e-4) {
        float lo = total * clamp(min(uLow, uHigh), 0.0, 0.99);
        float hi = total * clamp(max(uLow, uHigh), 0.01, 1.0);
        float acc = 0.0, sum = 0.0, used = 0.0;
        for (int i = 0; i < kBins; ++i) {
            float a0 = acc, a1 = acc + hist[i];
            acc = a1;
            float part = max(0.0, min(a1, hi) - max(a0, lo));
            float center = kHistMin + (float(i) + 0.5) * (kHistMax - kHistMin) / float(kBins);
            sum += center * part;
            used += part;
        }
        target = used > 1e-5 ? sum / used : prev;
    }
    float ev = clamp(target, uMinEV, uMaxEV);
    if (uHasPrev == 0) { FragColor = vec4(ev, 0.0, 0.0, 1.0); return; }
    float speed = ev > prev ? uSpeedUp : uSpeedDown;
    float k = 1.0 - exp(-uDt * max(speed, 0.0));
    FragColor = vec4(mix(prev, ev, k), 0.0, 0.0, 1.0);
}
)";

// Применение: средне-серый 18 % кадра приводится к 18 % на экране, плюс
// компенсация автора в ступенях (по умолчанию +1).
const char* kAutoExposureApplyFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform sampler2D uAdapted;
uniform float uCompensation;
void main() {
    float avg = texture(uAdapted, vec2(0.5)).r;
    float scale = 0.18 * exp2(uCompensation - avg);
    FragColor = vec4(texture(uScene, vUV).rgb * scale, 1.0);
}
)";

// --- Цвет: яркость, контраст, насыщенность, температура. HDR -> HDR ---------
const char* kColorFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform float uBrightness;
uniform float uContrast;
uniform float uSaturation;
uniform float uTemperature;
uniform float uTint;
void main() {
    vec3 c = max(texture(uScene, vUV).rgb, 0.0) * uBrightness;

    // Баланс белого — усилением каналов. Приближение, и намеренно простое:
    // честный пересчёт через цветовую температуру требует матрицы адаптации и
    // белой точки, а на глаз в игре крутят именно «теплее/холоднее».
    c *= vec3(1.0 + uTemperature * 0.25, 1.0 + uTint * 0.15, 1.0 - uTemperature * 0.25);

    // КОНТРАСТ ВОКРУГ СРЕДНЕ-СЕРОГО 0.18, А НЕ ВОКРУГ 0.5.
    //
    // Кадр здесь ещё линейный (HDR), и «середина» в нём — не 0.5, а 0.18:
    // это то, что после тон-маппинга станет серединой картинки. Контраст,
    // закрученный вокруг 0.5 в линейном кадре, не поджимает его к середине, а
    // тянет вниз — то есть просто гасит. Ровно на этом однажды и попались,
    // когда контраст стоял до гаммы.
    const float pivot = 0.18;
    c = pow(c / pivot, vec3(uContrast)) * pivot;

    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(luma), c, uSaturation);
    FragColor = vec4(max(c, 0.0), 1.0);
}
)";

// --- Цветокоррекция по диапазонам: тени / средние / света. HDR -> HDR -------
const char* kGradeFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform vec3 uShadows;
uniform vec3 uMidtones;
uniform vec3 uHighlights;
void main() {
    vec3 c = max(texture(uScene, vUV).rgb, 0.0);
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    // Веса диапазонов перекрываются и в сумме дают единицу: иначе на границе
    // диапазонов появилась бы ступенька — видно её сразу и объясняется плохо.
    float hi = smoothstep(0.25, 1.0, luma);
    float lo = 1.0 - smoothstep(0.0, 0.25, luma);
    float mid = max(1.0 - hi - lo, 0.0);
    vec3 gain = uShadows * lo + uMidtones * mid + uHighlights * hi;
    FragColor = vec4(c * gain, 1.0);
}
)";

// --- Тон-маппинг: HDR -> готовый к показу кадр ------------------------------
// Единственное звено, меняющее пространство картинки. Кривая выбирается
// режимом: разные кривые — разный характер света, и подменять выбор автора
// «правильной» нельзя.
const char* kTonemapFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform int uMode;
uniform float uGamma;
uniform int uOutput;   // 0 sRGB, 1 гамма, 2 линейный

vec3 ACES(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}
vec3 Reinhard(vec3 x) { return x / (1.0 + x); }
vec3 Filmic(vec3 x) {
    // Uncharted 2: мягкое плечо в светах и поджатые тени.
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30, W = 11.2;
    vec3 v = ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
    float w = ((W * (A * W + C * B) + D * E) / (W * (A * W + B) + D * F)) - E / F;
    return clamp(v / w, 0.0, 1.0);
}

// --- Кинематографическая плёночная кривая -----------------------------------
// Параметрическая кривая «носок — прямая — плечо» (Slope 0.88, Toe 0.55,
// Shoulder 0.26, BlackClip 0, WhiteClip 0.04), построенная в логарифме
// яркости в пространстве ACES AP1: там насыщенные цвета выгорают к белому, а
// не к «кислотному» оттенку, — это и делает картинку мягкой в светах. Глоу и
// красная поправка RRT опущены: они меняют оттенок едва заметно, а стоят двух
// лишних матриц.
vec3 MulRows(vec3 r0, vec3 r1, vec3 r2, vec3 v) { return vec3(dot(r0, v), dot(r1, v), dot(r2, v)); }
vec3 CinematicFilm(vec3 lin) {
    const float Slope = 0.88, Toe = 0.55, Shoulder = 0.26, BlackClip = 0.0, WhiteClip = 0.04;
    const vec3 Y = vec3(0.2722287, 0.6740818, 0.0536895);   // яркость в AP1
    vec3 ap1 = MulRows(vec3(0.61319, 0.33951, 0.04737), vec3(0.07021, 0.91634, 0.01345),
                       vec3(0.02062, 0.10957, 0.86961), lin);
    ap1 = max(mix(vec3(dot(ap1, Y)), ap1, 0.96), vec3(1e-10));
    vec3 logc = log(ap1) / log(10.0);
    const float ToeScale = 1.0 + BlackClip - Toe;
    const float ShoulderScale = 1.0 + WhiteClip - Shoulder;
    const float InMatch = 0.18, OutMatch = 0.18;
    float bt = (OutMatch + BlackClip) / ToeScale - 1.0;
    float ToeMatch = log(InMatch) / log(10.0) - 0.5 * log((1.0 + bt) / (1.0 - bt)) * (ToeScale / Slope);
    float StraightMatch = (1.0 - Toe) / Slope - ToeMatch;
    float ShoulderMatch = Shoulder / Slope - StraightMatch;
    vec3 straight = Slope * (logc + StraightMatch);
    vec3 toe = -BlackClip + (2.0 * ToeScale) / (1.0 + exp((-2.0 * Slope / ToeScale) * (logc - ToeMatch)));
    vec3 shoulder = (1.0 + WhiteClip) - (2.0 * ShoulderScale) /
                    (1.0 + exp((2.0 * Slope / ShoulderScale) * (logc - ShoulderMatch)));
    toe = mix(straight, toe, vec3(lessThan(logc, vec3(ToeMatch))));
    shoulder = mix(straight, shoulder, vec3(greaterThan(logc, vec3(ShoulderMatch))));
    vec3 t = clamp((logc - ToeMatch) / (ShoulderMatch - ToeMatch), 0.0, 1.0);
    // При этих настройках стык плеча лежит ЛЕВЕЕ стыка носка, и
    // смешивание разворачивается — без этой строки средне-серый уезжал на
    // плечо и темнел до 0.14.
    if (ShoulderMatch < ToeMatch) t = 1.0 - t;
    t = (3.0 - 2.0 * t) * t * t;
    vec3 tone = mix(toe, shoulder, t);
    tone = max(mix(vec3(dot(tone, Y)), tone, 0.93), vec3(0.0));
    vec3 srgb = MulRows(vec3(1.70505, -0.62179, -0.08326), vec3(-0.13026, 1.14080, -0.01055),
                        vec3(-0.02400, -0.12897, 1.15297), tone);
    return clamp(srgb, 0.0, 1.0);
}

// --- AgX: мягкие света, честный переход насыщенного в белый ----------------
// Приближение полиномом (Benjamin Wrensch), вывод — линейный цвет дисплея.
vec3 AgX(vec3 lin) {
    const mat3 inMat = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                            0.0784335999999992, 0.878468636469772, 0.0784336,
                            0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outMat = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                             -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                             -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    vec3 v = inMat * lin;
    v = clamp(log2(max(v, vec3(1e-10))), minEv, maxEv);
    v = (v - minEv) / (maxEv - minEv);
    vec3 x2 = v * v, x4 = x2 * x2;
    v = 15.5 * x4 * x2 - 40.14 * x4 * v + 31.96 * x4 - 6.868 * x2 * v + 0.4298 * x2 + 0.1191 * v - 0.00232;
    v = outMat * v;
    return clamp(pow(max(v, vec3(0.0)), vec3(2.2)), 0.0, 1.0);
}

// --- Khronos PBR Neutral: цвет материала остаётся цветом материала ---------
// Кривая для предметной визуализации: до 0.76 не трогает ничего, выше —
// сжимает только света. Базовый цвет ассета на экране совпадает с заданным.
vec3 PbrNeutral(vec3 c) {
    const float start = 0.8 - 0.04, desat = 0.15;
    float x = min(c.r, min(c.g, c.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    c -= offset;
    float peak = max(c.r, max(c.g, c.b));
    if (peak < start) return clamp(c, 0.0, 1.0);
    const float d = 1.0 - start;
    float newPeak = 1.0 - d * d / (peak + d - start);
    c *= newPeak / peak;
    float g = 1.0 - 1.0 / (desat * (peak - newPeak) + 1.0);
    return clamp(mix(c, vec3(newPeak), g), 0.0, 1.0);
}

// Кодирование для экрана. sRGB — кусочная кривая стандарта (её ждут мониторы
// и браузеры), гамма — степенная (как было в движке до выбора), линейный —
// без кодирования: для внешней обработки кадра и для отладки.
vec3 Encode(vec3 c) {
    if (uOutput == 2) return c;
    if (uOutput == 1) return pow(c, vec3(1.0 / uGamma));
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, vec3(lessThanEqual(c, vec3(0.0031308))));
}

void main() {
    vec3 hdr = max(texture(uScene, vUV).rgb, 0.0);
    vec3 c;
    if (uMode == 1)      c = Reinhard(hdr);
    else if (uMode == 2) c = ACES(hdr);
    else if (uMode == 3) c = Filmic(hdr);
    else if (uMode == 4) c = CinematicFilm(hdr);
    else if (uMode == 5) c = AgX(hdr);
    else if (uMode == 6) c = PbrNeutral(hdr);
    else                 c = clamp(hdr, 0.0, 1.0); // без кривой: просто обрезка
    FragColor = vec4(Encode(c), 1.0);
}
)";

// --- Виньетка: падение яркости к краю кадра. LDR -> LDR ---------------------
const char* kVignetteFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform float uIntensity;
uniform float uSmoothness;
void main() {
    vec2 d = vUV - 0.5;
    float r = length(d) * 1.4142;           // 1.0 в углу кадра
    float edge = smoothstep(1.0 - uSmoothness, 1.0, r);
    float vig = 1.0 - uIntensity * edge;
    FragColor = vec4(texture(uScene, vUV).rgb * clamp(vig, 0.0, 1.0), 1.0);
}
)";

// --- Хроматическая аберрация: каналы расходятся к краю. LDR -> LDR ----------
const char* kChromaticFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform float uAmount;
void main() {
    // ВДОЛЬ РАДИУСА и тем сильнее, чем дальше от центра, — так ведёт себя
    // настоящая оптика. В центре расхождения нет, и лицо в центре кадра
    // остаётся чистым.
    vec2 off = (vUV - 0.5) * uAmount * 0.02;
    FragColor = vec4(texture(uScene, vUV + off).r,
                     texture(uScene, vUV).g,
                     texture(uScene, vUV - off).b, 1.0);
}
)";

// --- Зерно плёнки. LDR -> LDR -----------------------------------------------
const char* kGrainFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform vec2 uResolution;
uniform float uAmount;
uniform float uSize;
uniform float uTime;
float Hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123); }
void main() {
    vec3 c = texture(uScene, vUV).rgb;
    // Зерно считается в ПИКСЕЛЯХ, а не в UV: иначе его размер менялся бы с
    // разрешением окна, и снятый в 4K кадр выглядел бы чище того же кадра в HD.
    vec2 cell = floor(vUV * uResolution / max(uSize, 0.5));
    float n = Hash(cell + fract(uTime) * 71.3) - 0.5;
    // Тени зернят сильнее светов — как настоящая плёнка.
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float weight = mix(1.0, 0.35, smoothstep(0.2, 0.9, luma));
    FragColor = vec4(clamp(c + n * uAmount * weight, 0.0, 1.0), 1.0);
}
)";

// --- Подмешивание свечения к кадру ------------------------------------------
// Раньше это делал тон-маппинг: он читал опубликованную карту свечения. То
// есть «включить свечение» означало «положить карту, которую подмешает кто-то
// другой», и выключенный тон-маппинг забирал свечение с собой. Теперь звено
// само кладёт результат в кадр — как и всякое другое звено тракта.
const char* kAddFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform sampler2D uAdd;
void main() { FragColor = vec4(texture(uScene, vUV).rgb + texture(uAdd, vUV).rgb, 1.0); }
)";

// --- Умножение кадра на карту (затенение) ------------------------------------
const char* kMulFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform sampler2D uMul;
void main() {
    float m = clamp(texture(uMul, vUV).r, 0.0, 1.0);
    FragColor = vec4(texture(uScene, vUV).rgb * m, 1.0);
}
)";

// Финальная копия результата тракта в цель вывода (FBO вьюпорта или экран).
//
// Отдельный проход, а не «последнее звено пишет прямо в выход»: звено обязано
// знать только то, что написано в его описании, — а куда смотрит камера, знает
// вызывающий. Так одно и то же звено годится и для превью в редакторе, и для
// собранной игры, и для второго вьюпорта с другим размером.
const char* kCopyFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
void main() { FragColor = vec4(texture(uScene, vUV).rgb, 1.0); }
)";

Shader& SsaoShader()      { static Shader* s = new Shader(Shader::FromSource(kFsVert, kSsaoFrag, "PostFX.SSAO")); return *s; }
Shader& AoBlurShader()    { static Shader* s = new Shader(Shader::FromSource(kFsVert, kAoBlurFrag, "PostFX.AOBlur")); return *s; }
// Общий блок круга нерезкости подставляется в оба шейдера, которым он нужен:
// считай его каждый по-своему — и граница «в фокусе» у prep и у сведения
// разошлась бы.
std::string DofSource(const char* body) {
    return std::string("#version 330 core\n") + kDofCommon + body;
}
Shader& DofPrepShader()      { static Shader* s = new Shader(Shader::FromSource(kFsVert, DofSource(kDofPrepFrag).c_str(), "PostFX.DoFPrep")); return *s; }
Shader& DofNearMaxShader()   { static Shader* s = new Shader(Shader::FromSource(kFsVert, kDofNearMaxFrag, "PostFX.DoFNearMax")); return *s; }
Shader& DofGatherShader()    { static Shader* s = new Shader(Shader::FromSource(kFsVert, kDofGatherFrag, "PostFX.DoFGather")); return *s; }
Shader& DofCompositeShader() { static Shader* s = new Shader(Shader::FromSource(kFsVert, DofSource(kDofCompositeFrag).c_str(), "PostFX.DoFComposite")); return *s; }
Shader& MotionShader()    { static Shader* s = new Shader(Shader::FromSource(kFsVert, kMotionFrag, "PostFX.MotionBlur")); return *s; }
Shader& BrightShader()    { static Shader* s = new Shader(Shader::FromSource(kFsVert, kBrightFrag, "PostFX.Bright")); return *s; }
Shader& BlurShader()      { static Shader* s = new Shader(Shader::FromSource(kFsVert, kBlurFrag, "PostFX.Blur")); return *s; }
Shader& ExposureShader()  { static Shader* s = new Shader(Shader::FromSource(kFsVert, kExposureFrag, "PostFX.Exposure")); return *s; }
Shader& AutoExposureMeterShader() { static Shader* s = new Shader(Shader::FromSource(kFsVert, kAutoExposureMeterFrag, "PostFX.AutoExposureMeter")); return *s; }
Shader& AutoExposureApplyShader() { static Shader* s = new Shader(Shader::FromSource(kFsVert, kAutoExposureApplyFrag, "PostFX.AutoExposureApply")); return *s; }
Shader& ColorShader()     { static Shader* s = new Shader(Shader::FromSource(kFsVert, kColorFrag, "PostFX.Color")); return *s; }
Shader& GradeShader()     { static Shader* s = new Shader(Shader::FromSource(kFsVert, kGradeFrag, "PostFX.Grade")); return *s; }
Shader& TonemapShader()   { static Shader* s = new Shader(Shader::FromSource(kFsVert, kTonemapFrag, "PostFX.Tonemap")); return *s; }
Shader& VignetteShader()  { static Shader* s = new Shader(Shader::FromSource(kFsVert, kVignetteFrag, "PostFX.Vignette")); return *s; }
Shader& ChromaticShader() { static Shader* s = new Shader(Shader::FromSource(kFsVert, kChromaticFrag, "PostFX.Chromatic")); return *s; }
Shader& GrainShader()     { static Shader* s = new Shader(Shader::FromSource(kFsVert, kGrainFrag, "PostFX.Grain")); return *s; }
Shader& AddShader()       { static Shader* s = new Shader(Shader::FromSource(kFsVert, kAddFrag, "PostFX.Add")); return *s; }
Shader& MulShader()       { static Shader* s = new Shader(Shader::FromSource(kFsVert, kMulFrag, "PostFX.Mul")); return *s; }
Shader& FxaaShader() { static Shader* s = new Shader(Shader::FromSource(kFsVert, kFxaaFrag, "PostFX.FXAA")); return *s; }
Shader& CopyShader() { static Shader* s = new Shader(Shader::FromSource(kFsVert, kCopyFrag, "PostFX.Copy")); return *s; }

std::unique_ptr<RenderTarget> MakeColor(int w, int h) {
    RenderTargetDesc d;
    d.Width = w; d.Height = h; d.Kind = RenderTargetKind::ColorHDR;
    return GraphicsDevice::Get().CreateRenderTarget(d);
}

} // namespace

// ============================================================================
//  Помощники звеньев (объявлены в PostEffect.h)
// ============================================================================

namespace {

GraphicsDevice& Dev(PostContext& ctx) { return *ctx.Device; }

// Полный экран: треугольник без вершинного буфера, рисуется тремя вершинами по
// gl_VertexID (см. kFsVert). Один и тот же для всех звеньев.
void DrawFullscreen(PostContext& ctx) { ctx.Scratch->Fullscreen->DrawArrays(3); }

// Сообщение, которое печатается ОДИН раз за процесс.
//
// Тракт исполняется КАЖДЫЙ кадр, и причина отказа у него одна и та же: без
// этой памяти лог получал бы шестьдесят одинаковых строк в секунду — ровно та
// беда, ради которой в движке уже заведено «печатать один раз» для ошибок
// скриптов. Здесь это тем важнее, что причина — свойство ТРАКТА, а не кадра.
void OnceLog(const std::string& message) {
    static std::vector<std::string> said;
    if (std::find(said.begin(), said.end(), message) != said.end()) return;
    said.push_back(message);
    LOG_WARN("PostFX") << message;
}

// Лениво создаёт буфер звена: полноразмерный HDR-таргет стоит заметной VRAM, а
// глубина резкости включается не в каждом проекте.
RenderTarget* Keep(std::unique_ptr<RenderTarget>& slot, int w, int h) {
    if (!slot) slot = MakeColor(std::max(1, w), std::max(1, h));
    return slot.get();
}

PostParamDesc Param(const char* name, const char* label, float def, float min, float max,
                    const char* hint) {
    PostParamDesc p;
    p.Name = name;
    p.Label = label;
    p.Type = PostParamType::Float;
    p.Min = min;
    p.Max = max;
    p.Default[0] = def;
    p.Hint = hint ? hint : "";
    return p;
}

// Цвет-множитель. Умолчание — белый: «ничего не менять» обязано быть видно
// глазом, а не вычисляться из нулей.
PostParamDesc ColorParam(const char* name, const char* label, const char* hint) {
    PostParamDesc p;
    p.Name = name;
    p.Label = label;
    p.Type = PostParamType::Color;
    p.Min = 0.0f;
    p.Max = 2.0f;
    p.Default[0] = p.Default[1] = p.Default[2] = p.Default[3] = 1.0f;
    p.Hint = hint ? hint : "";
    return p;
}

// Выбор из именованных вариантов: режим кривой, способ смешивания и прочее,
// где число само по себе не значит ничего.
PostParamDesc EnumParam(const char* name, const char* label, int def,
                        std::vector<std::string> options, const char* hint) {
    PostParamDesc p;
    p.Name = name;
    p.Label = label;
    p.Type = PostParamType::Enum;
    p.Min = 0.0f;
    p.Max = (float)(options.size() > 0 ? options.size() - 1 : 0);
    p.Default[0] = (float)def;
    p.Options = std::move(options);
    p.Hint = hint ? hint : "";
    return p;
}

// Галка: включить/выключить часть звена.
PostParamDesc BoolParam(const char* name, const char* label, bool def, const char* hint) {
    PostParamDesc p;
    p.Name = name;
    p.Label = label;
    p.Type = PostParamType::Bool;
    p.Min = 0.0f;
    p.Max = 1.0f;
    p.Default[0] = def ? 1.0f : 0.0f;
    p.Hint = hint ? hint : "";
    return p;
}

// Подпись-разделитель внутри списка параметров звена. Значения у неё нет:
// это способ сгруппировать настройки, не заводя вложенных структур.
PostParamDesc Heading(const char* label) {
    PostParamDesc p;
    p.Label = label;
    p.Type = PostParamType::Heading;
    return p;
}

// --- Звено «Ambient Occlusion» ----------------------------------------------
// Публикует карту postaux::kAO. И радиус, и сила принадлежат ЗВЕНУ: убрали
// звено — уехали оба, и возводить в степень стало некому (см. kAoBlurFrag).
void RunAmbientOcclusion(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("SSAO");
    GraphicsDevice& device = Dev(ctx);
    PostScratch& sc = *ctx.Scratch;
    const glm::vec2 texel(1.0f / (float)ctx.Width, 1.0f / (float)ctx.Height);

    sc.Ao->Bind();
    Shader& ao = SsaoShader();
    ao.Use();
    ao.SetInt("uDepth", 0);
    device.BindTexture2D(0, ctx.SceneDepth);
    ao.SetMat4("uProj", ctx.Proj);
    ao.SetMat4("uInvProj", glm::inverse(ctx.Proj));
    ao.SetFloat("uRadius", e.Float("radius", 0.5f));
    ao.SetVec2("uTexel", texel);
    DrawFullscreen(ctx);

    // Размытие РАЗДЕЛЁННОЕ: горизонталь в AoBlur, вертикаль обратно в Ao. Два
    // прохода по 9 выборок вместо одного по 16 — и дешевле, и шире окно
    // (сглаживает шум выборки, который квадрат 4x4 оставлял).
    Shader& aob = AoBlurShader();
    aob.Use();
    aob.SetInt("uAO", 0);
    aob.SetVec2("uTexel", texel);
    aob.SetFloat("uStrength", glm::max(e.Float("strength", 1.0f), 0.01f));

    sc.AoBlur->Bind();
    aob.SetVec2("uDirection", glm::vec2(1.0f, 0.0f));
    aob.SetInt("uApplyStrength", 0);
    device.BindTexture2D(0, sc.Ao->ColorTextureHandle());
    DrawFullscreen(ctx);

    sc.Ao->Bind();
    aob.SetVec2("uDirection", glm::vec2(0.0f, 1.0f));
    aob.SetInt("uApplyStrength", 1);
    device.BindTexture2D(0, sc.AoBlur->ColorTextureHandle());
    DrawFullscreen(ctx);

    // Готовая карта лежит в Ao: вертикальный проход пишет обратно в него.
    // Карта ещё и публикуется — звену ИГРЫ она может понадобиться (см.
    // postaux), — но применяет её к кадру САМО звено: «эффект, который ничего
    // не меняет, пока его не подмешает кто-то другой», — это не звено тракта.
    PostPublishAux(ctx, postaux::kAO, sc.Ao->ColorTextureHandle());

    RenderTarget* out = PostAcquireTarget(ctx, 1);
    out->Bind();
    Shader& mul = MulShader();
    mul.Use();
    mul.SetInt("uScene", 0);
    mul.SetInt("uMul", 1);
    device.BindTexture2D(0, ctx.Color);
    device.BindTexture2D(1, sc.Ao->ColorTextureHandle());
    DrawFullscreen(ctx);
    PostSetColor(ctx, out->ColorTextureHandle(), ctx.ColorIsLdr);
}

// --- Звено «Depth of Field» --------------------------------------------------
void RunDepthOfField(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Глубина резкости");
    GraphicsDevice& device = Dev(ctx);
    PostScratch& sc = *ctx.Scratch;
    const int hw = std::max(1, ctx.Width / 2), hh = std::max(1, ctx.Height / 2);
    RenderTarget* prep = Keep(sc.DofPrep, hw, hh);
    RenderTarget* farT = Keep(sc.DofBlur, hw, hh);
    RenderTarget* nearA = Keep(sc.DofNearMaxA, hw, hh);
    RenderTarget* nearB = Keep(sc.DofNearMaxB, hw, hh);
    RenderTarget* nearT = Keep(sc.DofNear, hw, hh);
    RenderTarget* out = PostAcquireTarget(ctx, 1);

    const float maxRadius = glm::clamp(e.Float("maxRadius", 16.0f), 0.0f, 64.0f);
    const glm::mat4 invProj = glm::inverse(ctx.Proj);
    auto setCommon = [&](Shader& sh) {
        sh.SetInt("uDepth", 1);
        sh.SetMat4("uInvProj", invProj);
        sh.SetFloat("uFocus", glm::max(e.Float("focus", 10.0f), 0.01f));
        sh.SetInt("uAutoFocus", e.Int("focusMode", 0) == 1 ? 1 : 0);
        sh.SetFloat("uFocusRange", glm::max(e.Float("focusRange", 0.0f), 0.0f));
        sh.SetFloat("uAperture", glm::max(e.Float("aperture", 2.8f), 0.5f));
        sh.SetFloat("uFocalLength", glm::clamp(e.Float("focalLength", 85.0f), 1.0f, 1000.0f));
        sh.SetFloat("uNearScale", glm::max(e.Float("nearBlur", 1.0f), 0.0f));
        sh.SetFloat("uFarScale", glm::max(e.Float("farBlur", 1.0f), 0.0f));
        sh.SetFloat("uMaxRadius", maxRadius);
        sh.SetFloat("uFullHeight", (float)ctx.Height);
        device.BindTexture2D(1, ctx.SceneDepth);
    };
    static const int kSamples[] = {32, 64, 128};
    const int maxSamples = kSamples[glm::clamp(e.Int("quality", 1), 0, 2)];
    const glm::vec2 halfTexel(1.0f / (float)hw, 1.0f / (float)hh);

    // 1. Префильтр + знаковый круг.
    prep->Bind();
    Shader& dprep = DofPrepShader();
    dprep.Use();
    dprep.SetInt("uScene", 0);
    device.BindTexture2D(0, ctx.Color);
    setCommon(dprep);
    dprep.SetVec2("uFullTexel", glm::vec2(1.0f / (float)ctx.Width, 1.0f / (float)ctx.Height));
    DrawFullscreen(ctx);

    // 2. Досягаемость ближнего плана: максимум по горизонтали, затем по вертикали.
    const int reach = glm::clamp((int)std::ceil(maxRadius * 0.5f), 1, 32);
    Shader& nmax = DofNearMaxShader();
    nmax.Use();
    nmax.SetInt("uSrc", 0);
    nmax.SetInt("uReach", reach);
    nearA->Bind();
    device.BindTexture2D(0, prep->ColorTextureHandle());
    nmax.SetVec2("uStep", glm::vec2(halfTexel.x, 0.0f));
    nmax.SetInt("uFromPrep", 1);
    DrawFullscreen(ctx);
    nearB->Bind();
    device.BindTexture2D(0, nearA->ColorTextureHandle());
    nmax.SetVec2("uStep", glm::vec2(0.0f, halfTexel.y));
    nmax.SetInt("uFromPrep", 0);
    DrawFullscreen(ctx);

    // 3–4. Дальний и ближний планы.
    Shader& gather = DofGatherShader();
    gather.Use();
    gather.SetInt("uPrep", 0);
    gather.SetInt("uNearMax", 1);
    gather.SetVec2("uTexel", halfTexel);
    gather.SetInt("uMaxSamples", maxSamples);
    device.BindTexture2D(0, prep->ColorTextureHandle());
    device.BindTexture2D(1, nearB->ColorTextureHandle());
    farT->Bind();
    gather.SetInt("uNear", 0);
    DrawFullscreen(ctx);
    nearT->Bind();
    gather.SetInt("uNear", 1);
    DrawFullscreen(ctx);

    // 5. Сведение.
    out->Bind();
    Shader& dcomp = DofCompositeShader();
    dcomp.Use();
    dcomp.SetInt("uScene", 0);
    dcomp.SetInt("uFar", 2);
    dcomp.SetInt("uNearLayer", 3);
    dcomp.SetInt("uShowFocus", e.Bool("showFocus", false) ? 1 : 0);
    dcomp.SetVec2("uHalfTexel", halfTexel);
    device.BindTexture2D(0, ctx.Color);
    device.BindTexture2D(2, farT->ColorTextureHandle());
    device.BindTexture2D(3, nearT->ColorTextureHandle());
    setCommon(dcomp);
    DrawFullscreen(ctx);

    PostSetColor(ctx, out->ColorTextureHandle(), /*ldr=*/false);
}

// --- Звено «Motion Blur» -----------------------------------------------------
void RunMotionBlur(PostContext& ctx, const PostEffect& e) {
    const bool useVelocity = ctx.Velocity.Valid();
    // С буфером скоростей история прошлого кадра НЕ нужна: её хранит проход
    // геометрии, у каждой сущности свою. Запасной путь без буфера сравнивает
    // матрицу этого кадра с матрицей прошлого и потому без истории невозможен.
    const bool useHistory = ctx.Scratch->HasPrevFrame;
    if (!useVelocity && !useHistory) {
        OnceLog("звено «Motion Blur» пропущено: нет ни буфера скоростей, ни прошлого кадра");
        return;
    }
    const float amount = e.Float("amount", 0.5f);
    if (amount <= 0.001f) return;

    SAGE_PROFILE("Смаз движения");
    GraphicsDevice& device = Dev(ctx);
    RenderTarget* out = PostAcquireTarget(ctx, 1);
    out->Bind();
    Shader& sh = MotionShader();
    sh.Use();
    sh.SetInt("uScene", 0);
    sh.SetInt("uDepth", 1);
    sh.SetInt("uVelocity", 2);
    device.BindTexture2D(0, ctx.Color);
    device.BindTexture2D(1, ctx.SceneDepth);
    // Сэмплер обязан быть привязан всегда, даже когда путь не выбран:
    // непривязанный юнит на части драйверов читается как чёрная текстура,
    // а на части — как мусор из чужого прохода.
    device.BindTexture2D(2, useVelocity ? ctx.Velocity : ctx.SceneDepth);
    sh.SetInt("uUseVelocity", useVelocity ? 1 : 0);
    sh.SetMat4("uInvViewProj", glm::inverse(ctx.Proj * ctx.View));
    sh.SetMat4("uPrevViewProj", ctx.Scratch->PrevViewProj);
    sh.SetFloat("uAmount", amount);
    sh.SetInt("uSamples", glm::clamp(e.Int("samples", 12), 2, 32));
    sh.SetFloat("uMaxLen", glm::clamp(e.Float("maxLength", 5.0f), 0.1f, 25.0f) * 0.01f);
    DrawFullscreen(ctx);

    PostSetColor(ctx, out->ColorTextureHandle(), /*ldr=*/false);
}

// --- Звено «Lens Flare»: блик солнца в объективе ----------------------------
// Раньше жил в НАСТРОЙКАХ ПРОЕКТА и рисовался мимо тракта камеры: выключатель
// компонента «Пост-обработка» его не гасил, а в редакторе его нельзя было ни
// включить, ни настроить. Теперь это звено, как все остальные.
void RunLensFlare(PostContext& ctx, const PostEffect& e) {
    if (!ctx.Lighting) {
        OnceLog("звено «Lens Flare» пропущено: кадру не передано освещение (PostFX::SetLighting)");
        return;
    }
    const float intensity = e.Float("intensity", 1.0f);
    if (intensity <= 0.0f) return;
    PostScratch& sc = *ctx.Scratch;
    if (!sc.Flare) sc.Flare = std::make_shared<LensFlare>();
    GraphicsDevice& device = Dev(ctx);

    // Копия кадра в свою цель, блик — поверх неё сложением.
    RenderTarget* out = PostAcquireTarget(ctx, 1);
    out->Bind();
    Shader& copy = CopyShader();
    copy.Use();
    copy.SetInt("uScene", 0);
    device.BindTexture2D(0, ctx.Color);
    DrawFullscreen(ctx);

    LensFlareSettings s;
    s.Enabled = true;
    s.Intensity = intensity;
    s.Ghosts = glm::clamp(e.Int("ghosts", 6), 0, 12);
    s.GhostSpacing = e.Float("ghostSpacing", 0.32f);
    s.GhostSize = e.Float("ghostSize", 0.055f);
    s.ApertureBlades = glm::clamp(e.Int("blades", 6), 0, 12);
    s.Halo = e.Float("halo", 0.55f);
    s.HaloRadius = e.Float("haloRadius", 0.42f);
    s.Starburst = e.Float("starburst", 0.5f);
    s.Glare = e.Float("glare", 0.45f);
    s.Chroma = e.Float("chroma", 0.55f);
    s.Threshold = e.Float("threshold", 1.1f);
    // Видимость солнца — по кадру СЦЕНЫ, а не по уже обработанному: порог
    // «это солнце» задан в яркости сцены, до экспозиции.
    sc.Flare->Render(*out, ctx.SceneColor, ctx.SceneDepth, ctx.Width, ctx.Height, ctx.Proj,
                     ctx.View, *ctx.Lighting, s);
    // Блик возвращает тест глубины — тракт работает без него.
    device.SetDepthTest(false);
    device.SetBlend(false);
    device.SetViewport(0, 0, ctx.Width, ctx.Height);
    PostSetColor(ctx, out->ColorTextureHandle(), /*ldr=*/false);
}

// --- Звено «Bloom» -----------------------------------------------------------
// Публикует карту postaux::kBloom. Порог И сила принадлежат звену: сила
// умножается на последнем проходе размытия, а не тем, кто это свечение
// подмешивает (см. kBlurFrag).
void RunBloom(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Bloom");
    GraphicsDevice& device = Dev(ctx);
    PostScratch& sc = *ctx.Scratch;
    const int hw = sc.Bright->Width(), hh = sc.Bright->Height();

    sc.Bright->Bind();
    Shader& br = BrightShader();
    br.Use();
    br.SetInt("uScene", 0);
    device.BindTexture2D(0, ctx.Color);
    br.SetFloat("uThreshold", e.Float("threshold", 1.0f));
    DrawFullscreen(ctx);

    Shader& blur = BlurShader();
    blur.Use();
    blur.SetInt("uTex", 0);
    // РАДИУС — шаг между выборками размытия. Он принадлежит звену: «свечение
    // шире» и «свечение ярче» — разные желания, и одной силой их не выразить.
    const float radius = glm::clamp(e.Float("radius", 1.0f), 0.25f, 4.0f);
    const glm::vec2 texel(radius / (float)hw, radius / (float)hh);
    const float intensity = e.Float("intensity", 0.55f);
    sage::rhi::TextureHandle src = sc.Bright->ColorTextureHandle();
    RenderTarget* dstA = sc.BloomA.get();
    RenderTarget* dstB = sc.BloomB.get();
    for (int i = 0; i < 2; ++i) {
        const bool last = (i == 1);
        dstA->Bind(); // горизонтальный
        device.BindTexture2D(0, src);
        blur.SetVec2("uDir", glm::vec2(texel.x, 0.0f));
        blur.SetInt("uApplyScale", 0);
        DrawFullscreen(ctx);
        dstB->Bind(); // вертикальный
        device.BindTexture2D(0, dstA->ColorTextureHandle());
        blur.SetVec2("uDir", glm::vec2(0.0f, texel.y));
        blur.SetFloat("uScale", intensity);
        blur.SetInt("uApplyScale", last ? 1 : 0);
        DrawFullscreen(ctx);
        src = dstB->ColorTextureHandle();
    }
    PostPublishAux(ctx, postaux::kBloom, src);

    // И СРАЗУ В КАДР. Раньше свечение только публиковалось, а складывал его с
    // кадром тон-маппинг — то есть выключенный тон-маппинг забирал свечение с
    // собой, а «сила свечения» жила в одном звене, а применялась в другом.
    RenderTarget* out = PostAcquireTarget(ctx, 1);
    out->Bind();
    Shader& add = AddShader();
    add.Use();
    add.SetInt("uScene", 0);
    add.SetInt("uAdd", 1);
    device.BindTexture2D(0, ctx.Color);
    device.BindTexture2D(1, src);
    DrawFullscreen(ctx);
    PostSetColor(ctx, out->ColorTextureHandle(), /*ldr=*/false);
}

// --- Простое звено: один полноэкранный проход по текущей картинке -----------
//
// Общее тело для всех звеньев, которым нужен ровно один проход: цель из пула,
// исходник нулевым юнитом, свои uniform-ы — и результат становится текущим
// кадром. Без него каждое такое звено повторяло бы восемь одинаковых строк, и
// ошибиться в них можно было бы по-разному.
void RunSimplePass(PostContext& ctx, Shader& shader, bool ldrOut,
                   const std::function<void(Shader&)>& setup) {
    GraphicsDevice& device = Dev(ctx);
    RenderTarget* out = PostAcquireTarget(ctx, 1);
    out->Bind();
    shader.Use();
    shader.SetInt("uScene", 0);
    setup(shader);
    device.BindTexture2D(0, ctx.Color);
    DrawFullscreen(ctx);
    PostSetColor(ctx, out->ColorTextureHandle(), ldrOut);
}

// --- Звено «Exposure»: сколько света собрал кадр ----------------------------
void RunExposure(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Экспозиция");
    RunSimplePass(ctx, ExposureShader(), /*ldrOut=*/false, [&](Shader& sh) {
        sh.SetFloat("uEV", e.Float("exposure", 0.0f));
    });
}

// --- Звено «Auto Exposure»: глаз привыкает к свету сцены -------------------
void RunAutoExposure(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Автоэкспозиция");
    GraphicsDevice& device = Dev(ctx);
    PostScratch& sc = *ctx.Scratch;
    for (auto& t : sc.Adapt)
        if (!t) t = MakeColor(1, 1);

    // Шаг времени — по часам тракта; первый кадр и длинный разрыв (свернули
    // окно, стояли на точке останова) не должны раскачивать экспозицию.
    float dt = sc.HasAdapt ? ctx.Time - sc.AdaptTime : 0.0f;
    dt = glm::clamp(dt, 0.0f, 0.25f);
    sc.AdaptTime = ctx.Time;

    const int prev = sc.AdaptIndex, next = 1 - sc.AdaptIndex;
    sc.Adapt[next]->Bind();
    Shader& meter = AutoExposureMeterShader();
    meter.Use();
    meter.SetInt("uScene", 0);
    meter.SetInt("uPrev", 1);
    meter.SetInt("uHasPrev", sc.HasAdapt ? 1 : 0);
    meter.SetFloat("uDt", dt);
    meter.SetFloat("uSpeedUp", e.Float("speedUp", 3.0f));
    meter.SetFloat("uSpeedDown", e.Float("speedDown", 1.0f));
    const float minEv = e.Float("minEV", -8.0f), maxEv = e.Float("maxEV", 10.0f);
    meter.SetFloat("uMinEV", glm::min(minEv, maxEv));
    meter.SetFloat("uMaxEV", glm::max(minEv, maxEv));
    meter.SetFloat("uLow", glm::clamp(e.Float("lowPercent", 10.0f), 0.0f, 99.0f) * 0.01f);
    meter.SetFloat("uHigh", glm::clamp(e.Float("highPercent", 90.0f), 1.0f, 100.0f) * 0.01f);
    meter.SetFloat("uBlack", e.Float("blackLevel", -10.0f));
    // «Без поправки» — адаптация, при которой кадр не меняется: 0.18·2^(c−a) = 1.
    meter.SetFloat("uNeutral", e.Float("compensation", 1.0f) + std::log2(0.18f));
    device.BindTexture2D(0, ctx.Color);
    device.BindTexture2D(1, sc.Adapt[prev]->ColorTextureHandle());
    DrawFullscreen(ctx);
    sc.AdaptIndex = next;
    sc.HasAdapt = true;

    RenderTarget* out = PostAcquireTarget(ctx, 1);
    out->Bind();
    Shader& apply = AutoExposureApplyShader();
    apply.Use();
    apply.SetInt("uScene", 0);
    apply.SetInt("uAdapted", 1);
    apply.SetFloat("uCompensation", e.Float("compensation", 1.0f));
    device.BindTexture2D(0, ctx.Color);
    device.BindTexture2D(1, sc.Adapt[next]->ColorTextureHandle());
    DrawFullscreen(ctx);
    PostSetColor(ctx, out->ColorTextureHandle(), /*ldr=*/false);
}

// --- Звено «Color»: яркость, контраст, насыщенность, температура ------------
void RunColor(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Цвет");
    RunSimplePass(ctx, ColorShader(), /*ldrOut=*/false, [&](Shader& sh) {
        sh.SetFloat("uBrightness", glm::max(e.Float("brightness", 1.0f), 0.0f));
        sh.SetFloat("uContrast", glm::max(e.Float("contrast", 1.0f), 0.01f));
        sh.SetFloat("uSaturation", glm::max(e.Float("saturation", 1.0f), 0.0f));
        sh.SetFloat("uTemperature", e.Float("temperature", 0.0f));
        sh.SetFloat("uTint", e.Float("tint", 0.0f));
    });
}

// --- Звено «Color Grading»: тени, средние, света ----------------------------
void RunGrading(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Цветокоррекция");
    auto rgb = [&e](const char* name) {
        const PostValue* v = e.Find(name);
        return v ? glm::vec3(v->V[0], v->V[1], v->V[2]) : glm::vec3(1.0f);
    };
    RunSimplePass(ctx, GradeShader(), /*ldrOut=*/false, [&](Shader& sh) {
        sh.SetVec3("uShadows", rgb("shadows"));
        sh.SetVec3("uMidtones", rgb("midtones"));
        sh.SetVec3("uHighlights", rgb("highlights"));
    });
}

// --- Звено «Tonemapping»: HDR -> готовый к показу кадр ----------------------
// ЕДИНСТВЕННОЕ звено каталога, переводящее кадр из HDR в LDR (см. Tonemaps).
void RunTonemap(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Тон-маппинг");
    RunSimplePass(ctx, TonemapShader(), /*ldrOut=*/true, [&](Shader& sh) {
        sh.SetInt("uMode", glm::clamp(e.Int("mode", 2), 0, 6));
        sh.SetFloat("uGamma", glm::max(e.Float("gamma", 2.2f), 0.01f));
        sh.SetInt("uOutput", glm::clamp(e.Int("output", 0), 0, 2));
    });
}

// --- Звено «Vignette» -------------------------------------------------------
void RunVignette(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Виньетка");
    RunSimplePass(ctx, VignetteShader(), /*ldrOut=*/true, [&](Shader& sh) {
        sh.SetFloat("uIntensity", glm::clamp(e.Float("intensity", 0.35f), 0.0f, 1.0f));
        sh.SetFloat("uSmoothness", glm::clamp(e.Float("smoothness", 0.6f), 0.01f, 1.0f));
    });
}

// --- Звено «Chromatic Aberration» -------------------------------------------
void RunChromatic(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Хроматическая аберрация");
    RunSimplePass(ctx, ChromaticShader(), /*ldrOut=*/true, [&](Shader& sh) {
        sh.SetFloat("uAmount", glm::max(e.Float("amount", 0.3f), 0.0f));
    });
}

// --- Звено «Film Grain» -----------------------------------------------------
void RunGrain(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("Зерно плёнки");
    RunSimplePass(ctx, GrainShader(), /*ldrOut=*/true, [&](Shader& sh) {
        sh.SetVec2("uResolution", glm::vec2((float)ctx.Width, (float)ctx.Height));
        sh.SetFloat("uAmount", glm::clamp(e.Float("amount", 0.12f), 0.0f, 1.0f));
        sh.SetFloat("uSize", glm::clamp(e.Float("size", 1.5f), 0.5f, 8.0f));
        sh.SetFloat("uTime", ctx.Time);
    });
}

// --- Звено «FXAA» ------------------------------------------------------------
void RunFxaa(PostContext& ctx, const PostEffect& e) {
    SAGE_PROFILE("FXAA");
    GraphicsDevice& device = Dev(ctx);
    RenderTarget* out = PostAcquireTarget(ctx, 1);
    out->Bind();
    Shader& fxaa = FxaaShader();
    fxaa.Use();
    fxaa.SetInt("uTex", 0);
    fxaa.SetVec2("uTexel",
                 glm::vec2(1.0f / (float)ctx.Width, 1.0f / (float)ctx.Height));
    fxaa.SetFloat("uContrastThreshold", glm::max(e.Float("threshold", 0.0625f), 0.0f));
    device.BindTexture2D(0, ctx.Color);
    DrawFullscreen(ctx);
    PostSetColor(ctx, out->ColorTextureHandle(), /*ldr=*/true);
}

} // namespace

RenderTarget* PostAcquireTarget(PostContext& ctx, int divisor) {
    PostScratch& sc = *ctx.Scratch;
    if (divisor < 1) divisor = 1;
    for (PostScratch::Pooled& entry : sc.Pool) {
        if (entry.Divisor != divisor) continue;
        // Цель, которую звено ЧИТАЕТ, брать нельзя: запись в неё затёрла бы вход.
        // Тракт линеен, поэтому этого одного правила достаточно — цель, бывшая
        // входом предыдущего звена, к этому моменту уже свободна.
        if (entry.Target->ColorTextureHandle() == ctx.Color) continue;
        return entry.Target.get();
    }
    // Свободных нет — заводим ещё одну. Так длина тракта не упирается в
    // заранее угаданное число буферов.
    PostScratch::Pooled fresh;
    fresh.Divisor = divisor;
    fresh.Target = MakeColor(std::max(1, sc.Width / divisor), std::max(1, sc.Height / divisor));
    sc.Pool.push_back(std::move(fresh));
    return sc.Pool.back().Target.get();
}

void PostDrawFullscreen(PostContext& ctx) { ctx.Scratch->Fullscreen->DrawArrays(3); }

void PostPublishAux(PostContext& ctx, const char* name, sage::rhi::TextureHandle texture) {
    if (!name) return;
    for (std::pair<std::string, sage::rhi::TextureHandle>& entry : ctx.Aux) {
        if (entry.first == name) {
            entry.second = texture;
            return;
        }
    }
    ctx.Aux.emplace_back(name, texture);
}

sage::rhi::TextureHandle PostAuxTexture(const PostContext& ctx, const char* name) {
    if (!name) return {};
    for (const std::pair<std::string, sage::rhi::TextureHandle>& entry : ctx.Aux)
        if (entry.first == name) return entry.second;
    return {};
}

void PostSetColor(PostContext& ctx, sage::rhi::TextureHandle texture, bool ldr) {
    ctx.Color = texture;
    ctx.ColorIsLdr = ldr;
}

// ============================================================================
//  Встроенные виды эффектов
// ============================================================================

void RegisterBuiltinPostEffects(PostEffectCatalog& catalog) {
    // ПОРЯДОК РЕГИСТРАЦИИ — ПОРЯДОК ОБРАБОТКИ. Список «что можно добавить» в
    // инспекторе идёт отсюда, и он обязан читаться как путь кадра: экспозиция,
    // глубина, свечение, цвет, тон-маппинг, оптика, плёнка, вывод.
    {
        PostEffectKind k;
        k.Id = "exposure";
        k.Label = "Exposure";
        k.Hint = "Сколько света собрал кадр. В ступенях: +1 — вдвое светлее";
        k.Stage = PostStage::Exposure;
        k.Params = {Param("exposure", "Exposure", 0.0f, -6.0f, 6.0f, "Ступени экспозиции (EV)")};
        k.Run = RunExposure;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "autoexposure";
        k.Label = "Auto Exposure";
        k.Hint = "Адаптация глаза: кадр сам подстраивается под яркость сцены —\n"
                 "в пещере светлеет, на солнце темнеет, плавно";
        k.Stage = PostStage::Exposure;
        k.Params = {Param("compensation", "Compensation", 1.0f, -4.0f, 4.0f,
                          "Поправка в ступенях: +1 — вдвое светлее"),
                    Param("minEV", "Min Brightness", -8.0f, -16.0f, 16.0f,
                          "Темнее этого (log2 яркости) сцену не вытягивать"),
                    Param("maxEV", "Max Brightness", 10.0f, -16.0f, 16.0f,
                          "Ярче этого (log2 яркости) сцену не притушивать"),
                    Param("lowPercent", "Low Percent", 10.0f, 0.0f, 99.0f,
                          "Сколько процентов самых тёмных точек кадра не учитывать"),
                    Param("highPercent", "High Percent", 90.0f, 1.0f, 100.0f,
                          "Выше какого процента яркости точки не учитывать (блики, лампы)"),
                    Param("blackLevel", "Black Level", -10.0f, -16.0f, 0.0f,
                          "Точки темнее этого (log2 яркости) — пустота: не участвуют в замере.\n"
                          "Иначе чёрное небо заставляет глаз вытягивать кадр в белое"),
                    Param("speedUp", "Speed Up", 3.0f, 0.0f, 20.0f,
                          "Как быстро глаз привыкает к свету (1/с)"),
                    Param("speedDown", "Speed Down", 1.0f, 0.0f, 20.0f,
                          "Как быстро глаз привыкает к темноте (1/с)")};
        k.Run = RunAutoExposure;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "ao";
        k.Label = "Ambient Occlusion";
        k.Hint = "Затемняет щели и места контакта предметов; считается по глубине кадра";
        k.Needs = PostNeeds::HdrColor | PostNeeds::Depth;
        k.Stage = PostStage::Depth;
        k.ProducesAux = postaux::kAO;
        k.Params = {Param("radius", "Radius", 0.5f, 0.05f, 2.0f, "Радиус выборки в метрах"),
                    Param("strength", "Strength", 1.0f, 0.0f, 4.0f, "Во сколько раз усилить затемнение")};
        k.Run = RunAmbientOcclusion;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "dof";
        k.Label = "Depth of Field";
        k.Hint = "Размывает то, что дальше или ближе плоскости фокуса";
        k.Needs = PostNeeds::HdrColor | PostNeeds::Depth;
        k.Stage = PostStage::Depth;
        k.Params = {
            EnumParam("focusMode", "Focus", 0, {"Manual", "Screen centre"},
                      "Вручную — на дистанции ниже; по центру — на предмет в центре кадра"),
            Param("focus", "Focus Distance", 10.0f, 0.05f, 1000.0f, "Расстояние до плоскости фокуса, м"),
            Param("focusRange", "Focus Range", 0.0f, 0.0f, 100.0f,
                  "Толщина резкого слоя вокруг фокуса, м: всё внутри — резкое"),
            Param("aperture", "Aperture", 2.8f, 0.7f, 22.0f, "f-число: меньше — сильнее размытие"),
            Param("focalLength", "Focal Length", 85.0f, 10.0f, 300.0f,
                  "Фокусное расстояние объектива, мм: длиннее — сильнее размытие фона"),
            Param("nearBlur", "Near Blur", 1.0f, 0.0f, 4.0f, "Сила размытия ближе фокуса (0 — нет)"),
            Param("farBlur", "Far Blur", 1.0f, 0.0f, 4.0f, "Сила размытия дальше фокуса (0 — нет)"),
            Param("maxRadius", "Max Radius", 16.0f, 0.0f, 64.0f, "Потолок радиуса в пикселях"),
            EnumParam("quality", "Quality", 1, {"Low", "Medium", "High"},
                      "Сколько выборок на пиксель размытия: выше — ровнее боке, дороже"),
            BoolParam("showFocus", "Show Focus", false,
                      "Раскрасить кадр: в фокусе — зелёный, ближе — синий, дальше — оранжевый")};
        k.Run = RunDepthOfField;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "motionblur";
        k.Label = "Motion Blur";
        k.Hint = "Смаз движения камеры (и объектов, если у кадра есть буфер скоростей)";
        k.Needs = PostNeeds::HdrColor | PostNeeds::Depth;
        k.Stage = PostStage::Depth;
        k.Params = {Param("amount", "Amount", 0.5f, 0.0f, 1.0f,
                          "Доля движения за кадр, которую размазать (выдержка затвора)"),
                    Param("samples", "Samples", 12.0f, 2.0f, 32.0f, "Выборок вдоль вектора"),
                    Param("maxLength", "Max Length", 5.0f, 0.1f, 25.0f,
                          "Потолок длины смаза, % ширины кадра")};
        k.Run = RunMotionBlur;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "lensflare";
        k.Label = "Lens Flare";
        k.Hint = "Блик солнца в объективе: призраки, ореол, лучи. Гаснет, когда солнце\n"
                 "закрыто предметом или ушло за кадр";
        k.Needs = PostNeeds::HdrColor | PostNeeds::Depth;
        k.Stage = PostStage::Depth;
        k.Params = {Param("intensity", "Intensity", 1.0f, 0.0f, 4.0f, "Сила блика"),
                    Param("ghosts", "Ghosts", 6.0f, 0.0f, 12.0f, "Сколько переотражений"),
                    Param("ghostSpacing", "Ghost Spacing", 0.32f, 0.05f, 1.0f,
                          "Шаг призраков по линии «солнце — центр кадра»"),
                    Param("ghostSize", "Ghost Size", 0.055f, 0.01f, 0.3f, "Размер первого призрака"),
                    Param("blades", "Aperture Blades", 6.0f, 0.0f, 12.0f,
                          "Лепестков диафрагмы: форма призраков (0 — круглые)"),
                    Param("halo", "Halo", 0.55f, 0.0f, 2.0f, "Сила кольца"),
                    Param("haloRadius", "Halo Radius", 0.42f, 0.05f, 1.0f, "Радиус кольца"),
                    Param("starburst", "Starburst", 0.5f, 0.0f, 2.0f, "Лучи вокруг солнца"),
                    Param("glare", "Glare", 0.45f, 0.0f, 2.0f, "Мягкое свечение вокруг солнца"),
                    Param("chroma", "Chroma", 0.55f, 0.0f, 2.0f, "Радужность призраков"),
                    Param("threshold", "Threshold", 1.1f, 0.0f, 16.0f,
                          "Яркость, начиная с которой пиксель — это солнце")};
        k.Run = RunLensFlare;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "bloom";
        k.Label = "Bloom";
        k.Hint = "Свечение ярких участков; считается до тон-маппинга";
        k.Stage = PostStage::Bloom;
        k.ProducesAux = postaux::kBloom;
        k.Params = {Param("intensity", "Bloom Intensity", 0.55f, 0.0f, 4.0f,
                          "Сила добавляемого свечения"),
                    Param("threshold", "Bloom Threshold", 1.0f, 0.0f, 8.0f,
                          "Яркость, выше которой пиксель светится"),
                    Param("radius", "Bloom Radius", 1.0f, 0.25f, 4.0f,
                          "Ширина свечения: шаг размытия")};
        k.Run = RunBloom;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "color";
        k.Label = "Color Adjustments";
        k.Hint = "Яркость, контраст, насыщенность и баланс белого — до тон-маппинга";
        k.Stage = PostStage::Color;
        k.Params = {
            Param("brightness", "Brightness", 1.0f, 0.0f, 2.0f, "Множитель яркости"),
            Param("contrast", "Contrast", 1.0f, 0.0f, 2.0f, "Контраст вокруг средне-серого"),
            Param("saturation", "Saturation", 1.0f, 0.0f, 2.0f, "Насыщенность"),
            Param("temperature", "Temperature", 0.0f, -1.0f, 1.0f, "Теплее (+) или холоднее (-)"),
            Param("tint", "Tint", 0.0f, -1.0f, 1.0f, "В зелёный (+) или в пурпурный (-)")};
        k.Run = RunColor;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "grading";
        k.Label = "Color Grading";
        k.Hint = "Свой оттенок теням, средним тонам и светам";
        k.Stage = PostStage::Grading;
        k.Params = {ColorParam("shadows", "Shadows", "Оттенок тёмных участков"),
                    ColorParam("midtones", "Midtones", "Оттенок средних тонов"),
                    ColorParam("highlights", "Highlights", "Оттенок светлых участков")};
        k.Run = RunGrading;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "tonemap";
        k.Label = "Tonemapping";
        k.Hint = "Переводит HDR-кадр в готовый к показу: кривая света и гамма";
        k.Stage = PostStage::Tonemap;
        k.Tonemaps = true;
        // Номера кривых только ДОПИСЫВАЮТСЯ в конец: сохранённые сцены хранят
        // номер, и вставка в середину молча сменила бы им кривую.
        // Умолчание звена — ACES, как было: сохранённые сцены не хранят
        // нетронутые значения, и смена умолчания молча перекрасила бы их.
        // Кинематографическую кривую получает НОВАЯ камера (см. PostChain::Default).
        k.Params = {EnumParam("mode", "Mode", 2,
                              {"Clamp", "Reinhard", "ACES", "Filmic", "Cinematic", "AgX", "Neutral"},
                              "Кривая света: чем переводить HDR в картинку.\n"
                              "Cinematic — мягкая плёночная кривая (у новой камеры), AgX — мягкие света,\n"
                              "Neutral — Khronos PBR Neutral: цвет материала без искажений"),
                    EnumParam("output", "Output", 0, {"sRGB", "Gamma", "Linear"},
                              "Кодирование для экрана: sRGB — стандарт мониторов,\n"
                              "Gamma — степенная кривая с показателем ниже,\n"
                              "Linear — без кодирования (для внешней обработки)"),
                    Param("gamma", "Gamma", 2.2f, 1.0f, 3.0f, "Показатель для вывода Gamma")};
        k.Run = RunTonemap;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "vignette";
        k.Label = "Vignette";
        k.Hint = "Затемнение к краям кадра";
        k.Needs = PostNeeds::LdrColor;
        k.Stage = PostStage::Lens;
        k.Params = {Param("intensity", "Intensity", 0.35f, 0.0f, 1.0f, "Насколько темнеет край"),
                    Param("smoothness", "Smoothness", 0.6f, 0.01f, 1.0f, "Мягкость перехода")};
        k.Run = RunVignette;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "chromatic";
        k.Label = "Chromatic Aberration";
        k.Hint = "Расхождение каналов к краям кадра — как в настоящей оптике";
        k.Needs = PostNeeds::LdrColor;
        k.Stage = PostStage::Lens;
        k.Params = {Param("amount", "Amount", 0.3f, 0.0f, 1.0f, "Сила расхождения")};
        k.Run = RunChromatic;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "grain";
        k.Label = "Film Grain";
        k.Hint = "Зерно плёнки: живой шум поверх готового кадра";
        k.Needs = PostNeeds::LdrColor;
        k.Stage = PostStage::Film;
        k.Params = {Param("amount", "Amount", 0.12f, 0.0f, 1.0f, "Сила зерна"),
                    Param("size", "Size", 1.5f, 0.5f, 8.0f, "Размер зерна в пикселях")};
        k.Run = RunGrain;
        catalog.Register(std::move(k));
    }
    {
        PostEffectKind k;
        k.Id = "fxaa";
        k.Label = "FXAA";
        k.Hint = "Сглаживает кромки по готовой картинке. С включённым MSAA не нужен: "
                 "он лечит то же самое и только размоет кадр";
        k.Needs = PostNeeds::LdrColor;
        k.Stage = PostStage::Output;
        k.Params = {Param("threshold", "Contrast Threshold", 0.0625f, 0.0f, 0.5f,
                          "Ниже этого перепада пиксель не считается кромкой")};
        k.Run = RunFxaa;
        catalog.Register(std::move(k));
    }
}

// Тракт НОВОГО компонента «Пост-обработка».
//
// Здесь, а не в PostEffect.cpp, ровно по той же причине, что и раньше: форма
// умолчания неотделима от того, какие виды зарегистрировал движок, — имена
// звеньев и их параметры заданы прямо выше.
//
// Что в нём есть: экспозиция, адаптация глаза, свечение, цвет и тон-маппинг
// кинематографической кривой (у самого звена умолчание ACES — ради старых
// сцен). То есть то, что делает картинку картинкой, без единой правки. Чего нет: глубины резкости, смаза, зерна,
// аберрации — эффектов, которые нужны НЕ всегда и которые человек добавляет
// осознанно. Компонент, приезжающий со всем сразу, пришлось бы первым делом
// раздевать.
PostChain PostChain::Default() {
    PostChain chain;
    chain.Effects.push_back(MakePostEffect("exposure"));
    chain.Effects.push_back(MakePostEffect("autoexposure"));
    chain.Effects.push_back(MakePostEffect("bloom"));
    chain.Effects.push_back(MakePostEffect("color"));
    PostEffect tonemap = MakePostEffect("tonemap");
    if (PostValue* mode = tonemap.Find("mode")) mode->V[0] = 4.0f;   // Cinematic
    chain.Effects.push_back(std::move(tonemap));
    return chain;
}

PostFX::PostFX() {
    m_scratch.Fullscreen = GraphicsDevice::Get().CreateGeometry(VertexLayout{});
}

void PostFX::EnsureTargets(int w, int h) {
    if (w == m_scratch.Width && h == m_scratch.Height && m_scratch.Ao) return;
    m_scratch.Width = w;
    m_scratch.Height = h;
    const int hw = std::max(1, w / 2), hh = std::max(1, h / 2); // свечение — половинное
    m_scratch.Ao = MakeColor(w, h);
    m_scratch.AoBlur = MakeColor(w, h);
    m_scratch.Bright = MakeColor(hw, hh);
    m_scratch.BloomA = MakeColor(hw, hh);
    m_scratch.BloomB = MakeColor(hw, hh);
    // Смена размера обесценивает рабочие буферы, пул целей и историю кадра:
    // репроецировать старую матрицу на другой кадр нельзя, а цель другого
    // размера не подходит ни одному звену.
    m_scratch.DofPrep.reset();
    m_scratch.DofBlur.reset();
    m_scratch.DofNearMaxA.reset();
    m_scratch.DofNearMaxB.reset();
    m_scratch.DofNear.reset();
    m_scratch.Pool.clear();
    m_scratch.HasPrevFrame = false;
}

RenderTarget* PostFX::EnsureAux(std::unique_ptr<RenderTarget>& slot, int w, int h) {
    return Keep(slot, w > 0 ? w : m_scratch.Width, h > 0 ? h : m_scratch.Height);
}

void PostFX::BlitToOutput(sage::rhi::TextureHandle source, Framebuffer* output, int outX, int outY,
                          int outW, int outH) {
    GraphicsDevice& device = GraphicsDevice::Get();
    // Состояние, от которого зависит проход, проход и выставляет: копия
    // вызывается и из тракта, и сама по себе — например хостом, у которого
    // пост-обработка выключена конфигом, или тестом «с обработкой и без».
    device.SetDepthTest(false);
    device.SetBlend(false);
    if (output) {
        output->Bind();
    } else {
        device.BindDefaultFramebuffer();
        device.SetViewport(outX, outY, outW, outH);
    }
    Shader& copy = CopyShader();
    copy.Use();
    copy.SetInt("uScene", 0);
    device.BindTexture2D(0, source);
    m_scratch.Fullscreen->DrawArrays(3);
}

void PostFX::Render(sage::rhi::TextureHandle sceneColor, sage::rhi::TextureHandle sceneDepth,
                    int w, int h, const glm::mat4& proj, const glm::mat4& view,
                    const PostChain& chain, Framebuffer* output, int outX, int outY, int outW,
                    int outH, sage::rhi::TextureHandle velocityTexture) {
    SAGE_PROFILE("Пост-обработка");
    GraphicsDevice& device = GraphicsDevice::Get();
    EnsureTargets(w, h);
    device.SetDepthTest(false); // все проходы — полноэкранные, глубина не нужна
    // СМЕШИВАНИЕ ВЫКЛЮЧАЕТСЯ ЯВНО, а не предполагается выключенным.
    //
    // Каждый проход этой цепочки обязан ЗАМЕНИТЬ содержимое своей цели: он
    // читает предыдущий буфер и пишет следующий. Со включённым смешиванием он
    // вместо этого подмешивается к тому, что в цели уже лежит, — а лежит там
    // результат ПРОШЛОГО КАДРА: буферы затенения, свечения и промежуточные
    // цели живут между кадрами и намеренно не очищаются (их всё равно
    // перекрывает полноэкранный треугольник).
    //
    // Получалась положительная обратная связь: кадр темнел на несколько
    // процентов, следующий брал уже потемневший результат и темнел ещё, и так
    // без конца. Замер на неподвижной сцене: яркость 105 -> 87 -> 78 -> 75...
    // Снаружи это и есть жалоба «сначала рендер работает нормально, а потом без
    // всякой причины начинает деградировать»; отладочные режимы вида при этом
    // остаются правильными, потому что идут мимо пост-обработки, — и поломка
    // выглядит необъяснимой.
    //
    // Кто именно оставил смешивание включённым, значения не имеет: этой цепочке
    // нельзя зависеть от порядка чужих проходов. Состояние, от которого зависит
    // проход, проход и выставляет — ровно как строкой выше с глубиной.
    device.SetBlend(false);

    // ТРАКТ ДОПОЛНЯЕТСЯ И ПРОВЕРЯЕТСЯ ДО ОТРИСОВКИ.
    //
    // Порядок звеньев — данные, и он может быть неверным: свечение после
    // тон-маппинга, сглаживание до него, два тон-маппинга. Заметить это «по
    // картинке» нельзя: картинка будет просто неверной, без единого сообщения.
    // Поэтому тракт сперва дополняется до рабочего (нет тон-маппинга — он
    // вставляется, см. PostChain::Completed), затем компилируется и только потом
    // исполняется.
    // ПО ЭТАПАМ, а не по порядку списка: что человек видит разделами в
    // инспекторе, то и выполняется здесь — см. PostChain::Ordered.
    PostChain run = chain.Completed().Ordered();
    const PostChainReport report = run.Compile();
    if (!report.Ok) {
        // Неверный тракт НЕ оставляет чёрный экран: кадр показывается с одним
        // тон-маппингом, а причина уходит в лог. Отказ показать картинку
        // из-за опечатки в настройке — самый дорогой исход: человек увидит
        // чёрное и решит, что сломан рендер, а не тракт.
        OnceLog("тракт пост-обработки неверен: " + report.Error +
                " — кадр показан без эффектов");
        run.Effects.clear();
        run.Effects.push_back(MakePostEffect("tonemap"));
    }

    // Контекст кадра — ВСЁ, что звено вправе знать. Ни очереди эффектов, ни
    // числа проходов, ни внутренностей исполнителя здесь нет: звено должно
    // уметь работать, ничего не зная о том, кто стоит до и после него.
    PostContext ctx;
    ctx.Device = &device;
    ctx.Width = w;
    ctx.Height = h;
    ctx.Proj = proj;
    ctx.View = view;
    ctx.SceneColor = sceneColor;
    ctx.SceneDepth = sceneDepth;
    ctx.Velocity = velocityTexture;
    ctx.Color = sceneColor;
    ctx.ColorIsLdr = false;
    ctx.Scratch = &m_scratch;
    ctx.Lighting = m_lighting;
    // Время — от первого кадра процесса, а не от начала эпохи: у секунд с 1970
    // года не хватает точности float, и зерно плёнки застывало бы на месте.
    {
        static const auto start = std::chrono::steady_clock::now();
        ctx.Time = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    }

    const glm::mat4 viewProj = proj * view;

    for (const PostEffect& effect : run.Effects) {
        if (!effect.Enabled) continue;
        const PostEffectKind* kind = PostEffectCatalog::Instance().Find(effect.Kind);
        if (!kind || !kind->Run) continue;

        // Чего не хватает КАДРУ, а не тракту: глубины может не быть вовсе, если
        // сцену нарисовали без буфера глубины. Это не ошибка автора тракта, и
        // звено пропускается — но с причиной, а не молча.
        if (Has(kind->Needs, PostNeeds::Depth) && !ctx.SceneDepth.Valid()) {
            OnceLog("звено «" + kind->Label + "» пропущено: у кадра нет текстуры глубины");
            continue;
        }
        // Эти два случая компилятор уже отверг; здесь они лишь страховка от
        // расхождения между проверкой и исполнением.
        if (Has(kind->Needs, PostNeeds::HdrColor) && ctx.ColorIsLdr) continue;
        if (Has(kind->Needs, PostNeeds::LdrColor) && !ctx.ColorIsLdr) continue;

        kind->Run(ctx, effect);
    }

    // ФИНАЛЬНАЯ КОПИЯ. Звено пишет в СВОЮ цель, а не в выход: оно не обязано
    // знать, куда смотрит камера. Копия стоит один проход и одно чтение
    // текстуры, зато одно и то же звено годится и для превью в редакторе, и для
    // экрана игры, и для второго вьюпорта другого размера.
    BlitToOutput(ctx.Color, output, outX, outY, outW, outH);

    device.SetDepthTest(true);

    // История для смаза следующего кадра. Пишется ВСЕГДА, даже когда смаз
    // выключен: иначе после его включения первый кадр сравнивался бы с давно
    // устаревшей матрицей и размазал бы весь экран.
    m_scratch.PrevViewProj = viewProj;
    m_scratch.HasPrevFrame = true;
}


// --- Проверка цепочки на этой видеокарте -------------------------------------
//
// Прогоняем известную картинку и смотрим, что вышло. Ровный средний серый —
// вход, у которого правильный ответ известен без спора: тон-маппинг его
// подвинет, но чёрным и белым он не станет ни при каких настройках.
//
// Проверяется ровно то, что ломается: не «совпал ли пиксель», а «не пропала ли
// картинка». Сравнивать с эталоном тут нельзя — у каждой видеокарты своя
// фильтрация и свой порядок операций, и любой такой эталон дал бы ложную
// тревогу на первой же чужой машине.
const PostFX::SelfCheck& PostFX::CheckPipeline() {
    if (m_selfCheck.Ran) return m_selfCheck;
    m_selfCheck.Ran = true;

    sage::rhi::GraphicsDevice& dev = sage::rhi::GraphicsDevice::Get();
    constexpr int kSize = 32;

    // Вход: ровный серый, непрозрачный.
    std::vector<unsigned char> gray((size_t)kSize * kSize * 4, 128);
    for (size_t i = 3; i < gray.size(); i += 4) gray[i] = 255;
    sage::rhi::Texture2DDesc desc;
    desc.Width = desc.Height = kSize;
    desc.Channels = 4;
    desc.FilterMode = sage::rhi::Filter::Bilinear;
    desc.WrapMode = sage::rhi::Wrap::ClampEdge;
    desc.GenerateMipmaps = false;
    std::unique_ptr<sage::rhi::Texture2D> input = dev.CreateTexture2D(desc, gray.data());
    if (!input) {
        m_selfCheck.Reason = "не удалось создать пробную текстуру";
        return m_selfCheck;
    }

    Framebuffer target(kSize, kSize);
    // Тракт для проверки — ПУСТОЙ: он означает «только тон-маппинг», а все
    // эффекты, читающие глубину, в этой пробе и не нужны (глубины тут нет).
    // Дополнение до рабочего делает сам исполнитель (PostChain::Completed).
    const PostChain chain;
    Render(input->Handle(), sage::rhi::TextureHandle{}, kSize, kSize, glm::mat4(1.0f),
           glm::mat4(1.0f), chain, &target, 0, 0, kSize, kSize);

    target.Resolve();
    target.Bind();
    std::vector<unsigned char> out((size_t)kSize * kSize * 3, 0);
    dev.ReadPixelsRGB(0, 0, kSize, kSize, out.data());
    dev.BindDefaultFramebuffer();

    double sum = 0.0;
    for (unsigned char c : out) sum += c;
    const double mean = sum / (double)out.size();

    // Порог низкий намеренно: ловим ПРОПАЖУ картинки, а не отличие в оттенке.
    // Серый после любого разумного тон-маппинга остаётся заметно светлее нуля.
    if (mean < 4.0) {
        m_selfCheck.Reason = "цепочка вернула чёрный кадр на заведомо сером входе";
        return m_selfCheck;
    }
    m_selfCheck.Ok = true;
    return m_selfCheck;
}

} // namespace sage::render
