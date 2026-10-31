#pragma once

// Часть общего GLSL-блока PBR (см. PbrShader.h: kPbrSharedGlsl собирается из
// частей при компиляции, байт в байт). Разделён по смыслу, чтобы правка теней
// не терялась в тысяче строк вместе с туманом и кластерным светом.
namespace sage::render {

// Uniform'ы освещения, BRDF, тени солнца и локальных источников, отладочные виды.
inline constexpr char kPbrGlslLighting[] = R"GLSL(
#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS 8
// Сколько локальных источников могут отбрасывать тень одновременно. Меньше,
// чем самих источников, и намеренно: карта прожектора стоит целого прохода
// геометрии, карта точечного — шести (см. render/ShadowAtlas.h). Кто попал в
// это число, решает распределитель мест по яркости, а не порядок в сцене.
const float PI = 3.14159265359;

// shadowSlot — номер места в атласе теней, -1 если тени у источника нет.
// Он ЧАСТЬ ИСТОЧНИКА, а не отдельный массив: иначе связь «эта лампа — эта
// плитка» держалась бы на совпадении двух порядков, а они расходятся при
// первом же источнике, которому места не досталось.
struct PointLight { vec3 position; vec3 color; float intensity; float constant; float linear; float quadratic; int shadowSlot; };
struct SpotLight  { vec3 position; vec3 direction; vec3 color; float intensity; float constant; float linear; float quadratic; float cosInner; float cosOuter; int shadowSlot; };

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
// Карты теней СОЛНЦА: по одной на каскад. Массив сэмплеров тут не годится —
// GLSL 330 требует индексировать его константой, а каскад выбирается на лету.
// Поэтому три именованных сэмплера и развёрнутый выбор ниже (см. CalcSunShadow).
uniform sampler2D uShadowMap;   // каскад 0 — самый ближний и подробный
uniform sampler2D uShadowMap1;
uniform sampler2D uShadowMap2;
uniform mat4 uShadowLightSpace[3];
// Размер текселя карты каскада В МЕТРАХ. Нужен, чтобы отступить от поверхности
// вдоль нормали ровно на тексель этого каскада: у дальнего каскада тексель в
// разы крупнее, и один общий отступ для всех либо не спасает ближний от полос
// самозатенения, либо отрывает тень в дальнем.
uniform float uShadowTexelWorld[3];
// Запас по глубине каждого каскада, уже в единицах записанной глубины.
// Считается из размера текселя и глубины ортобокса на стороне процессора
// (ShadowMap::CascadeDepthBias): одной константы на все каскады не существует —
// у ближнего и дальнего разный масштаб и по ширине текселя, и по диапазону.
uniform float uShadowDepthBias[3];
// Полуширина полутени в текселях каскада на единицу глубины «приёмник −
// заслон» (ShadowMap::CascadePenumbraTexels): из неё и разницы глубин — радиус
// фильтра. 0 — резкая тень на всю длину.
uniform float uShadowPenumbra[3];
uniform int uShadowCascades;    // сколько каскадов реально заполнено (1..3)
uniform bool uShadowsEnabled;
// --- Тени локальных источников (см. render/ShadowAtlas.h) ------------------
// Один атлас на все прожекторы и точечные: юнитов под карту-на-источник в
// OpenGL 3.3 просто нет. Плитку источника задаёт прямоугольник в uv атласа.
uniform sampler2D uLocalShadowAtlas;
uniform bool uLocalShadowsEnabled;
uniform float uLocalAtlasTexel;  // 1 / сторона атласа
// Раскладка плиток вместо массивов по местам (их размер был лимитом числа
// теневых ламп): плитка места считается по номеру (LocalShadowTile).
uniform int uLocalAtlasGrid;     // плиток по стороне
uniform float uLocalTileUV;      // сторона плитки в uv
uniform float uLocalTileTexels;  // сторона плитки в текселях
uniform int uLocalSpotCount;     // плиток прожекторов — точечные идут после них
// Где тень начинает растворяться и где её уже нет (метры от камеры).
uniform float uShadowFadeStart;
uniform float uShadowFadeEnd;
// Режим отладочного показа. 0 — обычная отрисовка, всё остальное — разбор
// кадра по слагаемым (см. sage/render/DebugView.h и DebugShade ниже).
uniform int uShadingMode;
// --- Отражения (см. sage/render/Reflection.h) ---
// Карта окружения: небо и сцена вокруг, запечённые в cubemap с мипами. Мип
// выбирается ШЕРОХОВАТОСТЬЮ — нулевой это зеркало, дальние размыты.
uniform samplerCube uEnvMap;
uniform bool uEnvEnabled;
uniform float uEnvIntensity;
uniform float uEnvMaxLod;      // сколько мипов реально есть (шкала шероховатости)
// Параллакс-коррекция: куб снят из ОДНОЙ точки, а смотрят на него из разных.
// Без поправки отражение «плывёт» при движении камеры — как будто окружение
// приклеено к объекту. Поправка ищет пересечение луча с коробкой влияния зонда
// и берёт направление уже к точке пересечения.
uniform bool uEnvBoxParallax;
uniform vec3 uEnvBoxMin;
uniform vec3 uEnvBoxMax;
uniform vec3 uEnvProbePos;
uniform vec2 uScreenTexel;     // 1/размер кадра — экранные буферы читаются по нему
// Плоское отражение (вода, зеркало): сцена, отражённая относительно плоскости.
uniform sampler2D uPlanarMap;
uniform bool uPlanarEnabled;
uniform bool uFogEnabled;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
uniform int uFogMode;          // 0 линейный, 1 высотный экспоненциальный
uniform float uFogDensity;
uniform float uFogFalloff;
uniform float uFogHeight;
uniform float uFogMaxOpacity;
uniform float uFogSunScatter;
uniform float uFogSunExponent;

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
// Френель С УЧЁТОМ ШЕРОХОВАТОСТИ — для РАССЕЯННОГО света, а не для источника.
//
// Обычный Френель на скользящем угле стремится к единице: «вся энергия ушла в
// отражение». Для источника света это верно, для ambient — нет: у рассеянного
// света нет одного направления, и шероховатая поверхность на скользящем угле
// продолжает рассеивать. Пока доля диффузного считалась обычным Френелем,
// шероховатый бок, повёрнутый к камере ребром, получал НОЛЬ рассеянного света
// и при отсутствии карты окружения становился ЧЁРНЫМ — посреди полностью
// освещённой сцены, без всякой тени. На модели это выглядит как чёрные полосы
// и крапины по краям и на гранях, повёрнутых от камеры.
//
// Формула стандартная (Karis): потолок отражённой доли опущен до
// (1 - шероховатость), то есть матовая поверхность не «зеркалит» даже вскользь.
vec3 FresnelSchlickRoughness(float cosT, vec3 F0, float rough) {
    return F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(clamp(1.0 - cosT, 0.0, 1.0), 5.0);
}

// Одно сравнение глубины: 1 — в тени, 0 — на свету.
float ShadowTest(sampler2D map, vec2 uv, float depth, float bias) {
    return (depth - bias > texture(map, uv).r) ? 1.0 : 0.0;
}

// Радиус диска выборки теней В ТЕКСЕЛЯХ карты. Меньше — на кромке проступает
// лесенка (проверка «кромка тени фильтруется» в tests/render меряет именно её),
// больше — начинает мылить и отъедать тень у тонких предметов. Резкость тени
// при этом задаёт не он, а размер текселя, то есть каскады и разрешение карты:
// радиус измеряется В ТЕКСЕЛЯХ и вместе с ними уменьшается.
//
// Константа ОБЩАЯ для фильтра и для отступа по нормали, и это не вкусовщина.
// Отступ обязан выносить точку выборки ДАЛЬШЕ, чем достаёт самый крайний тап
// фильтра: иначе крайние тапы попадают на текселя, чья записанная глубина чуть
// меньше глубины поверхности, и возвращают «в тени». На большой ровной
// плоскости это даёт мелкую рябь по всему полу — при неподвижном фильтре она
// сложилась бы в полосы, а при повёрнутом на каждый пиксель рассыпается в шум.
// Разъехаться этим двум числам нельзя, поэтому число одно.
const float kShadowFilterTexels = 1.25;

// Билинейная фильтрация СРАВНЕНИЙ, а не глубин. Усреднять глубины соседних
// текселей бессмысленно: между ступенькой и полом среднее даёт глубину, какой
// в сцене нет, и получается ореол. Сравнивать надо в каждом из четырёх
// текселей, а смешивать уже результаты — 0 и 1. Ровно это делает аппаратный
// sampler2DShadow; здесь то же самое руками, без изменения формата текстуры.
//
// Без неё край тени не может быть гладким В ПРИНЦИПЕ: точечная выборка внутри
// одного текселя даёт всем пикселям один и тот же ответ, и сколько таких
// выборок ни делай, получится ступенька шириной в тексель. Промежуточные
// оттенки берутся только отсюда.
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

// Поиск заслонов: сколько из девяти точек (сам фрагмент + восемь вокруг)
// закрыты и на какой
// средней глубине стоит заслон. Точки ТОЧЕЧНЫЕ (одно чтение) и с неподвижным
// узором: оценка ширины полутени не должна шуметь от пикселя к пикселю, иначе
// край тени «кипит» вместе с ней.
float BlockerSearch(sampler2D map, vec4 rect, vec2 uv, float depth, float bias, float radius,
                    vec2 texel, out float avgDepth) {
    const float kGoldenAngle = 2.39996323;
    // Первая точка — сам фрагмент: тень тонкого предмета (жердь, трос) уже
    // текселя, и кольцо точек вокруг может пройти мимо неё — тень пропала бы.
    float count = 0.0, sum = 0.0;
    {
        float d = texture(map, clamp(uv, rect.xy, rect.zw)).r;
        if (depth - bias > d) {
            count += 1.0;
            sum += d;
        }
    }
    for (int i = 0; i < 8; ++i) {
        float r = radius * sqrt((float(i) + 0.5) / 8.0);
        float a = float(i) * kGoldenAngle;
        vec2 p = clamp(uv + vec2(cos(a), sin(a)) * r * texel, rect.xy, rect.zw);
        float d = texture(map, p).r;
        if (depth - bias > d) {
            count += 1.0;
            sum += d;
        }
    }
    avgDepth = count > 0.0 ? sum / count : depth;
    return count;
}

// Диск Вогеля из билинейных сравнений (8 точек = 32 чтения) радиусом radius
// текселей. rect — допустимая область выборки (xy — нижний левый, zw —
// верхний правый угол в uv): у атласа это своя плитка.
float FilterDisk(sampler2D map, vec4 rect, vec2 uv, float depth, float bias, float radius,
                 float rotation, vec2 texel) {
    const float kGoldenAngle = 2.39996323;
    float shadow = 0.0;
    for (int i = 0; i < 8; ++i) {
        float r = radius * sqrt((float(i) + 0.5) / 8.0);
        float a = float(i) * kGoldenAngle + rotation;
        vec2 p = clamp(uv + vec2(cos(a), sin(a)) * r * texel, rect.xy, rect.zw);
        shadow += ShadowBilinear(map, p, depth, bias, texel);
    }
    return shadow / 8.0;
}

// Тень по одной карте. pc — координаты фрагмента в пространстве этого каскада,
// уже приведённые к [0,1] (смещение по нормали к ним УЖЕ применено — см.
// CascadeCoords).
//
// ПОЛУТЕНЬ ПО РАССТОЯНИЮ ДО ЗАСЛОНА (как у настоящего солнца). Раньше фильтр
// был одного радиуса везде: тень одинаково мылилась и у основания столба, и в
// десяти метрах от него — неестественно мягко там, где предмет касается земли,
// и неестественно резко вдали. Теперь радиус = (глубина приёмника − глубина
// заслона) × penumbra: у самого предмета тень резкая, с удалением от него —
// расплывается. penumbra считается из углового размера солнца
// (ShadowSettings::SunAngle).
//
// РАННИЙ ВЫХОД — главная экономия. Поиск заслонов стоит девяти чтений; если
// ни одна точка не закрыта, фрагмент освещён и фильтр (32 чтения) не нужен.
// Таких фрагментов в кадре большинство. Если закрыты все девять, а полутень
// уже области поиска, фрагмент целиком в тени — тоже без фильтра.
//
// Фильтр — диск Вогеля со случайным поворотом на пиксель: регулярная сетка
// давала полутени считанное число ступеней яркости, которые на пологой кромке
// читаются полосами, а поворот рассыпает их в мелкий шум. Точки остаются
// БИЛИНЕЙНЫМИ: точечная выборка внутри текселя отвечает всем пикселям
// одинаково, и край тени становится ступенькой шириной в тексель.
float ShadowPCF(sampler2D map, vec3 pc, float ndl, float rotation, float baseBias, float penumbra) {
    vec2 texel = 1.0 / vec2(textureSize(map, 0));
    vec4 rect = vec4(0.0, 0.0, 1.0, 1.0);

    // Наклонный bias: у поверхности, стоящей боком к свету, глубина внутри
    // одного текселя меняется сильнее, и запас нужен больше. Базовая величина
    // приходит из каскада (см. uShadowDepthBias), здесь только наклон.
    float slope = clamp(1.0 - abs(ndl), 0.0, 1.0);
    float bias = baseBias * (1.0 + 3.0 * slope);

    const float kMaxRadius = 8.0;
    // Искать заслоны надо так далеко, как широка самая широкая полутень, какую
    // может дать эта точка (заслон у самого источника), — но не дальше
    // потолка фильтра.
    float search = clamp(pc.z * penumbra, kShadowFilterTexels, kMaxRadius * 0.75);
    float blockerDepth;
    float blockers = BlockerSearch(map, rect, pc.xy, pc.z, bias, search, texel, blockerDepth);
    if (blockers == 0.0) return 0.0;

    float radius = clamp((pc.z - blockerDepth) * penumbra, 0.0, kMaxRadius);
    radius = max(radius, kShadowFilterTexels);
    if (blockers == 9.0 && radius <= search) return 1.0;
    // Шире фильтр — дальше тапы от центра по наклонной поверхности: запас
    // растёт вместе с ним, иначе широкая полутень сама себя затеняет.
    float wideBias = bias * max(1.0, radius / kShadowFilterTexels);
    return FilterDisk(map, rect, pc.xy, pc.z, wideBias, radius, rotation, texel);
}

// Угол поворота диска выборки — свой у каждого пикселя (interleaved gradient
// noise). Без него все пиксели берут пробы в одних и тех же местах, и полутень
// получает считаное число ступеней яркости, которые на пологой кромке видны
// полосами. С поворотом те же ступени рассыпаются в мелкий шум, а глаз
// собирает его в ровный градиент. Общий для солнца и для локальных теней:
// разные повороты у разных источников дали бы разный характер шума на одной
// поверхности.
float ShadowRotation() {
    float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy,
                                             vec2(0.06711056, 0.00583715))));
    return ign * 6.2831853;
}

// --- Тени локальных источников ---------------------------------------------
//
// Отличие от солнца одно, но оно меняет всю арифметику: проекция ПЕРСПЕКТИВНАЯ.
// У солнца тексель карты — одна величина на весь каскад, и отступы считаются
// один раз на процессоре. Здесь тексель растёт линейно с расстоянием от лампы:
// у самой лампы он миллиметровый, у границы дальности — дециметровый. Поэтому
// и отступ вдоль нормали, и запас по глубине считаются ЗДЕСЬ, из расстояния до
// источника — по углу проекции и размеру плитки (uLocalTileTexels).

// Ближняя плоскость карт локального света — та же, что LocalShadowBinding::kNear.
const float kLocalShadowNear = 0.05;

// Плитка атласа по номеру: xy — левый нижний угол в uv, z — сторона.
vec4 LocalShadowTile(int index) {
    int g = max(uLocalAtlasGrid, 1);
    return vec4(vec2(float(index - (index / g) * g), float(index / g)) * uLocalTileUV, uLocalTileUV, 0.0);
}

// Дальность лампы из её затухания (Linear = 4.5 / Range, см. scene/Light.h):
// так она есть и в кластерном пути, и в старых массивах источников.
float LightRangeFromLinear(float linear) { return max(4.5 / max(linear, 1e-6), kLocalShadowNear * 2.0); }

// Глубина, записанная в карту, для точки на расстоянии dist вдоль оси взгляда
// источника. Повторяет то, что делает перспективная матрица: глубина в буфере
// нелинейна, и сравнивать с ней метры нельзя.
float PerspectiveDepth01(float dist, float nearZ, float farZ) {
    float denom = max(farZ - nearZ, 1e-4);
    float ndc = ((farZ + nearZ) - 2.0 * farZ * nearZ / max(dist, 1e-4)) / denom;
    return ndc * 0.5 + 0.5;
}

// Перевод мирового запаса (метры) в единицы записанной глубины на расстоянии
// dist. Производная PerspectiveDepth01 по dist: near*far/((far-near)*dist^2).
// Константы здесь не существует в принципе — у поверхности в метре от лампы и
// в двадцати метрах один и тот же запас отличается в четыреста раз.
float LocalDepthBias(float dist, float nearZ, float farZ, float slackMetres) {
    float denom = max(farZ - nearZ, 1e-4);
    return slackMetres * (nearZ * farZ) / (denom * max(dist * dist, 1e-4));
}

// Выборка из ПЛИТКИ атласа: тот же диск Вогеля и та же билинейная фильтрация
// сравнений, что у солнца, но с зажимом в границы своей плитки.
//
// Зажим обязателен и не имеет аналога в солнечном пути. Там каждый каскад —
// отдельная текстура, и уход фильтра за край даёт лишь повтор крайнего текселя.
// Здесь за краем плитки лежит карта ДРУГОЙ лампы, и один-единственный тап,
// перешагнувший границу, приносит её глубину — то есть тень предмета из другого
// конца уровня, отпечатанную поперёк пола.
float AtlasShadowPCF(vec4 rect, vec2 uv, float depth, float bias, float rotation) {
    vec2 texel = vec2(uLocalAtlasTexel);
    // Полтора текселя запаса: билинейная выборка читает соседний тексель, и
    // зажимать надо с учётом её тапов, а не только центра.
    vec4 bounds = vec4(rect.xy + texel * 1.5, rect.xy + vec2(rect.z) - texel * 1.5);
    vec2 p = rect.xy + uv * rect.z;
    // Тот же ранний выход, что у солнца: девять точечных чтений решают, есть
    // ли рядом заслон, и освещённый фрагмент (а их большинство) фильтр не
    // платит. Размер лампы неизвестен — полутень постоянная.
    float blockerDepth;
    float blockers = BlockerSearch(uLocalShadowAtlas, bounds, p, depth, bias, kShadowFilterTexels,
                                   texel, blockerDepth);
    if (blockers == 0.0) return 0.0;
    if (blockers == 9.0) return 1.0;
    return FilterDisk(uLocalShadowAtlas, bounds, p, depth, bias, kShadowFilterTexels, rotation, texel);
}

// Тень прожектора. dist — расстояние от фрагмента до лампы (у вызывающего оно
// уже посчитано для затухания, и считать его второй раз незачем).
//
// Проекцию строим здесь же — та же, что LocalShadowAtlas::Prepare на
// процессоре: lookAt(позиция, позиция + направление, верх) и перспектива на
// полный внешний угол конуса. Массивов матриц по местам больше нет — их размер
// и был потолком числа теневых прожекторов.
float SpotShadow(int slot, vec3 lightPos, vec3 lightDir, float cosOuter, float linear,
                 vec3 worldPos, vec3 normal, float ndl, float dist, float rotation) {
    float outer = clamp(acos(clamp(cosOuter, -1.0, 1.0)), radians(1.0), radians(87.0));
    float far = LightRangeFromLinear(linear);
    float focal = 1.0 / tan(outer);
    float slope = clamp(1.0 - abs(ndl), 0.0, 1.0);
    float texelWorld = dist * (2.0 / focal) / max(uLocalTileTexels, 1.0);
    float offset = texelWorld * kShadowFilterTexels * 1.5 * (1.0 + 2.0 * slope);

    vec3 f = normalize(lightDir);
    vec3 up = abs(f.y) > 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 s = normalize(cross(f, up));
    vec3 u = cross(s, f);
    vec3 rel = worldPos + normal * offset - lightPos;
    float axis = dot(rel, f);
    // За спиной у лампы карты нет: деление на такое расстояние дало бы
    // координаты, зеркально отражённые в её плитку, то есть чужую тень.
    if (axis <= kLocalShadowNear) return 0.0;
    vec2 uv = vec2(dot(rel, s), dot(rel, u)) * focal / axis * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 0.0;
    float depth = PerspectiveDepth01(axis, kLocalShadowNear, far);
    if (depth > 1.0) return 0.0;  // дальше дальности лампы — она туда не светит
    float bias = LocalDepthBias(axis, kLocalShadowNear, far, texelWorld * (1.0 + 3.0 * slope));
    return AtlasShadowPCF(LocalShadowTile(slot), uv, depth, bias, rotation);
}

// Грань куба точечного света и координаты внутри её плитки. Ручной аналог того,
// что делает samplerCube: наибольшая по модулю компонента задаёт грань, две
// другие — положение внутри неё. Через настоящий samplerCube это стоило бы
// отдельного текстурного юнита на КАЖДУЮ лампу; здесь все грани всех ламп
// лежат в одном атласе.
//
// Таблица граней та же, что на процессоре (см. CubeFaces() в ShadowAtlas.h) —
// разъехаться им нельзя, поэтому она проверена тестом, сравнивающим эту
// формулу с настоящими матрицами граней.
int PointFace(vec3 v, out vec2 uv, out float axis) {
    vec3 a = abs(v);
    vec2 sc;
    int face;
    if (a.x >= a.y && a.x >= a.z) {
        axis = a.x;
        if (v.x > 0.0) { face = 0; sc = vec2(-v.z, -v.y); }
        else           { face = 1; sc = vec2( v.z, -v.y); }
    } else if (a.y >= a.z) {
        axis = a.y;
        if (v.y > 0.0) { face = 2; sc = vec2(v.x,  v.z); }
        else           { face = 3; sc = vec2(v.x, -v.z); }
    } else {
        axis = a.z;
        if (v.z > 0.0) { face = 4; sc = vec2( v.x, -v.y); }
        else           { face = 5; sc = vec2(-v.x, -v.y); }
    }
    // Угол грани ровно 90°: tan(45°) == 1, и проекция сводится к делению.
    uv = sc / max(axis, 1e-4) * 0.5 + 0.5;
    return face;
}

float PointShadow(int slot, vec3 worldPos, vec3 normal, vec3 lightPos, float linear, float ndl,
                  float rotation) {
    float far = LightRangeFromLinear(linear);
    float slope = clamp(1.0 - abs(ndl), 0.0, 1.0);
    float dist = length(worldPos - lightPos);
    // Грань 90°: ширина на расстоянии d равна 2d — отсюда тексель.
    float texelWorld = dist * 2.0 / max(uLocalTileTexels, 1.0);
    float offset = texelWorld * kShadowFilterTexels * 1.5 * (1.0 + 2.0 * slope);
    vec3 v = (worldPos + normal * offset) - lightPos;

    vec2 uv;
    float axis;
    int face = PointFace(v, uv, axis);
    if (axis <= kLocalShadowNear) return 0.0;   // ближе ближней плоскости — в карту не писалось
    float depth = PerspectiveDepth01(axis, kLocalShadowNear, far);
    if (depth > 1.0) return 0.0;     // дальше дальности лампы
    float bias = LocalDepthBias(axis, kLocalShadowNear, far, texelWorld * (1.0 + 3.0 * slope));
    return AtlasShadowPCF(LocalShadowTile(uLocalSpotCount + slot * 6 + face), uv, depth, bias, rotation);
}

// Координаты фрагмента в пространстве каскада, приведённые к [0,1], со
// смещением точки выборки ВДОЛЬ НОРМАЛИ.
//
// Смещение делается ЗДЕСЬ, в мире, до проекции в свет, — и это не перестановка
// строк, а исправление. Раньше оно применялось уже к готовым координатам карты:
//
//     uv = pc.xy + normal.xz * texel * (...)
//
// то есть икс и зет МИРОВОЙ нормали подставлялись как сдвиг по осям КАРТЫ. Эти
// оси совпадают ровно в одном случае — когда солнце светит строго вниз. При
// любом другом наклоне карта повёрнута, и смещение уезжало вбок: на одних
// поверхностях оно било мимо и оставляло полосы самозатенения, на других
// перебирало и отрывало тень от предмета. Симптом — «артефакты, зависящие от
// времени суток», и найти его по виду почти невозможно, потому что при
// полуденном солнце всё правильно.
//
// В мире же нормаль есть нормаль: отступаем от поверхности на размер текселя
// ЭТОГО каскада (в метрах) и только потом проецируем.
// БЕЗ ПРОИЗВОДНЫХ (dFdx/dFdy/fwidth). Здесь стоял размер экранного пикселя в
// мире — length(fwidth(worldPos)), — чтобы у горизонта, где один пиксель
// накрывает десятки текселей карты, отступать на большую неопределённость. Идея
// верная, место негодное: CalcSunShadow вызывается из тернарника
// `uShadowsEnabled ? … : 0.0`, и хотя условие однородное, компилятор вправе
// собрать его настоящей ВЕТКОЙ — а производные внутри ветвления спецификация
// объявляет неопределёнными. На программном растеризаторе (и в эталонных кадрах
// CI) это работало, на настоящей видеокарте вернуло мусор: NaN уезжал в тень,
// оттуда в яркость, ACES зажимал NaN в ноль — и ВСЯ освещённая геометрия
// становилась чёрной. Небо и сетка при этом рисовались, потому что идут мимо
// PBR: ровно та картинка, которую видно на скриншоте.
//
// Отступ считается от размера текселя каскада — величины, известной на стороне
// процессора. Скользящий угол закрывает запас по глубине (uShadowDepthBias), он
// тоже считается из текселя и диапазона ортобокса.
vec3 CascadeCoords(int cascade, vec3 worldPos, vec3 normal, float ndl) {
    float slope = clamp(1.0 - abs(ndl), 0.0, 1.0);
    // Множитель 1.5 к радиусу фильтра — это диагональ квадрата текселя
    // (sqrt(2), с запасом): крайний тап диска может уйти по диагонали, и
    // отступ обязан перекрывать именно её, а не сторону.
    float offset = uShadowTexelWorld[cascade] * kShadowFilterTexels * 1.5 * (1.0 + 2.0 * slope);
    vec4 lp = uShadowLightSpace[cascade] * vec4(worldPos + normal * offset, 1.0);
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
//
// Если следующего каскада в этой точке нет, подмешивать НЕЧЕГО — и подмешивать
// туда ноль (то есть «свет») нельзя: по всей границе каскада тень плавно
// выцветала бы в освещённую полосу. В этом случае остаётся значение своего
// каскада — см. CalcSunShadow.
float CascadeEdge(vec3 pc, float band) {
    vec2 d = min(pc.xy, vec2(1.0) - pc.xy); // расстояние до ближайшего края
    float m = min(d.x, d.y);
    return 1.0 - clamp(m / band, 0.0, 1.0);
}

// --- Отладочные виды -----------------------------------------------------
//
// Разбор кадра по слагаемым: показать ровно одну величину вместо результата
// освещения. Нужен затем, что «слишком тёмно» — не диагноз: причиной бывает и
// albedo, и нормаль, и шероховатость, и тень, и AO, и все они на готовом кадре
// выглядят одинаково. Здесь каждую видно отдельно.
//
// Живёт в общем блоке, а не в конкретном шейдере: путей отрисовки два
// (инстансный батч и материал), и вид, сделанный в одном, молча отсутствовал бы
// в другом.
//
// Возвращает true, если режим отладочный и цвет уже посчитан.
bool DebugShade(int mode, vec3 N, vec3 worldPos, vec3 albedo, float metallic,
                float roughness, float ao, vec3 emissive, float shadow, out vec4 outColor) {
    outColor = vec4(0.0, 0.0, 0.0, 1.0);
    if (mode == 0) return false;
    if (mode == 1) { outColor = vec4(albedo + emissive, 1.0); return true; }
    if (mode == 2) { outColor = vec4(N * 0.5 + 0.5, 1.0); return true; }
    if (mode == 3) { outColor = vec4(albedo, 1.0); return true; }
    if (mode == 4) { outColor = vec4(vec3(roughness), 1.0); return true; }
    if (mode == 5) { outColor = vec4(vec3(metallic), 1.0); return true; }
    if (mode == 6) { outColor = vec4(emissive, 1.0); return true; }
    if (mode == 7) { outColor = vec4(vec3(ao), 1.0); return true; }
    if (mode == 8) { outColor = vec4(vec3(1.0 - shadow), 1.0); return true; }
    if (mode == 9) {
        // Глубина от камеры трёхцветной шкалой: близко синее, десяток метров —
        // зелёное, дальше красное и к сотне метров чёрное.
        //
        // Не серый градиент: на открытой сцене почти всё попадает в первые
        // проценты шкалы, и серым это неотличимо от однотонной заливки. По
        // смене ЦВЕТА расстояние читается там, где по яркости уже не читается.
        // Плюс метровые риски — по ним видно и масштаб, и что шкала линейна.
        float d = length(worldPos - uViewPos);
        float t = clamp(d / 100.0, 0.0, 1.0);
        vec3 ramp = t < 0.5 ? mix(vec3(0.1, 0.3, 1.0), vec3(0.2, 1.0, 0.3), t * 2.0)
                            : mix(vec3(0.2, 1.0, 0.3), vec3(1.0, 0.2, 0.1), t * 2.0 - 1.0);
        float tick = smoothstep(0.94, 1.0, abs(fract(d * 0.1) * 2.0 - 1.0));
        outColor = vec4(mix(ramp * (1.0 - t * 0.75), vec3(1.0), tick * 0.35), 1.0);
        return true;
    }
    if (mode == 10) {
        // Каскад тени, в который попал фрагмент: красный/зелёный/синий от
        // ближнего к дальнему, серый — тени нет. По этой картинке сразу видно,
        // не растянут ли ближний каскад на весь кадр.
        for (int i = 0; i < 3; ++i) {
            if (i >= uShadowCascades) break;
            vec3 pc = CascadeCoords(i, worldPos, N, 1.0);
            if (InsideCascade(pc, 0.02)) {
                vec3 tint = i == 0 ? vec3(1.0, 0.35, 0.3)
                          : i == 1 ? vec3(0.35, 1.0, 0.4)
                                   : vec3(0.4, 0.55, 1.0);
                outColor = vec4(tint * (0.35 + 0.65 * dot(N, vec3(0.0, 1.0, 0.0)) * 0.5 + 0.3), 1.0);
                return true;
            }
        }
        outColor = vec4(vec3(0.25), 1.0);
        return true;
    }
    if (mode == 11) {
        // Мировые координаты по модулю метра: клетка размером в метр. По ней
        // видно и масштаб модели, и разрывы развёртки, и то, что объект
        // случайно смасштабирован в сто раз.
        vec3 g = abs(fract(worldPos) - 0.5);
        float line = smoothstep(0.46, 0.5, max(max(g.x, g.y), g.z));
        outColor = vec4(mix(albedo * 0.6 + 0.2, vec3(1.0, 0.85, 0.3), line), 1.0);
        return true;
    }
    return false;
}

float CalcSunShadow(vec3 worldPos, vec3 normal, vec3 sunDir) {
    float ndl = dot(normal, sunDir);

    // Поверхность отвёрнута от солнца — считать тень НЕ НАДО.
    //
    // Это не микрооптимизация. Солнце такую грань не освещает вовсе (вклад
    // умножается на max(dot(N,L),0) == 0), то есть результат выборки ни на что
    // не влияет — а стоит он тридцати двух чтений карты, и на каждый каскад,
    // который фрагмент заденет полосой смешивания. В любой замкнутой сцене от
    // солнца отвёрнута примерно половина видимых граней: столько же выборок
    // делалось впустую каждый кадр.
    if (ndl <= 0.0) return 0.0;
    const float kMargin = 0.02; // запас под диск фильтра у края карты
    const float kBand = 0.10;   // ширина полосы смешивания каскадов

    float rot = ShadowRotation();

    // Каскады перебираются от ближнего к дальнему, и берётся ПЕРВЫЙ
    // подходящий: он самый подробный из тех, что накрывают эту точку.
    //
    // Выбор развёрнут вручную и выглядит длинно. Причина не в лени: GLSL 330
    // запрещает индексировать массив сэмплеров переменной, а sampler нельзя
    // положить в локальную переменную. Цикл здесь физически невозможен.
    vec3 pc0 = CascadeCoords(0, worldPos, normal, ndl);
    if (InsideCascade(pc0, kMargin)) {
        float s = ShadowPCF(uShadowMap, pc0, ndl, rot, uShadowDepthBias[0], uShadowPenumbra[0]);
        if (uShadowCascades > 1) {
            float w = CascadeEdge(pc0, kBand);
            if (w > 0.0) {
                vec3 pc1 = CascadeCoords(1, worldPos, normal, ndl);
                float far = InsideCascade(pc1, kMargin) ? ShadowPCF(uShadowMap1, pc1, ndl, rot, uShadowDepthBias[1], uShadowPenumbra[1]) : s;
                s = mix(s, far, w);
            }
        }
        return s;
    }
    if (uShadowCascades > 1) {
        vec3 pc1 = CascadeCoords(1, worldPos, normal, ndl);
        if (InsideCascade(pc1, kMargin)) {
            float s = ShadowPCF(uShadowMap1, pc1, ndl, rot, uShadowDepthBias[1], uShadowPenumbra[1]);
            if (uShadowCascades > 2) {
                float w = CascadeEdge(pc1, kBand);
                if (w > 0.0) {
                    vec3 pc2 = CascadeCoords(2, worldPos, normal, ndl);
                    float far = InsideCascade(pc2, kMargin) ? ShadowPCF(uShadowMap2, pc2, ndl, rot, uShadowDepthBias[2], uShadowPenumbra[2]) : s;
                    s = mix(s, far, w);
                }
            }
            return s;
        }
    }
    if (uShadowCascades > 2) {
        vec3 pc2 = CascadeCoords(2, worldPos, normal, ndl);
        if (InsideCascade(pc2, kMargin)) return ShadowPCF(uShadowMap2, pc2, ndl, rot, uShadowDepthBias[2], uShadowPenumbra[2]);
    }
    // Дальше последнего каскада тени нет — там всё освещено. Тянуть её краем
    // карты значило бы рисовать полосу ложной тени до горизонта.
    return 0.0;
}

// Тень с растворением у предела дальности.
//
// Без него граница карты видна как РОВНАЯ ЛИНИЯ поперёк земли: с одной стороны
// у всех предметов тени есть, с другой — ни у кого. Глаз читает это как ошибку
// рендера, а не как «дальше тени не считаются», и никакой фильтрацией внутри
// карты это не лечится — дело не в качестве тени, а в том, что она кончается
// разом. Узкая полоса, на которой тень гаснет, убирает линию целиком.
float SunShadow(vec3 worldPos, vec3 normal, vec3 sunDir) {
    float s = CalcSunShadow(worldPos, normal, sunDir);
    if (s <= 0.0) return 0.0;
    float d = length(uViewPos - worldPos);
    return s * (1.0 - smoothstep(uShadowFadeStart, uShadowFadeEnd, d));
}

)GLSL";

} // namespace sage::render
