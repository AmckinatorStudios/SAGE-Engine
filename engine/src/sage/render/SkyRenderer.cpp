#include "sage/render/SkyRenderer.h"

#include <algorithm>

#include "sage/render/ResourceManager.h"
#include "sage/render/Texture.h"
#include "sage/scene/Light.h"

#include <glm/gtc/matrix_transform.hpp>

namespace {

// Полноэкранный треугольник; направление мирового луча восстанавливается из
// обратной проекции + поворота камеры (translation не нужен — небо на
// бесконечности).
const char* kVertexSrc = R"GLSL(
#version 330 core
out vec3 vDir;
uniform mat4 uInvProj;
uniform mat4 uInvViewRot; // поворот view->world в mat4 (SetMat3 в RHI нет)
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); // 0,0 / 2,0 / 0,2
    vec2 ndc = pos * 2.0 - 1.0;                                // -1..3 накрывает экран
    vec4 vp = uInvProj * vec4(ndc, 1.0, 1.0);

    // ВАЖНО: НЕ нормализуем здесь.
    //
    // Здесь и была «кривизна» неба. Растеризатор интерполирует varying ЛИНЕЙНО,
    // и линейно интерполировать можно только точки дальней плоскости — они
    // лежат на плоскости, и промежуточное значение остаётся на ней. Нормаль же
    // делит каждую вершину на СВОЮ длину, а длины у вершин полноэкранного
    // треугольника отличаются в разы (NDC идёт от -1 до 3, угол куда дальше
    // центра). После такого деления линейная интерполяция даёт направления,
    // которые не соответствуют ни одному лучу камеры: градиент неба сжимается,
    // горизонт уезжает и перекашивается, и чем шире кадр, тем сильнее.
    //
    // Поэтому в varying уходит ПОЗИЦИЯ на дальней плоскости, а нормализация —
    // во фрагментном шейдере, где она уже поточечная и ничего не искажает.
    vDir = mat3(uInvViewRot) * (vp.xyz / vp.w);
    gl_Position = vec4(ndc, 1.0, 1.0); // z=1 — дальняя плоскость
}
)GLSL";

const char* kFragmentSrc = R"GLSL(
#version 330 core
in vec3 vDir;
out vec4 FragColor;
uniform vec3 uTop;
uniform vec3 uHorizon;
uniform float uCelestials;   // 0 — светил нет (голый градиент, как было)
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform float uSunSize;
uniform float uMoon;
uniform vec3 uMoonColor;
uniform float uMoonSize;
uniform float uStars;
uniform float uDay;   // 1 день, 0 ночь — по нему гаснут звёзды и бледнеет луна
uniform int uHeightFog;
uniform vec3 uFogColor;
uniform float uFogDensity;
uniform float uFogFalloff;
uniform float uFogHeight;
uniform float uFogMaxOpacity;
uniform float uFogSunScatter;
uniform float uFogSunExponent;
uniform vec3 uFogSunLight;
uniform vec3 uCamPos;
// Форма неба (см. SkyboxSettings).
uniform float uGradExp;
uniform float uHorizonSoft;
uniform float uHorizonOffset;
uniform int uGround;
uniform vec3 uGroundColor;
uniform float uGroundBlend;
uniform int uSunShape;       // 0 круг, 1 квадрат
uniform int uMoonShape;
uniform float uSunBrightness;
uniform float uSunGlow;
uniform int uMoonPhase;
uniform int uSunTex;
uniform int uMoonTex;
uniform sampler2D uSunMap;
uniform sampler2D uMoonMap;
uniform float uStarDensity;
uniform float uStarSize;
uniform int uClouds;
uniform int uCloudStyle;     // 0 блоки, 1 мягкие
uniform vec3 uCloudColor;
uniform float uCloudHeight;
uniform float uCloudScale;
uniform float uCloudCoverage;
uniform float uCloudOpacity;
uniform float uCloudFade;
uniform vec2 uCloudOffset;

// Хэш без sin: у sin на больших аргументах (облака за километр от начала
// координат) точность на части видеокарт падает, и узор рассыпается в шум.
float Hash2(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float ValueNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(Hash2(i), Hash2(i + vec2(1.0, 0.0)), u.x),
               mix(Hash2(i + vec2(0.0, 1.0)), Hash2(i + vec2(1.0, 1.0)), u.x), u.y);
}

// Диск светила в касательной плоскости к направлению на него. Координаты —
// тангенсы углов, поэтому и круг, и квадрат, и картинка не зависят ни от поля
// зрения, ни от соотношения сторон кадра. Базис — «восток/верх» светила:
// квадрат и картинка не крутятся при повороте камеры.
vec2 DiscPlane(vec3 dir, vec3 toBody, out float facing) {
    vec3 right = abs(toBody.y) < 0.999 ? normalize(cross(vec3(0.0, 1.0, 0.0), toBody))
                                       : vec3(1.0, 0.0, 0.0);
    vec3 up = cross(toBody, right);
    facing = dot(dir, toBody);
    return facing > 1e-3 ? vec2(dot(dir, right), dot(dir, up)) / facing : vec2(1e3);
}

// Маска формы: 1 внутри, 0 снаружи, край сглажен на пиксель.
//
// Ширина сглаживания ограничена половиной диска НЕ для красоты. На большом
// круге, где светило оказывается «сбоку» (facing ~ 0), координаты касательной
// плоскости улетают в бесконечность, и fwidth между соседними пикселями там
// огромен: без ограничения край «сглаживался» на всё небо, и через кадр шла
// тонкая светлая линия — ровно по этому кругу.
float DiscMask(vec2 p, float size, int shape, float facing) {
    if (facing <= 0.0) return 0.0;
    float m = shape == 1 ? max(abs(p.x), abs(p.y)) : length(p);
    float aa = clamp(fwidth(m), 1e-5, size * 0.5);
    return 1.0 - smoothstep(size - aa, size + aa, m);
}

// Картинка светила: сложением, как у Minecraft (чёрный фон — прозрачен).
vec3 DiscTexture(sampler2D map, vec2 p, float size, float facing) {
    if (facing <= 0.0) return vec3(0.0);
    vec2 uv = p / (2.0 * size) + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec3(0.0);
    vec4 t = texture(map, vec2(uv.x, 1.0 - uv.y));
    return t.rgb * t.a;
}

void main() {
    vec3 dir = normalize(vDir);
    float y = dir.y - uHorizonOffset;
    float t = clamp(y, 0.0, 1.0);            // 0 у горизонта и ниже, 1 в зените

    // pow(t, 0.5) даёт нужный вид — почти всё небо цвета зенита, у горизонта
    // узкая светлая полоса, — но у корня БЕСКОНЕЧНАЯ производная в нуле. Ниже
    // горизонта при этом лежит плоская плита ровно одного цвета. Стык плиты с
    // бесконечно крутым подъёмом — разрыв по производной, и выглядит он как
    // жёсткая линия: на самом небе полосой у горизонта, а на отражающем шаре —
    // ступенькой поперёк, которая читается как дефект рендера, а не как небо.
    //
    // Окно smoothstep гасит подъём в первых ~15° над горизонтом: у него нулевая
    // производная в нуле, поэтому стык становится гладким. Выше окна множитель
    // равен единице, и картина остаётся ровно той, ради которой брали корень.
    // Нулевое окно — осознанно резкий горизонт (стилизованное небо).
    float soften = uHorizonSoft > 1e-4 ? smoothstep(0.0, uHorizonSoft, y) : 1.0;
    vec3 col = mix(uHorizon, uTop, pow(t, uGradExp) * soften);

    // Низ неба. Ниже горизонта — свой цвет (у Minecraft это тёмно-синяя
    // «пустота»), а не продолжение полосы горизонта до самого надира.
    if (uGround == 1) {
        float g = uGroundBlend > 1e-4 ? smoothstep(0.0, uGroundBlend, -y) : (y < 0.0 ? 1.0 : 0.0);
        col = mix(col, uGroundColor, g);
    }

    // --- Светила ------------------------------------------------------------
    //
    // Диск рисуется по УГЛУ между лучом и направлением на светило, а не
    // проекцией на плоскость экрана: угол не зависит ни от поля зрения, ни от
    // соотношения сторон, поэтому солнце остаётся круглым в любом окне и не
    // растягивается по краям кадра.
    if (uCelestials > 0.5) {
        // Солнце. Мягкий край — это не размытие ради красоты: диск в один
        // пиксель без сглаживания мерцает при движении камеры.
        float sunCos = dot(dir, uSunDir);
        // Диск и ореол гаснут вместе с солнцем: ушедшее за горизонт светило
        // не имеет права продолжать светить в кадре той же яркостью. Полностью
        // ноль не берём — у самого горизонта диск ещё виден, и это закат.
        float sunVisible = smoothstep(-0.12, 0.06, uSunDir.y);
        vec3 sunLight;
        if (uSunTex == 1 || uSunShape == 1) {
            float facing;
            vec2 p = DiscPlane(dir, uSunDir, facing);
            float size = tan(uSunSize);
            sunLight = uSunTex == 1 ? DiscTexture(uSunMap, p, size, facing)
                                    : vec3(DiscMask(p, size, uSunShape, facing));
        } else {
            float sunAng = acos(clamp(sunCos, -1.0, 1.0));
            sunLight = vec3(1.0 - smoothstep(uSunSize * 0.85, uSunSize, sunAng));
        }
        // Ореол вокруг диска: без него солнце выглядит наклейкой на небе.
        float sunGlow = (pow(max(0.0, sunCos), 220.0) * 0.6
                       + pow(max(0.0, sunCos), 12.0) * 0.10) * uSunGlow;
        col += uSunColor * (sunLight * uSunBrightness + sunGlow) * sunVisible;

        if (uMoon > 0.5) {
            vec3 moonDir = -uSunDir;
            float moonCos = dot(dir, moonDir);
            // ЯРКОСТЬ ЛУНЫ — ПО ВРЕМЕНИ СУТОК, А НЕ ПО ЗВЁЗДАМ. Здесь стоял
            // множитель uStars: выключил звёзды — пропала и луна, хотя это
            // разные вещи и настройка у них разная. Днём луна видна бледным
            // диском (так и в жизни), ночью светит в полную силу.
            float moonVisible = mix(1.0, 0.25, uDay);
            // Фаза: тень наползает сбоку. Без неё луна — просто белый кружок.
            vec3 east = normalize(cross(vec3(0.0, 1.0, 0.0), moonDir) + vec3(1e-5));
            float phase = uMoonPhase == 1
                ? smoothstep(-0.25, 0.35, dot(dir - moonDir, east) / max(uMoonSize, 1e-4))
                : 1.0;
            float moonGlow = pow(max(0.0, moonCos), 400.0) * 0.25 * uSunGlow;
            vec3 moonLight;
            if (uMoonTex == 1 || uMoonShape == 1) {
                float facing;
                vec2 p = DiscPlane(dir, moonDir, facing);
                float size = tan(uMoonSize);
                moonLight = uMoonTex == 1 ? DiscTexture(uMoonMap, p, size, facing) * 1.6
                                          : vec3(DiscMask(p, size, uMoonShape, facing)) * mix(0.15, 1.6, phase);
            } else {
                float moonAng = acos(clamp(moonCos, -1.0, 1.0));
                float moonDisk = 1.0 - smoothstep(uMoonSize * 0.8, uMoonSize, moonAng);
                moonLight = vec3(moonDisk * mix(0.15, 1.6, phase));
            }
            col += uMoonColor * (moonLight + moonGlow) * moonVisible;
        }

        // Звёзды — только там, где темно, и только ночью. Шум по направлению
        // луча: звёзды обязаны стоять на месте при повороте камеры, а не
        // мерцать вместе с ней, поэтому решётка берётся в МИРОВОМ направлении.
        if (uStars > 0.01 && dir.y > -0.05) {
            vec3 g = floor(dir * (260.0 / max(uStarSize, 0.05)));
            float h = fract(sin(dot(g, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
            float star = smoothstep(1.0 - 0.0035 * uStarDensity, 1.0, h);
            col += vec3(star) * uStars * smoothstep(-0.05, 0.25, dir.y) * 1.2;
        }
    }

    // --- Облака -------------------------------------------------------------
    //
    // Плоский слой на высоте uCloudHeight над камерой (минус — под ней): луч
    // пересекается с плоскостью, и узор берётся в МИРОВЫХ координатах точки
    // пересечения — облака стоят на месте, когда камера крутится, и уплывают
    // назад, когда она летит вперёд.
    if (uClouds == 1 && dir.y * uCloudHeight > 0.0) {
        float dist = uCloudHeight / dir.y;
        vec2 xz = uCamPos.xz + dir.xz * dist + uCloudOffset;
        vec2 q = xz / max(uCloudScale, 0.01);
        float cover;
        if (uCloudStyle == 0) {
            // Блоки: клетка целиком либо облако, либо нет. Крупный шум по
            // центрам клеток собирает их в острова, мелкий хэш рвёт края.
            vec2 cell = floor(q);
            float n = ValueNoise((cell + 0.5) * 0.13) * 0.8 + Hash2(cell) * 0.2;
            cover = step(1.0 - uCloudCoverage, n);
        } else {
            float n = ValueNoise(q * 0.125) * 0.5 + ValueNoise(q * 0.25) * 0.3
                    + ValueNoise(q * 0.5) * 0.2;
            cover = smoothstep(1.0 - uCloudCoverage, 1.0 - uCloudCoverage + 0.25, n);
        }
        // Вдали клетка меньше пикселя — узор там превращается в муар. Там,
        // где клеток на пиксель больше одной, берётся их средняя доля.
        float px = length(fwidth(q));
        cover = mix(cover, uCloudCoverage * 0.8, smoothstep(0.35, 1.2, px));
        float fade = exp(-abs(dist) / max(uCloudFade, 1.0));
        col = mix(col, uCloudColor, clamp(cover * uCloudOpacity * fade, 0.0, 1.0));
    }

    // Высотный туман — та же формула, что у геометрии (ApplyFog в
    // PbrShader.h), на луче длиной 3 км: вниз и вдоль горизонта небо тонет в
    // дымке, вверх — чистое.
    if (uHeightFog == 1) {
        const float len = 3000.0;
        float fall = uFogFalloff * dir.y * len;
        float shape = abs(fall) > 1e-4 ? (1.0 - exp(-fall)) / fall : 1.0;
        float density = uFogDensity * exp(-uFogFalloff * (uCamPos.y - uFogHeight));
        float amount = min(1.0 - exp(-density * len * shape), uFogMaxOpacity);
        float sun = pow(max(dot(dir, uSunDir), 0.0), uFogSunExponent);
        vec3 inscatter = uFogColor + uFogSunLight * uFogSunScatter * sun;
        col = mix(col, inscatter, clamp(amount, 0.0, 1.0));
    }

    FragColor = vec4(col, 1.0);
}
)GLSL";

} // namespace

SkyCelestials CelestialsFromEnvironment(const LightingEnvironment& env) {
    SkyCelestials c;
    // Одноцветное небо — это ЗАЛИВКА, и светил на ней быть не может: солнце,
    // луна и звёзды сразу превращают её обратно в небо, ради отсутствия
    // которого режим и выбирают.
    c.Enabled = env.Skybox.Celestials && env.Skybox.Kind != SkyboxSettings::Source::Solid;
    // Направление НА СОЛНЦЕ берётся из разрешённого состояния кадра, а не из
    // env.Sun: ночью в слоте солнца лежит ЛУНА (см. SkyModel.h), и диск солнца
    // уехал бы вслед за ней — то есть всходил бы на западе.
    if (env.SkyResolved && glm::length(env.SkySunDirection) > 1e-6f) {
        c.SunDir = glm::normalize(env.SkySunDirection);
    } else {
        // Состояние кадра не посчитано (окружение собрано кодом, без ApplySky).
        // Тогда — как раньше: у направленного света хранится, КУДА он светит, а
        // нарисовать надо там, ОТКУДА.
        const glm::vec3 d = env.Sun.Direction;
        c.SunDir = glm::length(d) > 1e-6f ? -glm::normalize(d) : glm::vec3(0.0f, 1.0f, 0.0f);
    }
    c.DayFactor = env.DayFactor;
    c.SunColor = env.Skybox.SunColor;
    c.SunSize = env.Skybox.SunSize;
    c.Moon = env.Skybox.Moon;
    c.MoonColor = env.Skybox.MoonColor;
    c.MoonSize = env.Skybox.MoonSize;
    c.StarIntensity = env.Skybox.StarIntensity;
    c.HeightFog = env.Fog.Enabled && env.Fog.Kind == FogSettings::Mode::ExponentialHeight;
    c.FogColor = env.Fog.Color;
    c.FogDensity = env.Fog.Density;
    c.FogFalloff = env.Fog.HeightFalloff;
    c.FogHeight = env.Fog.BaseHeight;
    c.FogMaxOpacity = env.Fog.MaxOpacity;
    c.FogSunScatter = env.Fog.SunScatter;
    c.FogSunExponent = env.Fog.SunExponent;
    c.FogSunLight = env.Sun.Color * env.Sun.Intensity;

    const SkyboxSettings& sky = env.Skybox;
    // Одноцветное небо — заливка: ни формы, ни низа, ни облаков.
    if (sky.Kind == SkyboxSettings::Source::Solid) return c;
    c.GradientExponent = sky.GradientExponent;
    c.HorizonSoftness = sky.HorizonSoftness;
    c.HorizonOffset = sky.HorizonOffset;
    c.Ground = sky.Ground;
    // Низ и облака темнеют по тому же времени суток, что и остальное небо.
    // Без смены суток DayFactor равен единице — дневные цвета.
    c.GroundColor = glm::mix(sky.NightGroundColor, sky.GroundColor, env.DayFactor);
    c.GroundBlend = sky.GroundBlend;
    c.SunShape = (int)sky.SunShape;
    c.MoonShape = (int)sky.MoonShape;
    c.SunBrightness = sky.SunBrightness;
    c.SunGlow = sky.SunGlow;
    c.MoonPhase = sky.MoonPhase;
    c.SunTexture = sky.SunTexture;
    c.MoonTexture = sky.MoonTexture;
    c.PixelArt = sky.PixelArt;
    c.StarDensity = sky.StarDensity;
    c.StarSize = sky.StarSize;
    c.Clouds = sky.Clouds;
    c.CloudStyle = (int)sky.CloudKind;
    c.CloudColor = glm::mix(sky.NightCloudColor, sky.CloudColor, env.DayFactor);
    c.CloudHeight = sky.CloudHeight;
    c.CloudScale = sky.CloudScale;
    c.CloudCoverage = sky.CloudCoverage;
    c.CloudOpacity = sky.CloudOpacity;
    c.CloudWind = sky.CloudWind;
    c.CloudFade = sky.CloudFade;
    return c;
}

SkyRenderer::SkyRenderer() {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    m_shader = device.CreateShaderProgram(kVertexSrc, kFragmentSrc);
    // Геометрия без атрибутов — вершины считаются из gl_VertexID (см. VS).
    m_geometry = device.CreateGeometry(sage::rhi::VertexLayout{});
}

void SkyRenderer::Draw(const glm::mat4& view, const glm::mat4& proj,
                       glm::vec3 topColor, glm::vec3 horizonColor, const SkyCelestials& sky) {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();

    m_shader->Use();
    m_shader->SetMat4("uInvProj", glm::inverse(proj));
    // view — ортонормальная (R*T): view->world поворот = transpose(mat3(view)),
    // упаковываем в mat4 (у ShaderProgram нет SetMat3; VS берёт mat3(...)).
    m_shader->SetMat4("uInvViewRot", glm::mat4(glm::transpose(glm::mat3(view))));
    m_shader->SetVec3("uTop", topColor);
    m_shader->SetVec3("uHorizon", horizonColor);
    m_shader->SetFloat("uCelestials", sky.Enabled ? 1.0f : 0.0f);
    const glm::vec3 sunDir = glm::length(sky.SunDir) > 1e-6f ? glm::normalize(sky.SunDir)
                                                             : glm::vec3(0.0f, 1.0f, 0.0f);
    m_shader->SetVec3("uSunDir", sunDir);
    m_shader->SetVec3("uSunColor", sky.SunColor);
    m_shader->SetFloat("uSunSize", sky.SunSize);
    m_shader->SetFloat("uMoon", sky.Moon ? 1.0f : 0.0f);
    m_shader->SetVec3("uMoonColor", sky.MoonColor);
    m_shader->SetFloat("uMoonSize", sky.MoonSize);
    // Звёзды проступают по мере ухода солнца. Множитель приходит СНАРУЖИ, из
    // общей модели времени суток (sage/render/SkyModel.h): раньше небо считало
    // «сколько сейчас ночи» само, отдельной формулой, и его представление о
    // времени расходилось с тем, по которому темнело освещение сцены. Одно
    // время суток на кадр — одно место, где оно считается.
    m_shader->SetFloat("uDay", sky.DayFactor);
    m_shader->SetFloat("uStars", (1.0f - sky.DayFactor) * sky.StarIntensity);
    m_shader->SetInt("uHeightFog", sky.HeightFog ? 1 : 0);
    m_shader->SetVec3("uFogColor", sky.FogColor);
    m_shader->SetFloat("uFogDensity", std::max(sky.FogDensity, 0.0f));
    m_shader->SetFloat("uFogFalloff", std::max(sky.FogFalloff, 0.0f));
    m_shader->SetFloat("uFogHeight", sky.FogHeight);
    m_shader->SetFloat("uFogMaxOpacity", std::clamp(sky.FogMaxOpacity, 0.0f, 1.0f));
    m_shader->SetFloat("uFogSunScatter", std::max(sky.FogSunScatter, 0.0f));
    m_shader->SetFloat("uFogSunExponent", std::max(sky.FogSunExponent, 1.0f));
    m_shader->SetVec3("uFogSunLight", sky.FogSunLight);
    // Положение камеры — из обратной матрицы вида: плотность тумана зависит
    // от высоты зрителя, а облака стоят в мировых координатах.
    m_shader->SetVec3("uCamPos", glm::vec3(glm::inverse(view)[3]));

    m_shader->SetFloat("uGradExp", std::clamp(sky.GradientExponent, 0.05f, 16.0f));
    m_shader->SetFloat("uHorizonSoft", std::max(sky.HorizonSoftness, 0.0f));
    m_shader->SetFloat("uHorizonOffset", std::clamp(sky.HorizonOffset, -0.9f, 0.9f));
    m_shader->SetInt("uGround", sky.Ground ? 1 : 0);
    m_shader->SetVec3("uGroundColor", sky.GroundColor);
    m_shader->SetFloat("uGroundBlend", std::max(sky.GroundBlend, 0.0f));
    m_shader->SetInt("uSunShape", sky.SunShape);
    m_shader->SetInt("uMoonShape", sky.MoonShape);
    m_shader->SetFloat("uSunBrightness", std::max(sky.SunBrightness, 0.0f));
    m_shader->SetFloat("uSunGlow", std::max(sky.SunGlow, 0.0f));
    m_shader->SetInt("uMoonPhase", sky.MoonPhase ? 1 : 0);
    m_shader->SetFloat("uStarDensity", std::clamp(sky.StarDensity, 0.0f, 50.0f));
    m_shader->SetFloat("uStarSize", std::clamp(sky.StarSize, 0.05f, 20.0f));
    m_shader->SetInt("uClouds", sky.Clouds ? 1 : 0);
    m_shader->SetInt("uCloudStyle", sky.CloudStyle);
    m_shader->SetVec3("uCloudColor", sky.CloudColor);
    m_shader->SetFloat("uCloudHeight", sky.CloudHeight);
    m_shader->SetFloat("uCloudScale", std::max(sky.CloudScale, 0.01f));
    m_shader->SetFloat("uCloudCoverage", std::clamp(sky.CloudCoverage, 0.0f, 1.0f));
    m_shader->SetFloat("uCloudOpacity", std::clamp(sky.CloudOpacity, 0.0f, 1.0f));
    m_shader->SetFloat("uCloudFade", std::max(sky.CloudFade, 1.0f));
    // Узор сдвигается ПРОТИВ ветра: облако, стоявшее над точкой x, через
    // секунду стоит над x + ветер.
    m_shader->SetVec2("uCloudOffset", -sky.CloudWind * sky.Time);

    // Картинки светил. Юниты 0 и 1 — у неба своих текстур больше нет.
    // Загрузка — через общий кэш: путь проверяется раз, дальше это поиск.
    auto bindDisc = [&](const std::string& path, int unit, const char* flag, const char* sampler) {
        std::shared_ptr<Texture> tex;
        if (!path.empty()) {
            tex = ResourceManager::Instance().GetTexture(
                path, sky.PixelArt ? TextureFilter::Nearest : TextureFilter::Bilinear,
                /*mipmaps*/ false, /*bleed*/ false, /*srgb*/ true);
        }
        m_shader->SetInt(sampler, unit);
        m_shader->SetInt(flag, tex ? 1 : 0);
        if (tex) tex->Bind((unsigned)unit);
    };
    bindDisc(sky.SunTexture, 0, "uSunTex", "uSunMap");
    bindDisc(sky.MoonTexture, 1, "uMoonTex", "uMoonMap");

    // Небо — фон: без теста и записи глубины, без отсечения. Рисуется первым,
    // сцена ложится поверх по своей глубине.
    device.SetDepthTest(false);
    device.SetDepthWrite(false);
    device.SetCullMode(sage::rhi::CullMode::Off);

    m_geometry->DrawArrays(3);

    device.SetCullMode(sage::rhi::CullMode::Back);
    device.SetDepthWrite(true);
    device.SetDepthTest(true);
}
