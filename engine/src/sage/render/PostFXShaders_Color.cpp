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

// Шейдеры цвета пост-обработки и ленивые программы всех шейдеров (см. PostFXDetail.h).
namespace sage::render::postfx {

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
//
// ГАММА РАБОТАЕТ ПРИ ЛЮБОМ ВЫВОДЕ. Раньше её читал только вывод «Gamma», а по
// умолчанию стоит sRGB — и ползунок, который двигают первым, не менял ничего.
// Для sRGB и линейного она — поправка средних тонов относительно стандартных
// 2.2: pow(c, 2.2/g). Ровно 2.2 — без поправки (старые сцены не меняются),
// больше — светлее, меньше — темнее, как и у вывода «Gamma».
vec3 Encode(vec3 c) {
    if (uOutput == 1) return pow(c, vec3(1.0 / uGamma));
    c = pow(c, vec3(2.2 / uGamma));
    if (uOutput == 2) return c;
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
//
// ЧТО БЫЛО НЕ ТАК. Три выборки (R наружу, G на месте, B внутрь) со сдвигом в
// долях UV:
//   • красный у края читался ЗА кадром — край размазывался полосой;
//   • три дискретные копии давали двоение контура, а не цветную кайму: стоило
//     прибавить силу, и предмет распадался на три цветных силуэта;
//   • сила упиралась в 1.0, то есть в 1% кадра у угла — эффект почти не виден.
//
// КАК ТЕПЕРЬ. Как у настоящего объектива: разные длины волн увеличиваются
// по-разному. Кадр выбирается N раз с масштабом от 1 (красный край спектра) до
// 1 - k (синий), и каждая выборка весит в R, G, B по спектральной кривой —
// получается сплошная кайма без двоения. Масштаб только УМЕНЬШАЕТ, поэтому
// выборка никогда не выходит за кадр. uStart оставляет центр чистым (радиус —
// с поправкой на соотношение сторон: «угол кадра» — это 1.0 и в 16:9).
const char* kChromaticFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform float uAmount;   // разница увеличения краёв спектра, доли кадра
uniform float uStart;    // радиус чистого центра (0 — с центра, 1 — только углы)
uniform float uAspect;   // ширина / высота
uniform int uSamples;    // выборок по спектру (3..32)

// Вес выборки t (0 — красный край, 1 — синий) в каналах: три перекрывающихся
// «горба», в сумме по t каждый канал набирает одинаковый вес.
vec3 Spectrum(float t) {
    return clamp(vec3(1.0 - abs(t * 2.0 - 0.0) * 1.0,
                      1.0 - abs(t * 2.0 - 1.0) * 1.0,
                      1.0 - abs(t * 2.0 - 2.0) * 1.0), 0.0, 1.0);
}

void main() {
    vec2 d = vUV - 0.5;
    vec2 da = d * vec2(uAspect, 1.0);
    float rMax = length(vec2(uAspect, 1.0) * 0.5);
    float r = length(da) / rMax;                       // 1.0 в углу кадра
    float mask = smoothstep(uStart, 1.0, r);
    float k = uAmount * mask;
    int n = clamp(uSamples, 3, 32);
    vec3 sum = vec3(0.0), wsum = vec3(0.0);
    for (int i = 0; i < 32; ++i) {
        if (i >= n) break;
        float t = float(i) / float(n - 1);
        vec3 w = Spectrum(t);
        vec2 uv = 0.5 + d * (1.0 - k * t);
        sum += texture(uScene, uv).rgb * w;
        wsum += w;
    }
    FragColor = vec4(sum / max(wsum, vec3(1e-4)), 1.0);
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
Shader& SsgiShader()      { static Shader* s = new Shader(Shader::FromSource(kFsVert, kSsgiFrag, "PostFX.SSGI")); return *s; }
Shader& SsgiBlurShader()  { static Shader* s = new Shader(Shader::FromSource(kFsVert, kSsgiBlurFrag, "PostFX.SSGIBlur")); return *s; }
Shader& SsgiApplyShader() { static Shader* s = new Shader(Shader::FromSource(kFsVert, kSsgiApplyFrag, "PostFX.SSGIApply")); return *s; }
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

} // namespace sage::render::postfx
