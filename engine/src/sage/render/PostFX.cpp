#include "sage/render/PostFX.h"

#include "sage/core/Profiler.h"

#include <algorithm>
#include <string>

#include <glm/gtc/matrix_inverse.hpp>

#include "sage/render/Shader.h"
#include "sage/rhi/GraphicsDevice.h"

using namespace sage::rhi;

namespace sage::render {

PostFXSettings FxFromConfig(const sage::EngineConfig& cfg) {
    PostFXSettings fx;
    fx.Exposure = cfg.Exposure; fx.Gamma = cfg.Gamma;
    fx.Saturation = cfg.Saturation; fx.Contrast = cfg.Contrast;
    fx.Vignette = cfg.Vignette;
    fx.BloomEnabled = cfg.Bloom; fx.BloomThreshold = cfg.BloomThreshold;
    fx.BloomIntensity = cfg.BloomIntensity;
    fx.AOEnabled = cfg.AmbientOcclusion; fx.AOStrength = cfg.AOStrength;
    fx.AORadius = cfg.AORadius;
    fx.DofEnabled = cfg.DepthOfField; fx.FocusDistance = cfg.FocusDistance;
    fx.Aperture = cfg.Aperture; fx.DofMaxRadius = cfg.DofMaxRadius;
    fx.MotionBlurEnabled = cfg.MotionBlur; fx.MotionBlurAmount = cfg.MotionBlurAmount;
    fx.MotionBlurSamples = cfg.MotionBlurSamples;
    fx.ChromaticAberration = cfg.ChromaticAberration;
    fx.FxaaContrastThreshold = cfg.FxaaContrastThreshold;

    // FXAA и MSAA НЕ СКЛАДЫВАЮТСЯ.
    //
    // Это и есть «сглаживание не работает, а картинка мыльная»: они лечат одно
    // и то же разными способами, и второй проход поверх первого уже нечего
    // сглаживать — зато он честно размывает всё, что похоже на кромку, включая
    // текстуры и мелкие детали. MSAA решает задачу в источнике (растеризатор
    // считает покрытие пикселя геометрией), FXAA — по готовым пикселям, гадая
    // по яркости; когда работает первый, второй только портит.
    fx.FxaaEnabled = cfg.Fxaa && SceneSamples(cfg) <= 1;
    return fx;
}

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
        float bias = max(uRadius * 0.02, 0.005);
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
    FragColor = vec4(vec3(sum / weight), centerDepth);
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
void main() {
    float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    vec3 c = texture(uTex, vUV).rgb * w[0];
    for (int i = 1; i < 5; ++i) {
        c += texture(uTex, vUV + uDir * float(i)).rgb * w[i];
        c += texture(uTex, vUV - uDir * float(i)).rgb * w[i];
    }
    FragColor = vec4(c, 1.0);
}
)";

// --- Глубина резкости: gather-размытие по кругу нерезкости из глубины ---
//
// Классическая проблема экранного DoF — «протекание» резкого переднего плана на
// размытый фон. Решается правилом сбора: сэмпл участвует в размытии пикселя,
// только если ОН САМ достаточно размыт, чтобы дотянуться сюда своим кругом
// нерезкости (или если он дальше от камеры, чем текущий пиксель). Поэтому
// каждый сэмпл проверяется по собственной глубине, а не берётся вслепую.
const char* kDofFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uScene;
uniform sampler2D uDepth;
uniform mat4 uInvProj;
uniform vec2 uTexel;
uniform float uFocus;      // расстояние до плоскости фокуса, единицы мира
uniform float uAperture;   // f-число
uniform float uMaxRadius;  // потолок радиуса, пиксели

// Расстояние от камеры вдоль взгляда (положительное).
float LinearDepth(vec2 uv) {
    float d = texture(uDepth, uv).r;
    vec4 c = uInvProj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return -(c.z / max(abs(c.w), 1e-6)) * sign(c.w);
}

// Радиус круга нерезкости в пикселях.
//
// Считается по РАЗНИЦЕ ОБРАТНЫХ РАССТОЯНИЙ (диоптрий), а не по относительному
// отклонению: именно так ведёт себя тонкая линза. Разница принципиальная —
// |d - focus| / d уже при полуторакратном удалении от плоскости фокуса упирается
// в потолок, и весь фон превращается в кашу; |1/focus - 1/d| растёт плавно и
// насыщается только на действительно далёких планах. Диафрагма делит результат:
// f/1.4 размывает сильно, f/16 оставляет резким почти всё.
float Coc(float depth) {
    float diopters = abs(1.0 / max(uFocus, 1e-3) - 1.0 / max(depth, 1e-3));
    return clamp(diopters * (15.0 / max(uAperture, 0.7)), 0.0, 1.0) * uMaxRadius;
}

void main() {
    float depth = LinearDepth(vUV);
    float coc = Coc(depth);

    vec3 sum = texture(uScene, vUV).rgb;
    float weight = 1.0;

    if (coc > 0.75) { // меньше пикселя — размывать нечего
        const int N = 24;
        for (int i = 0; i < N; ++i) {
            // Спираль по золотому углу: равномерное покрытие диска без таблиц.
            float fi = (float(i) + 0.5) / float(N);
            float ang = float(i) * 2.39996323;
            float r = sqrt(fi) * coc;
            vec2 suv = vUV + vec2(cos(ang), sin(ang)) * r * uTexel;

            float sd = LinearDepth(suv);
            float sc = Coc(sd);
            // Дальний сэмпл виден всегда; ближний — только если его собственный
            // круг нерезкости дотягивается до нас (иначе резкий передний план
            // размазался бы по фону).
            float w = (sd >= depth) ? 1.0 : smoothstep(0.0, 1.0, sc / max(r, 1e-3));
            sum += texture(uScene, suv).rgb * w;
            weight += w;
        }
    }
    FragColor = vec4(sum / weight, 1.0);
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

void main() {
    vec2 velocity;

    if (uUseVelocity == 1) {
        // Готовый вектор из прохода геометрии: он уже учитывает и движение
        // камеры, и собственное движение объекта.
        velocity = texture(uVelocity, vUV).xy * uAmount;
    } else {
        // Запасной путь без буфера скоростей: мир считается неподвижным, и
        // смаз получается только от камеры (см. комментарий к PostFX::Render).
        float d = texture(uDepth, vUV).r;

        vec4 clip = vec4(vUV * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
        vec4 world = uInvViewProj * clip;
        if (abs(world.w) < 1e-6) { FragColor = texture(uScene, vUV); return; }
        world /= world.w;

        vec4 prevClip = uPrevViewProj * world;
        if (prevClip.w < 1e-6) { FragColor = texture(uScene, vUV); return; } // за камерой
        vec2 prevUV = (prevClip.xy / prevClip.w) * 0.5 + 0.5;

        velocity = (vUV - prevUV) * uAmount;
    }

    // Потолок длины: резкий рывок камеры иначе размазал бы кадр целиком, и
    // вместо смаза получилась бы каша.
    const float kMaxLen = 0.05;
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

const char* kCompositeFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform sampler2D uAO;
uniform bool uUseBloom;
uniform bool uUseAO;
uniform float uExposure;
uniform float uGamma;
uniform float uSaturation;
uniform float uContrast;
uniform float uVignette;
uniform float uBloomIntensity;
uniform float uAOStrength;
uniform float uChromatic;

vec3 ACES(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// Чтение сцены с хроматической аберрацией: каналы расходятся ВДОЛЬ РАДИУСА и
// тем сильнее, чем дальше от центра кадра, — так ведёт себя реальная оптика. В
// центре расхождения нет, поэтому лицо в центре кадра остаётся чистым.
vec3 SampleScene(vec2 uv) {
    if (uChromatic <= 0.0) return texture(uScene, uv).rgb;
    vec2 radial = uv - 0.5;
    vec2 off = radial * uChromatic * 0.02;
    return vec3(texture(uScene, uv + off).r,
                texture(uScene, uv).g,
                texture(uScene, uv - off).b);
}

void main() {
    vec3 hdr = SampleScene(vUV);
    if (uUseAO) {
        float ao = clamp(texture(uAO, vUV).r, 0.0, 1.0);
        ao = pow(ao, uAOStrength); // усиление затемнения
        hdr *= ao;
    }
    if (uUseBloom) hdr += texture(uBloom, vUV).rgb * uBloomIntensity;

    vec3 color = ACES(hdr * uExposure);
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(luma), color, uSaturation);
    color = (color - 0.5) * uContrast + 0.5;

    vec2 uv = vUV - 0.5;
    float vig = 1.0 - uVignette * dot(uv, uv) * 2.0;
    color *= clamp(vig, 0.0, 1.0);

    color = pow(max(color, 0.0), vec3(1.0 / uGamma));
    FragColor = vec4(color, 1.0);
}
)";

Shader& SsaoShader()      { static Shader* s = new Shader(Shader::FromSource(kFsVert, kSsaoFrag, "PostFX.SSAO")); return *s; }
Shader& AoBlurShader()    { static Shader* s = new Shader(Shader::FromSource(kFsVert, kAoBlurFrag, "PostFX.AOBlur")); return *s; }
Shader& DofShader()       { static Shader* s = new Shader(Shader::FromSource(kFsVert, kDofFrag, "PostFX.DoF")); return *s; }
Shader& MotionShader()    { static Shader* s = new Shader(Shader::FromSource(kFsVert, kMotionFrag, "PostFX.MotionBlur")); return *s; }
Shader& BrightShader()    { static Shader* s = new Shader(Shader::FromSource(kFsVert, kBrightFrag, "PostFX.Bright")); return *s; }
Shader& BlurShader()      { static Shader* s = new Shader(Shader::FromSource(kFsVert, kBlurFrag, "PostFX.Blur")); return *s; }
Shader& CompositeShader() { static Shader* s = new Shader(Shader::FromSource(kFsVert, kCompositeFrag, "PostFX.Composite")); return *s; }
Shader& FxaaShader() { static Shader* s = new Shader(Shader::FromSource(kFsVert, kFxaaFrag, "PostFX.FXAA")); return *s; }

std::unique_ptr<RenderTarget> MakeColor(int w, int h) {
    RenderTargetDesc d;
    d.Width = w; d.Height = h; d.Kind = RenderTargetKind::ColorHDR;
    return GraphicsDevice::Get().CreateRenderTarget(d);
}

} // namespace

PostFX::PostFX() {
    m_fsTri = GraphicsDevice::Get().CreateGeometry(VertexLayout{});
}

void PostFX::EnsureTargets(int w, int h) {
    if (w == m_w && h == m_h && m_ao) return;
    m_w = w; m_h = h;
    int hw = std::max(1, w / 2), hh = std::max(1, h / 2); // bloom — половинное разрешение
    m_ao = MakeColor(w, h);
    m_aoBlur = MakeColor(w, h);
    m_bright = MakeColor(hw, hh);
    m_bloomA = MakeColor(hw, hh);
    m_bloomB = MakeColor(hw, hh);
    // Смена размера обесценивает вспомогательные буферы и историю кадра:
    // репроецировать старую матрицу на другой кадр нельзя.
    m_dof.reset();
    m_motion.reset();
    m_ldr.reset();
    m_hasPrevFrame = false;
}

RenderTarget* PostFX::EnsureAux(std::unique_ptr<RenderTarget>& slot) {
    if (!slot) slot = MakeColor(m_w, m_h);
    return slot.get();
}

void PostFX::Render(sage::rhi::TextureHandle sceneColor, sage::rhi::TextureHandle sceneDepth,
                    int w, int h, const glm::mat4& proj, const glm::mat4& view,
                    const PostFXSettings& s, Framebuffer* output, int outX, int outY, int outW,
                    int outH, sage::rhi::TextureHandle velocityTexture) {
    SAGE_PROFILE("Пост-обработка");
    GraphicsDevice& device = GraphicsDevice::Get();
    EnsureTargets(w, h);
    device.SetDepthTest(false); // все проходы — полноэкранные, глубина не нужна

    auto drawTri = [&]() { m_fsTri->DrawArrays(3); };

    // Все три эффекта, читающие глубину, без depth-текстуры невозможны.
    const bool haveDepth = sceneDepth.Valid();
    const bool doAO = s.AOEnabled && haveDepth;
    const bool doDof = s.DofEnabled && haveDepth;
    // С буфером скоростей история прошлого кадра ЗДЕСЬ не нужна — её хранит
    // проход геометрии, у каждой сущности свою. Поэтому m_hasPrevFrame требуется
    // только запасному пути.
    const bool useVelocity = velocityTexture.Valid();
    const bool doMotion = s.MotionBlurEnabled && (useVelocity || (haveDepth && m_hasPrevFrame)) &&
                          s.MotionBlurAmount > 0.001f;
    const bool doBloom = s.BloomEnabled;

    const glm::mat4 viewProj = proj * view;

    // --- 1. SSAO из глубины сцены -> m_ao, затем размытие -> m_aoBlur ---
    if (doAO) {
        SAGE_PROFILE("SSAO");
        glm::mat4 invProj = glm::inverse(proj);
        m_ao->Bind();
        Shader& ao = SsaoShader();
        ao.Use();
        ao.SetInt("uDepth", 0);
        device.BindTexture2D(0, sceneDepth);
        ao.SetMat4("uProj", proj);
        ao.SetMat4("uInvProj", invProj);
        ao.SetFloat("uRadius", s.AORadius);
        const glm::vec2 texel(1.0f / (float)w, 1.0f / (float)h);
        ao.SetVec2("uTexel", texel);
        drawTri();

        // Размытие РАЗДЕЛЁННОЕ: горизонталь в m_aoBlur, вертикаль обратно в
        // m_ao. Два прохода по 9 выборок вместо одного по 16 — и дешевле, и
        // шире окно (сглаживает шум выборки, который квадрат 4x4 оставлял).
        Shader& aob = AoBlurShader();
        aob.Use();
        aob.SetInt("uAO", 0);
        aob.SetVec2("uTexel", texel);

        m_aoBlur->Bind();
        aob.SetVec2("uDirection", glm::vec2(1.0f, 0.0f));
        device.BindTexture2D(0, m_ao->ColorTextureHandle());
        drawTri();

        m_ao->Bind();
        aob.SetVec2("uDirection", glm::vec2(0.0f, 1.0f));
        device.BindTexture2D(0, m_aoBlur->ColorTextureHandle());
        drawTri();
    }

    // --- 2. Глубина резкости: размытие по кругу нерезкости из глубины ---
    // colorTex — «текущая картинка» цепочки: каждый следующий проход читает
    // результат предыдущего, а не исходный кадр.
    sage::rhi::TextureHandle colorTex = sceneColor;
    if (doDof) {
        SAGE_PROFILE("Глубина резкости");
        RenderTarget* dof = EnsureAux(m_dof);
        dof->Bind();
        Shader& sh = DofShader();
        sh.Use();
        sh.SetInt("uScene", 0);
        sh.SetInt("uDepth", 1);
        device.BindTexture2D(0, colorTex);
        device.BindTexture2D(1, sceneDepth);
        sh.SetMat4("uInvProj", glm::inverse(proj));
        sh.SetVec2("uTexel", glm::vec2(1.0f / (float)w, 1.0f / (float)h));
        sh.SetFloat("uFocus", glm::max(s.FocusDistance, 0.01f));
        sh.SetFloat("uAperture", glm::max(s.Aperture, 0.7f));
        sh.SetFloat("uMaxRadius", glm::max(s.DofMaxRadius, 0.0f));
        drawTri();
        colorTex = dof->ColorTextureHandle();
    }

    // --- 3. Motion blur: смаз вдоль вектора репроекции прошлым кадром ---
    if (doMotion) {
        SAGE_PROFILE("Смаз движения");
        RenderTarget* motion = EnsureAux(m_motion);
        motion->Bind();
        Shader& sh = MotionShader();
        sh.Use();
        sh.SetInt("uScene", 0);
        sh.SetInt("uDepth", 1);
        sh.SetInt("uVelocity", 2);
        device.BindTexture2D(0, colorTex);
        device.BindTexture2D(1, sceneDepth);
        // Сэмплер обязан быть привязан всегда, даже когда путь не выбран:
        // непривязанный юнит на части драйверов читается как чёрная текстура,
        // а на части — как мусор из чужого прохода.
        device.BindTexture2D(2, useVelocity ? velocityTexture : sceneDepth);
        sh.SetInt("uUseVelocity", useVelocity ? 1 : 0);
        sh.SetMat4("uInvViewProj", glm::inverse(viewProj));
        sh.SetMat4("uPrevViewProj", m_prevViewProj);
        sh.SetFloat("uAmount", s.MotionBlurAmount);
        sh.SetInt("uSamples", glm::clamp(s.MotionBlurSamples, 2, 32));
        drawTri();
        colorTex = motion->ColorTextureHandle();
    }

    // --- 4. Bloom: bright-pass -> размытие (2 итерации, ping-pong) ---
    // Считается по УЖЕ размытой картинке (см. комментарий к порядку цепочки в
    // PostFX.h): свечение размытого источника не должно быть резким.
    if (doBloom) {
        SAGE_PROFILE("Bloom");
        int hw = m_bright->Width(), hh = m_bright->Height();
        m_bright->Bind();
        Shader& br = BrightShader();
        br.Use();
        br.SetInt("uScene", 0);
        device.BindTexture2D(0, colorTex);
        br.SetFloat("uThreshold", s.BloomThreshold);
        drawTri();

        Shader& blur = BlurShader();
        blur.Use();
        blur.SetInt("uTex", 0);
        glm::vec2 texel(1.0f / (float)hw, 1.0f / (float)hh);
        sage::rhi::TextureHandle src = m_bright->ColorTextureHandle();
        RenderTarget* dstA = m_bloomA.get();
        RenderTarget* dstB = m_bloomB.get();
        for (int i = 0; i < 2; ++i) {
            dstA->Bind(); // горизонтальный
            device.BindTexture2D(0, src);
            blur.SetVec2("uDir", glm::vec2(texel.x, 0.0f));
            drawTri();
            dstB->Bind(); // вертикальный
            device.BindTexture2D(0, dstA->ColorTextureHandle());
            blur.SetVec2("uDir", glm::vec2(0.0f, texel.y));
            drawTri();
            src = dstB->ColorTextureHandle();
        }
    }

    // --- 5. Composite -> output (FBO вьюпорта) или экран ---
    // FXAA работает по ГОТОВОЙ картинке, поэтому при включённом сглаживании
    // composite пишет не в выход, а в промежуточный буфер: шейдеру нужен вход,
    // а читать тот же буфер, в который пишешь, нельзя.
    const bool doFxaa = s.FxaaEnabled;
    RenderTarget* ldr = doFxaa ? EnsureAux(m_ldr) : nullptr;
    {
    SAGE_PROFILE("Тон-маппинг");
    if (ldr) {
        ldr->Bind();
    } else if (output) {
        output->Bind();
    } else {
        device.BindDefaultFramebuffer();
        device.SetViewport(outX, outY, outW, outH);
    }
    Shader& comp = CompositeShader();
    comp.Use();
    comp.SetInt("uScene", 0);
    comp.SetInt("uBloom", 1);
    comp.SetInt("uAO", 2);
    comp.SetInt("uUseBloom", doBloom ? 1 : 0);
    comp.SetInt("uUseAO", doAO ? 1 : 0);
    comp.SetFloat("uExposure", s.Exposure);
    comp.SetFloat("uGamma", s.Gamma);
    comp.SetFloat("uSaturation", s.Saturation);
    comp.SetFloat("uContrast", s.Contrast);
    comp.SetFloat("uVignette", s.Vignette);
    comp.SetFloat("uBloomIntensity", s.BloomIntensity);
    comp.SetFloat("uAOStrength", glm::max(s.AOStrength, 0.01f));
    comp.SetFloat("uChromatic", glm::max(s.ChromaticAberration, 0.0f));
    device.BindTexture2D(0, colorTex);
    if (doBloom) device.BindTexture2D(1, m_bloomB->ColorTextureHandle());
    // ГОТОВОЕ размытое AO лежит в m_ao: вертикальный проход пишет обратно в
    // него (m_aoBlur — промежуточный буфер горизонтали, см. шаг 1).
    if (doAO) device.BindTexture2D(2, m_ao->ColorTextureHandle());
    drawTri();
    }

    // --- 6. FXAA -> выход ---
    if (ldr) {
        SAGE_PROFILE("FXAA");
        if (output) {
            output->Bind();
        } else {
            device.BindDefaultFramebuffer();
            device.SetViewport(outX, outY, outW, outH);
        }
        Shader& fxaa = FxaaShader();
        fxaa.Use();
        fxaa.SetInt("uTex", 0);
        fxaa.SetVec2("uTexel", glm::vec2(1.0f / (float)m_w, 1.0f / (float)m_h));
        fxaa.SetFloat("uContrastThreshold", glm::max(s.FxaaContrastThreshold, 0.0f));
        device.BindTexture2D(0, ldr->ColorTextureHandle());
        drawTri();
    }

    device.SetDepthTest(true);

    // История для смаза следующего кадра. Пишется ВСЕГДА, даже когда motion blur
    // выключен: иначе после его включения первый кадр сравнивался бы с давно
    // устаревшей матрицей и размазал бы весь экран.
    m_prevViewProj = viewProj;
    m_hasPrevFrame = true;
}

} // namespace sage::render
