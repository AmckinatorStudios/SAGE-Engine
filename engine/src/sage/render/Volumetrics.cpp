#include "sage/render/Volumetrics.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "sage/core/Config.h"
#include "sage/core/Profiler.h"
#include "sage/core/Log.h"
#include "sage/render/Shader.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Light.h"

using namespace sage::rhi;

namespace sage::render {
namespace {

const char* kVert = R"(#version 330 core
out vec2 vUV;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// --- 1. Глубина прохода -------------------------------------------------------
//
// Марш и подъём обязаны видеть ОДНУ И ТУ ЖЕ глубину, иначе вес билатерального
// фильтра считается по одному, а содержимое взято по другому — и фильтр
// превращается в размытие. Поэтому глубина прохода готовится здесь один раз:
//   R — расстояние от камеры до поверхности вдоль луча (метры),
//   G — 1, если луч ушёл в небо.
// Из блока берётся БЛИЖАЙШАЯ поверхность: потерять тонкий предмет (перила,
// трос) хуже, чем чуть занизить расстояние на его краю.
const char* kDepthFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uDepth;
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
uniform vec2 uFullTexel;   // размер пикселя ПОЛНОГО буфера
uniform int  uBlock;       // сторона блока в полных пикселях (1 — без уменьшения)
uniform float uMaxDist;

float distAt(vec2 uv, out bool sky) {
    float d = texture(uDepth, uv).r;
    sky = d >= 0.9999;
    if (sky) return uMaxDist;
    vec4 wp = uInvViewProj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return length(wp.xyz / wp.w - uCamPos);
}

void main() {
    bool sky = true;
    float best = 1e30;
    bool anySolid = false;
    for (int y = 0; y < 4; ++y) {
        if (y >= uBlock) break;
        for (int x = 0; x < 4; ++x) {
            if (x >= uBlock) break;
            vec2 uv = vUV + (vec2(float(x), float(y)) - float(uBlock - 1) * 0.5) * uFullTexel;
            bool s;
            float dist = distAt(uv, s);
            if (!s) {
                anySolid = true;
                best = min(best, dist);
            }
        }
    }
    if (anySolid) FragColor = vec4(best, 0.0, 0.0, 1.0);
    else FragColor = vec4(uMaxDist, 1.0, 0.0, 1.0);
}
)";

// --- 2. Марш по лучу: рассеяние в воздухе + облака -----------------------------
//
// Результат ПРЕДУМНОЖЕН на альфу: rgb — уже пришедший к глазу свет, a — сколько
// неба закрыто облаком. Так композит делается одной формулой смешивания и без
// второго прохода: лучи складываются, облака закрывают.
const char* kMarchFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSceneDist;   // R — расстояние до поверхности, G — небо
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
uniform vec3 uSunDir;        // направление НА солнце
uniform vec3 uSunColor;
uniform vec3 uSkyTop;
uniform vec3 uSkyHorizon;
uniform float uTime;
uniform vec2 uJitter;        // размер пикселя уменьшенного буфера
uniform float uFrameJitter;  // сдвиг шума этого кадра (накопление по времени)

uniform int   uShafts;
uniform float uDensity;
uniform float uAniso;
uniform float uMaxDist;
uniform float uIntensity;
uniform float uHeightFalloff;
uniform float uBaseHeight;
uniform int   uSteps;

uniform int   uClouds;
uniform float uCloudBottom;
uniform float uCloudTop;
uniform float uCoverage;
uniform float uCloudDensity;
uniform float uCloudScale;
uniform vec2  uWind;
uniform vec3  uTint;
uniform int   uCloudSteps;
uniform int   uCloudLightSteps;

uniform int   uDebug;        // 1 — показать поле видимости солнца
uniform int   uShadowsOn;
uniform int   uCascades;
uniform mat4  uLightMat[4];
uniform vec2  uShadowTexel[4];
uniform sampler2D uShadow0;
uniform sampler2D uShadow1;
uniform sampler2D uShadow2;
uniform sampler2D uShadow3;

// Фазовая функция Хеньи–Гринштейна: во сколько раз охотнее среда рассеивает
// вперёд. Без неё солнце не «загорается» в дымке, когда смотришь на него, —
// именно этот всплеск и читается глазом как объём.
float phaseHG(float c, float g) {
    float g2 = g * g;
    float d = 1.0 + g2 - 2.0 * g * c;
    return (1.0 - g2) / (12.566370614 * max(d * sqrt(max(d, 1e-4)), 1e-4));
}

float shadowTap(int i, vec2 uv, float z) {
    float d;
    if (i == 0) d = texture(uShadow0, uv).r;
    else if (i == 1) d = texture(uShadow1, uv).r;
    else if (i == 2) d = texture(uShadow2, uv).r;
    else d = texture(uShadow3, uv).r;
    // Запас по глубине больше, чем у поверхностей: точка в воздухе не лежит на
    // геометрии, и жёсткий порог давал бы в лучах полосы.
    return (z - 0.0025 > d) ? 0.0 : 1.0;
}

// Мягкая выборка каскада: четыре точки по углам текселя вместо одной.
//
// Бинарная выборка давала лучу край в ОДИН пиксель — ступеньку, которую не
// лечит ни число шагов, ни накопление по кадрам: она не шум, а точный ответ на
// слишком грубый вопрос. Четыре точки превращают её в полутон, и кромка луча
// становится мягкой, как у настоящего света в пыли.
float sampleCascade(int i, vec3 world) {
    vec4 lp = uLightMat[i] * vec4(world, 1.0);
    vec3 uv = lp.xyz / max(lp.w, 1e-6) * 0.5 + 0.5;
    if (uv.x < 0.02 || uv.x > 0.98 || uv.y < 0.02 || uv.y > 0.98 || uv.z > 1.0) return -1.0;
    vec2 t = uShadowTexel[i];
    float s = shadowTap(i, uv.xy + vec2(-0.5, -0.5) * t, uv.z)
            + shadowTap(i, uv.xy + vec2( 0.5, -0.5) * t, uv.z)
            + shadowTap(i, uv.xy + vec2(-0.5,  0.5) * t, uv.z)
            + shadowTap(i, uv.xy + vec2( 0.5,  0.5) * t, uv.z);
    return s * 0.25;
}

// Освещён ли этот кусочек воздуха. Каскады перебираются от ближнего: первый,
// в чьи границы точка попала, и отвечает.
float sunVisibility(vec3 world) {
    if (uShadowsOn == 0) return 1.0;
    for (int i = 0; i < 4; ++i) {
        if (i >= uCascades) break;
        float v = sampleCascade(i, world);
        if (v >= 0.0) return v;
    }
    return 1.0;   // за последним каскадом карты нет — считаем освещённым
}

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float vnoise(vec3 x) {
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash13(i + vec3(0,0,0)), hash13(i + vec3(1,0,0)), f.x),
                   mix(hash13(i + vec3(0,1,0)), hash13(i + vec3(1,1,0)), f.x), f.y),
               mix(mix(hash13(i + vec3(0,0,1)), hash13(i + vec3(1,0,1)), f.x),
                   mix(hash13(i + vec3(0,1,1)), hash13(i + vec3(1,1,1)), f.x), f.y), f.z);
}

float fbm(vec3 p) {
    float a = 0.5, s = 0.0;
    for (int i = 0; i < 4; ++i) { s += a * vnoise(p); p *= 2.03; a *= 0.5; }
    return s;
}

// Тот же шум в две октавы вместо четырёх — для марша К СОЛНЦУ.
//
// Там детали не нужны: считается, сколько света дошло до точки, а это величина
// низкочастотная по своей природе. Разницы в кадре нет, а стоит она половину:
// световой марш зовётся по нескольку раз на КАЖДЫЙ шаг основного, и именно он
// делает облака самым дорогим местом прохода.
float fbmLow(vec3 p) {
    float a = 0.5, s = 0.0;
    for (int i = 0; i < 2; ++i) { s += a * vnoise(p); p *= 2.03; a *= 0.5; }
    return s * 1.33;   // подгон под средний уровень четырёхоктавного
}

// Плотность для светового марша: без мелкой эрозии и по дешёвому шуму.
float cloudLightDensity(vec3 p) {
    float h = (p.y - uCloudBottom) / max(uCloudTop - uCloudBottom, 1.0);
    if (h < 0.0 || h > 1.0) return 0.0;
    float profile = smoothstep(0.0, 0.18, h) * smoothstep(1.0, 0.55, h);
    vec3 q = p * uCloudScale + vec3(uWind.x, 0.0, uWind.y) * uTime * 0.004;
    float gate = smoothstep(0.36, 0.62, fbmLow(q * 0.85 + 19.3));
    if (gate <= 0.0) return 0.0;
    float d = clamp((fbmLow(q) * profile - (1.0 - uCoverage)) / max(uCoverage, 0.05), 0.0, 1.0);
    return d * gate * uCloudDensity * 0.03;
}

// Плотность облака в точке. Профиль по высоте даёт плоское основание и пухлый
// верх — без него слой выглядит одинаковой ватой сверху донизу.
float cloudAt(vec3 p) {
    float h = (p.y - uCloudBottom) / max(uCloudTop - uCloudBottom, 1.0);
    if (h < 0.0 || h > 1.0) return 0.0;
    // Профиль по высоте: плоское основание, пухлый верх.
    float profile = smoothstep(0.0, 0.18, h) * smoothstep(1.0, 0.55, h);
    vec3 q = p * uCloudScale + vec3(uWind.x, 0.0, uWind.y) * uTime * 0.004;

    // Крупная карта решает, ГДЕ облака есть, а где чистое небо. Без неё шум
    // одной частоты даёт ровную пелену от края до края: отдельных облаков с
    // просветами между ними не получается ни при какой плотности.
    float gate = smoothstep(0.36, 0.62, fbm(q * 0.85 + 19.3));
    if (gate <= 0.0) return 0.0;

    // Порог по покрытию с ПЕРЕНОРМИРОВКОЙ: остаток растягивается обратно на
    // 0..1, иначе при высоком покрытии всё небо получает одинаковую среднюю
    // плотность и облака теряют форму.
    float base = fbm(q) * profile;
    float d = clamp((base - (1.0 - uCoverage)) / max(uCoverage, 0.05), 0.0, 1.0);
    d = d * d * (3.0 - 2.0 * d);   // мягкие края, плотная середина
    d *= gate;

    // Мелкий шум съедает края: ровная кромка сразу выдаёт математику.
    d -= fbm(q * 4.3 + 7.7) * 0.16 * (1.0 - 0.5 * h);

    // Множитель переводит безразмерный шум в ОСЛАБЛЕНИЕ НА МЕТР. Без него
    // плотность порядка единицы на шаге в сотню метров (а у горизонта шаг
    // именно такой) давала оптическую толщину в десятки: любое облако
    // становилось непрозрачным белым пятном за один шаг.
    return max(d, 0.0) * uCloudDensity * 0.03;
}

// Сколько света доходит до точки внутри облака. Несколько шагов к солнцу —
// этого хватает на объём: важен не точный интеграл, а то, что низ темнее верха.
float cloudLight(vec3 p) {
    float t = 0.0, dens = 0.0;
    float step = (uCloudTop - uCloudBottom) / float(max(uCloudLightSteps, 1)) * 0.6;
    for (int i = 0; i < 8; ++i) {
        if (i >= uCloudLightSteps) break;
        t += step;
        dens += cloudLightDensity(p + uSunDir * t) * step;
    }
    return exp(-dens * 0.9);
}

void main() {
    vec2 uv = vUV;
    vec2 sceneInfo = texture(uSceneDist, uv).rg;
    float sceneDist = sceneInfo.r;
    bool sky = sceneInfo.g > 0.5;

    // Мировой луч через пиксель.
    vec4 far = uInvViewProj * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    vec3 farPos = far.xyz / far.w;
    vec3 dir = normalize(farPos - uCamPos);

    // Сдвиг начала марша: упорядоченный шум по пикселю ПЛЮС сдвиг кадра.
    //
    // Без пиксельной части малое число шагов даёт кольца-«ступени». Без
    // КАДРОВОЙ части шум замирает узором на экране — та самая «зернистость,
    // которая не уходит». Меняя его каждый кадр и усредняя историей (см.
    // проход накопления), получаем и отсутствие полос, и отсутствие зерна.
    vec2 px = uv / max(uJitter, vec2(1e-6));
    float dither = fract(52.9829189 * fract(0.06711056 * px.x + 0.00583715 * px.y)
                         + uFrameJitter);

    // Отладка: вместо результата показываем, что марш ВИДИТ как тень. Чёрное —
    // затенённый воздух, белое — освещённый. Без такой картинки «лучей нет»
    // неотличимо от «карта теней в этот проход не пришла».
    if (uDebug == 1) {
        float march = min(sceneDist, uMaxDist);
        float acc = 0.0;
        for (int i = 0; i < 32; ++i) {
            acc += sunVisibility(uCamPos + dir * (march * (float(i) + 0.5) / 32.0));
        }
        // Альфа 1 — предумноженное смешивание ЗАМЕНИТ кадр, а не добавит к
        // нему: поверх светлой сцены добавка всегда белая и ничего не говорит.
        FragColor = vec4(vec3(acc / 32.0), 1.0);
        return;
    }

    vec3 scatter = vec3(0.0);
    // Сколько света ПРОХОДИТ сквозь дымку от геометрии к глазу. Без этого
    // множителя проход только добавляет свет и ничего не отнимает: при
    // сколько-нибудь заметной плотности кадр уходит в белое, потому что энергия
    // берётся из ниоткуда. Дымка обязана и подсвечивать, и приглушать то, что
    // за ней.
    float aerial = 1.0;

    // --- Лучи в воздухе ---
    if (uShafts != 0) {
        float march = min(sceneDist, uMaxDist);
        int steps = max(uSteps, 1);
        float ph = phaseHG(dot(dir, uSunDir), uAniso);

        // РАСТУЩИЙ ШАГ. Равномерный тратит поровну на первый метр и на сотый, а
        // видно их по-разному: плотность падает с высотой и с расстоянием, и
        // ошибка ближнего шага стоит полосы поперёк кадра, дальнего — ничего.
        // Ряд с постоянным отношением даёт вдвое более мелкий шаг у камеры при
        // том же числе выборок.
        float growth = 1.05;
        float norm = (pow(growth, float(steps)) - 1.0) / (growth - 1.0);
        float dt = march / norm;   // длина ПЕРВОГО шага

        float t = 0.0;
        float transmittance = 1.0;
        for (int i = 0; i < 128; ++i) {
            if (i >= steps || t >= march) break;
            float seg = min(dt, march - t);
            vec3 p = uCamPos + dir * (t + seg * dither);
            // Плотность падает с высотой: у воды дымка густая, наверху её нет.
            float d = uDensity * exp(-max(p.y - uBaseHeight, 0.0) * uHeightFalloff);
            if (d > 1e-5) {
                float vis = sunVisibility(p);
                // Аналитическое интегрирование отрезка вместо «плотность на
                // длину»: при густой дымке и длинном шаге прямоугольник
                // переоценивает вклад в разы, и дальние шаги светятся ярче
                // ближних — ровно наоборот тому, как ведёт себя среда.
                float att = exp(-d * seg);
                scatter += uSunColor * (vis * ph * transmittance * (1.0 - att));
                transmittance *= att;
                if (transmittance < 0.02) break;   // дальше вклада уже не видно
            }
            t += seg;
            dt *= growth;
        }
        // Фаза уже нормирована (интеграл по сфере = 1) — домножать на 4π
        // нельзя, иначе взгляд в сторону солнца даёт вспышку в разы ярче
        // самого солнца и весь кадр уходит в белое.
        scatter *= uIntensity;
        aerial = transmittance;
    }

    // --- Облака ---
    float cloudAlpha = 0.0;
    vec3 cloudColor = vec3(0.0);
    if (uClouds != 0 && sky && dir.y > 0.01) {
        float t0 = (uCloudBottom - uCamPos.y) / dir.y;
        float t1 = (uCloudTop - uCamPos.y) / dir.y;
        if (t1 > t0) {
            t0 = max(t0, 0.0);
            // Потолок пути внутри слоя. У горизонта луч идёт сквозь облака
            // километрами, и без ограничения шаг раздувается до сотен метров —
            // клубы превращаются в полосы.
            float span = min(t1 - t0, 2200.0);
            float dt = span / float(max(uCloudSteps, 1));
            float t = t0 + dt * dither;
            float trans = 1.0;
            float ph = phaseHG(dot(dir, uSunDir), 0.35);
            for (int i = 0; i < 128; ++i) {
                if (i >= uCloudSteps || trans < 0.02) break;
                vec3 p = uCamPos + dir * t;
                float d = cloudAt(p);
                if (d > 0.001) {
                    // Световой марш — самое дорогое место прохода. На еле
                    // заметном облаке его результат всё равно тонет в альфе,
                    // поэтому там берём приближение вместо десятка выборок.
                    float lit = d > 0.004 ? cloudLight(p) : 0.85;
                    // Powder: у самой кромки свет успевает рассеяться назад, и
                    // край облака на просвет ярче середины. Множится только на
                    // ПРЯМОЙ свет — на подсветку неба он не влияет, и без этого
                    // разделения облако выходит равномерно серым комом.
                    float powder = 1.0 - exp(-d * 4.0);
                    float hf = clamp((p.y - uCloudBottom) / max(uCloudTop - uCloudBottom, 1.0), 0.0, 1.0);
                    // Прямой свет солнца сквозь толщу + многократное рассеяние
                    // (второе слагаемое): без него освещённая сторона облака
                    // выходит темнее неба, и облака читаются грязными пятнами.
                    vec3 direct = uSunColor * lit * (ph * 2.2 + 0.55) * mix(0.55, 1.0, powder);
                    // Небо освещает облако сверху сильнее, чем снизу.
                    vec3 ambient = mix(uSkyHorizon, uSkyTop, 0.6) * mix(0.35, 1.05, hf);
                    vec3 lum = direct + ambient;
                    float a = 1.0 - exp(-d * dt * 0.9);
                    cloudColor += lum * a * trans;
                    cloudAlpha += a * trans;
                    trans *= 1.0 - a;
                }
                t += dt;
            }
            cloudColor *= uTint;
            // У горизонта слой обязан растворяться, и растворяться ШИРОКО.
            // Луч, идущий почти параллельно слою, проходит сквозь него
            // километры и набирает непрозрачность там, где глаз ждёт далёкую
            // дымку, — получается сплошная стена облаков по краю кадра.
            float horizon = smoothstep(0.02, 0.30, dir.y);
            cloudAlpha *= horizon;
            cloudColor *= horizon;
        }
    }

    // Итог в предумноженной альфе. Альфа — насколько кадр за проходом закрыт:
    // дымкой (1 − aerial) и облаком, вместе. Облако вдобавок приглушается той
    // же дымкой — оно за ней.
    float a = 1.0 - aerial * (1.0 - clamp(cloudAlpha, 0.0, 1.0));
    FragColor = vec4(scatter + cloudColor * aerial, clamp(a, 0.0, 1.0));
}
)";

// --- 3. Накопление по кадрам --------------------------------------------------
//
// Марш сдвигает начало шага случайно, поэтому ОДИН кадр всегда зернист: это
// цена отсутствия полос. Усреднение по времени убирает зерно, не возвращая
// полос, — но только если история честно перепроецирована в текущий кадр и
// отброшена там, где перепроецировать нечего.
//
// Ограничение окрестностью (neighborhood clamp) вместо сравнения глубин: у
// объёма нет одной поверхности, к которой можно привязать проверку, зато есть
// простое правило — накопленное значение не имеет права выходить за пределы
// того, что даёт текущий кадр рядом. При резком повороте или выходе объекта
// из-за угла история сама сжимается к новому ответу за пару кадров, и «шлейфа»
// не остаётся.
const char* kTemporalFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uCurrent;
uniform sampler2D uHistory;
uniform sampler2D uSceneDist;
uniform mat4 uInvViewProj;
uniform mat4 uPrevViewProj;
uniform vec3 uCamPos;
uniform vec2 uTexel;
uniform float uBlend;      // доля истории
uniform int   uHasHistory;

void main() {
    vec4 cur = texture(uCurrent, vUV);
    if (uHasHistory == 0) { FragColor = cur; return; }

    vec2 info = texture(uSceneDist, vUV).rg;
    // Точка, по которой ищем этот же пиксель в прошлом кадре. Для геометрии —
    // сама поверхность; для неба — очень далёкая точка вдоль луча: облака стоят
    // фактически на бесконечности, и привязывать их к дальности марша значило
    // бы тащить их за камерой при каждом шаге вбок.
    vec4 far = uInvViewProj * vec4(vUV * 2.0 - 1.0, 1.0, 1.0);
    vec3 dir = normalize(far.xyz / far.w - uCamPos);
    float dist = info.g > 0.5 ? 5000.0 : info.r;
    vec3 world = uCamPos + dir * dist;

    vec4 clip = uPrevViewProj * vec4(world, 1.0);
    if (clip.w <= 0.0) { FragColor = cur; return; }
    vec2 prevUV = (clip.xy / clip.w) * 0.5 + 0.5;
    if (prevUV.x < 0.0 || prevUV.x > 1.0 || prevUV.y < 0.0 || prevUV.y > 1.0) {
        FragColor = cur;   // в прошлом кадре этого пикселя не было
        return;
    }

    // Границы допустимого — по окрестности 3x3 текущего кадра.
    vec4 lo = cur, hi = cur;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec4 s = texture(uCurrent, vUV + vec2(float(x), float(y)) * uTexel);
            lo = min(lo, s);
            hi = max(hi, s);
        }
    }
    vec4 hist = clamp(texture(uHistory, prevUV), lo, hi);
    FragColor = mix(cur, hist, uBlend);
}
)";

// --- 4. Подъём в полный размер и композит -------------------------------------
//
// Обычная билинейная фильтрация протекла бы через силуэты: за краем мачты
// лежит небо, и половина выборок пришла бы оттуда — вокруг тонких предметов
// появилась бы светящаяся кайма. Здесь берутся ЧЕТЫРЕ ближайших текселя
// уменьшенного буфера с билинейными весами, домноженными на близость
// расстояния до поверхности. На ровной поверхности веса совпадают с чистой
// билинейной выборкой (ничего не размывается), на силуэте — выигрывают
// совпавшие выборки.
const char* kUpsampleFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uVolume;
uniform sampler2D uSceneDistLow;
uniform sampler2D uDepthFull;
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
uniform vec2 uLowSize;     // размер уменьшенного буфера в текселях
uniform float uMaxDist;

void main() {
    float dFull = texture(uDepthFull, vUV).r;
    float distFull = uMaxDist;
    if (dFull < 0.9999) {
        vec4 wp = uInvViewProj * vec4(vUV * 2.0 - 1.0, dFull * 2.0 - 1.0, 1.0);
        distFull = length(wp.xyz / wp.w - uCamPos);
    }

    // Координаты в текселях уменьшенного буфера и четыре ближайших центра.
    vec2 st = vUV * uLowSize - 0.5;
    vec2 base = floor(st);
    vec2 f = st - base;

    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            vec2 texel = base + vec2(float(x), float(y)) + 0.5;
            vec2 uv = texel / uLowSize;
            float bilinear = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
            float distLow = texture(uSceneDistLow, uv).r;
            // Порог по РАЗНИЦЕ РАССТОЯНИЙ, а не по абсолютной глубине: на
            // ровной стене соседи отличаются на сантиметры и вес не падает, на
            // силуэте разница — метры, и чужая выборка выбывает.
            float w = bilinear / (1.0 + abs(distLow - distFull) * 2.0);
            sum += texture(uVolume, uv) * w;
            wsum += w;
        }
    }
    FragColor = sum / max(wsum, 1e-5);
}
)";

// Прямая копия: при полном разрешении подъёма нет вовсе, и любой фильтр здесь
// был бы чистой потерей резкости.
const char* kBlitFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uVolume;
void main() { FragColor = texture(uVolume, vUV); }
)";

Shader& DepthShader() {
    static Shader* s = new Shader(Shader::FromSource(kVert, kDepthFrag, "Volumetrics.Depth"));
    return *s;
}
Shader& MarchShader() {
    static Shader* s = new Shader(Shader::FromSource(kVert, kMarchFrag, "Volumetrics.March"));
    return *s;
}
Shader& TemporalShader() {
    static Shader* s = new Shader(Shader::FromSource(kVert, kTemporalFrag, "Volumetrics.Temporal"));
    return *s;
}
Shader& UpsampleShader() {
    static Shader* s = new Shader(Shader::FromSource(kVert, kUpsampleFrag, "Volumetrics.Upsample"));
    return *s;
}
Shader& BlitShader() {
    static Shader* s = new Shader(Shader::FromSource(kVert, kBlitFrag, "Volumetrics.Blit"));
    return *s;
}

std::unique_ptr<RenderTarget> MakeColor(int w, int h) {
    RenderTargetDesc d;
    d.Width = std::max(w, 1);
    d.Height = std::max(h, 1);
    d.Kind = RenderTargetKind::ColorHDR;
    return GraphicsDevice::Get().CreateRenderTarget(d);
}

} // namespace

VolumetricSettings VolumetricsFromConfig(const sage::EngineConfig& cfg) {
    VolumetricSettings v;
    v.Enabled = cfg.Volumetrics;
    v.LightShafts = cfg.VolumetricShafts;
    v.Clouds = cfg.VolumetricClouds;
    v.Density = cfg.VolumetricDensity;
    v.Intensity = cfg.VolumetricIntensity;
    v.Steps = cfg.VolumetricSteps;
    v.CloudSteps = cfg.CloudSteps;
    v.Coverage = cfg.CloudCoverage;
    v.Scale = cfg.VolumetricScale;
    v.Debug = cfg.VolumetricDebug;
    v.MaxDistance = cfg.VolumetricMaxDistance;
    v.HeightFalloff = cfg.VolumetricHeightFalloff;
    v.CloudBottom = cfg.CloudBottom;
    v.CloudTop = cfg.CloudTop;
    v.Temporal = cfg.VolumetricTemporal;
    v.TemporalBlend = cfg.VolumetricTemporalBlend;
    v.Anisotropy = cfg.VolumetricAnisotropy;
    v.CloudDensity = cfg.CloudDensity;
    return v;
}

void Volumetrics::ResetHistory() {
    for (auto& kv : m_history) kv.second.Valid = false;
}

void Volumetrics::EnsureShared(int w, int h) {
    if (w == m_w && h == m_h && m_march && m_depthLow) return;
    m_w = w;
    m_h = h;
    m_march = MakeColor(w, h);
    m_depthLow = MakeColor(w, h);
    // Размер прохода изменился — накопленное описывает другой растр.
    ResetHistory();
}

Volumetrics::ViewHistory& Volumetrics::HistoryFor(int viewId, int w, int h) {
    ViewHistory& v = m_history[viewId];
    if (v.Width != w || v.Height != h || !v.Accum[0] || !v.Accum[1]) {
        v.Accum[0] = MakeColor(w, h);
        v.Accum[1] = MakeColor(w, h);
        v.Width = w;
        v.Height = h;
        v.Valid = false;
        v.Current = 0;
    }
    return v;
}

void Volumetrics::Render(Framebuffer& target, sage::rhi::TextureHandle sceneDepth, int w, int h,
                         const glm::mat4& proj, const glm::mat4& view, const glm::vec3& camPos,
                         const LightingEnvironment& env, const ShadowBinding& shadows,
                         const VolumetricSettings& s, float time, int viewId) {
    if (!s.Enabled || (!s.LightShafts && !s.Clouds)) return;
    if (!sceneDepth.Valid() || w < 8 || h < 8) return;
    SAGE_PROFILE("Объёмный свет");

    GraphicsDevice& device = GraphicsDevice::Get();
    if (!m_fsTri) m_fsTri = device.CreateGeometry(VertexLayout{});

    const float scale = glm::clamp(s.Scale, 0.25f, 1.0f);
    const int lw = std::max(8, (int)((float)w * scale));
    const int lh = std::max(8, (int)((float)h * scale));
    const bool fullRes = (lw == w && lh == h);
    EnsureShared(lw, lh);
    ++m_frame;

    const glm::mat4 viewProj = proj * view;
    const glm::mat4 invViewProj = glm::inverse(viewProj);
    const glm::vec3 sunDir = glm::normalize(-env.Sun.Direction);   // НА солнце
    const float maxDist = std::max(s.MaxDistance, 1.0f);

    device.SetDepthTest(false);
    device.SetBlend(false);

    // --- 1. Глубина прохода: расстояние до поверхности + признак неба ---
    m_depthLow->Bind();
    Shader& dep = DepthShader();
    dep.Use();
    dep.SetInt("uDepth", 0);
    device.BindTexture2D(0, sceneDepth);
    dep.SetMat4("uInvViewProj", invViewProj);
    dep.SetVec3("uCamPos", camPos);
    dep.SetVec2("uFullTexel", glm::vec2(1.0f / (float)w, 1.0f / (float)h));
    dep.SetInt("uBlock", glm::clamp((int)std::lround(1.0f / scale), 1, 4));
    dep.SetFloat("uMaxDist", maxDist);
    m_fsTri->DrawArrays(3);

    // --- 2. Марш в буфере прохода ---
    m_march->Bind();
    device.SetClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    device.Clear();

    Shader& march = MarchShader();
    march.Use();
    march.SetInt("uSceneDist", 0);
    device.BindTexture2D(0, m_depthLow->ColorTextureHandle());
    march.SetMat4("uInvViewProj", invViewProj);
    march.SetVec3("uCamPos", camPos);
    march.SetVec3("uSunDir", sunDir);
    march.SetVec3("uSunColor", glm::vec3(env.Sun.Color) * env.Sun.Intensity);
    march.SetVec3("uSkyTop", glm::vec3(env.SkyColor));
    march.SetVec3("uSkyHorizon", glm::vec3(env.Fog.Color));
    march.SetFloat("uTime", time);
    march.SetVec2("uJitter", glm::vec2(1.0f / (float)lw, 1.0f / (float)lh));
    // Золотое сечение по номеру кадра: последовательность равномерно
    // заполняет отрезок и не повторяется коротким периодом, поэтому история
    // усредняет РАЗНЫЕ сдвиги, а не два-три одних и тех же.
    march.SetFloat("uFrameJitter", s.Temporal ? std::fmod((float)m_frame * 0.6180339887f, 1.0f)
                                              : 0.0f);

    march.SetInt("uDebug", s.Debug ? 1 : 0);
    march.SetInt("uShafts", s.LightShafts ? 1 : 0);
    march.SetFloat("uDensity", s.Density);
    march.SetFloat("uAniso", glm::clamp(s.Anisotropy, -0.95f, 0.95f));
    march.SetFloat("uMaxDist", maxDist);
    march.SetFloat("uIntensity", s.Intensity);
    march.SetFloat("uHeightFalloff", s.HeightFalloff);
    march.SetFloat("uBaseHeight", s.BaseHeight);
    march.SetInt("uSteps", glm::clamp(s.Steps, 4, 128));

    march.SetInt("uClouds", s.Clouds ? 1 : 0);
    march.SetFloat("uCloudBottom", s.CloudBottom);
    march.SetFloat("uCloudTop", std::max(s.CloudTop, s.CloudBottom + 10.0f));
    march.SetFloat("uCoverage", glm::clamp(s.Coverage, 0.0f, 1.0f));
    march.SetFloat("uCloudDensity", s.CloudDensity);
    march.SetFloat("uCloudScale", s.CloudScale);
    march.SetVec2("uWind", s.Wind);
    march.SetVec3("uTint", s.Tint);
    march.SetInt("uCloudSteps", glm::clamp(s.CloudSteps, 8, 128));
    march.SetInt("uCloudLightSteps", glm::clamp(s.CloudLightSteps, 1, 8));

    const bool useShadows = shadows.Enabled && s.LightShafts;
    march.SetInt("uShadowsOn", useShadows ? 1 : 0);
    march.SetInt("uCascades", useShadows ? std::min(shadows.Count, 4) : 0);
    for (int i = 0; i < 4; ++i) {
        const int src = std::min(i, ShadowMap::kMaxCascades - 1);
        march.SetMat4(("uLightMat[" + std::to_string(i) + "]").c_str(), shadows.Matrices[src]);
        // Размер текселя каскада — для мягкой выборки. Берётся из привязки, а
        // не угадывается: у каскадов разные карты и разные масштабы.
        const float res = shadows.Resolution > 0 ? (float)shadows.Resolution : 1024.0f;
        march.SetVec2(("uShadowTexel[" + std::to_string(i) + "]").c_str(),
                      glm::vec2(1.0f / res, 1.0f / res));
        const std::string name = "uShadow" + std::to_string(i);
        march.SetInt(name.c_str(), 1 + i);
        if (useShadows && i < shadows.Count)
            device.BindTexture2D(1 + i, shadows.Textures[i]);
        else
            device.BindTexture2D(1 + i, sceneDepth);   // юнит обязан быть привязан
    }
    m_fsTri->DrawArrays(3);

    // --- 3. Накопление по кадрам ---
    sage::rhi::TextureHandle volume = m_march->ColorTextureHandle();
    ViewHistory& hist = HistoryFor(viewId, lw, lh);
    if (s.Temporal && !s.Debug) {
        RenderTarget& dst = *hist.Accum[hist.Current];
        RenderTarget& prev = *hist.Accum[1 - hist.Current];
        dst.Bind();
        Shader& tmp = TemporalShader();
        tmp.Use();
        tmp.SetInt("uCurrent", 0);
        tmp.SetInt("uHistory", 1);
        tmp.SetInt("uSceneDist", 2);
        device.BindTexture2D(0, m_march->ColorTextureHandle());
        device.BindTexture2D(1, prev.ColorTextureHandle());
        device.BindTexture2D(2, m_depthLow->ColorTextureHandle());
        tmp.SetMat4("uInvViewProj", invViewProj);
        tmp.SetMat4("uPrevViewProj", hist.PrevViewProj);
        tmp.SetVec3("uCamPos", camPos);
        tmp.SetVec2("uTexel", glm::vec2(1.0f / (float)lw, 1.0f / (float)lh));
        tmp.SetFloat("uBlend", glm::clamp(s.TemporalBlend, 0.0f, 0.98f));
        tmp.SetInt("uHasHistory", hist.Valid ? 1 : 0);
        m_fsTri->DrawArrays(3);

        volume = dst.ColorTextureHandle();
        hist.Current = 1 - hist.Current;
        hist.Valid = true;
        hist.PrevViewProj = viewProj;
    } else {
        hist.Valid = false;
    }

    // --- 4. Композит в буфер сцены ---
    //
    // Смешивание для ПРЕДУМНОЖЕННОЙ альфы: цвет складывается, а закрытое
    // облаком небо гасится ровно на его непрозрачность. Одна формула на оба
    // эффекта — второй проход не нужен.
    target.Bind();
    device.SetBlend(true);
    device.SetBlendMode(GraphicsDevice::BlendMode::Premultiplied);

    if (fullRes) {
        Shader& blit = BlitShader();
        blit.Use();
        blit.SetInt("uVolume", 0);
        device.BindTexture2D(0, volume);
    } else {
        Shader& up = UpsampleShader();
        up.Use();
        up.SetInt("uVolume", 0);
        up.SetInt("uSceneDistLow", 1);
        up.SetInt("uDepthFull", 2);
        device.BindTexture2D(0, volume);
        device.BindTexture2D(1, m_depthLow->ColorTextureHandle());
        device.BindTexture2D(2, sceneDepth);
        up.SetMat4("uInvViewProj", invViewProj);
        up.SetVec3("uCamPos", camPos);
        up.SetVec2("uLowSize", glm::vec2((float)lw, (float)lh));
        up.SetFloat("uMaxDist", maxDist);
    }
    m_fsTri->DrawArrays(3);

    device.SetBlendMode(GraphicsDevice::BlendMode::Alpha);
    device.SetBlend(false);
    device.SetDepthTest(true);
}

} // namespace sage::render
