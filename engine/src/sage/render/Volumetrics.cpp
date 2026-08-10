#include "sage/render/Volumetrics.h"

#include <algorithm>
#include <cmath>

#include "sage/core/Config.h"
#include "sage/core/Profiler.h"
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

// --- Марш по лучу: рассеяние в воздухе + облака -------------------------------
//
// Результат ПРЕДУМНОЖЕН на альфу: rgb — уже пришедший к глазу свет, a — сколько
// неба закрыто облаком. Так композит делается одной формулой смешивания и без
// второго прохода: лучи складываются, облака закрывают.
const char* kMarchFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uDepth;
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
uniform vec3 uSunDir;        // направление НА солнце
uniform vec3 uSunColor;
uniform vec3 uSkyTop;
uniform vec3 uSkyHorizon;
uniform float uTime;
uniform vec2 uJitter;        // размер пикселя уменьшенного буфера

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

uniform int   uShadowsOn;
uniform int   uCascades;
uniform mat4  uLightMat[4];
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

float sampleCascade(int i, vec3 world) {
    vec4 lp = uLightMat[i] * vec4(world, 1.0);
    vec3 uv = lp.xyz / max(lp.w, 1e-6) * 0.5 + 0.5;
    if (uv.x < 0.02 || uv.x > 0.98 || uv.y < 0.02 || uv.y > 0.98 || uv.z > 1.0) return -1.0;
    float d;
    if (i == 0) d = texture(uShadow0, uv.xy).r;
    else if (i == 1) d = texture(uShadow1, uv.xy).r;
    else if (i == 2) d = texture(uShadow2, uv.xy).r;
    else d = texture(uShadow3, uv.xy).r;
    // Запас по глубине больше, чем у поверхностей: точка в воздухе не лежит на
    // геометрии, и жёсткий порог давал бы в лучах полосы.
    return (uv.z - 0.0025 > d) ? 0.0 : 1.0;
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
    float gate = smoothstep(0.38, 0.66, fbm(q * 0.3));
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

// Сколько света доходит до точки внутри облака. Пять шагов к солнцу — этого
// хватает на объём: важен не точный интеграл, а то, что низ темнее верха.
float cloudLight(vec3 p) {
    float t = 0.0, dens = 0.0;
    float step = (uCloudTop - uCloudBottom) / float(max(uCloudLightSteps, 1)) * 0.6;
    for (int i = 0; i < 8; ++i) {
        if (i >= uCloudLightSteps) break;
        t += step;
        dens += cloudAt(p + uSunDir * t) * step;
    }
    return exp(-dens * 0.9);
}

void main() {
    vec2 uv = vUV;
    float depth = texture(uDepth, uv).r;

    // Мировой луч через пиксель.
    vec4 far = uInvViewProj * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    vec3 farPos = far.xyz / far.w;
    vec3 dir = normalize(farPos - uCamPos);

    // Расстояние до геометрии: при depth == 1 луч ушёл в небо.
    float sceneDist = uMaxDist;
    bool sky = depth >= 0.9999;
    if (!sky) {
        vec4 wp = uInvViewProj * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
        sceneDist = length(wp.xyz / wp.w - uCamPos);
    }

    // Упорядоченный шум по пикселю: сдвигает начало марша, и «ступени» от
    // малого числа шагов рассыпаются вместо того, чтобы стоять кольцами.
    vec2 px = uv / max(uJitter, vec2(1e-6));
    float dither = fract(52.9829189 * fract(0.06711056 * px.x + 0.00583715 * px.y));

    vec3 scatter = vec3(0.0);

    // --- Лучи в воздухе ---
    if (uShafts != 0) {
        float march = min(sceneDist, uMaxDist);
        float dt = march / float(max(uSteps, 1));
        float t = dt * dither;
        float transmittance = 1.0;
        float ph = phaseHG(dot(dir, uSunDir), uAniso);
        for (int i = 0; i < 64; ++i) {
            if (i >= uSteps || t >= march) break;
            vec3 p = uCamPos + dir * t;
            // Плотность падает с высотой: у воды дымка густая, наверху её нет.
            float d = uDensity * exp(-max(p.y - uBaseHeight, 0.0) * uHeightFalloff);
            if (d > 1e-5) {
                float vis = sunVisibility(p);
                scatter += uSunColor * (vis * ph * d * dt * transmittance);
                transmittance *= exp(-d * dt);
                if (transmittance < 0.02) break;   // дальше вклада уже не видно
            }
            t += dt;
        }
        // Фаза уже нормирована (интеграл по сфере = 1) — домножать на 4π
        // нельзя, иначе взгляд в сторону солнца даёт вспышку в разы ярче
        // самого солнца и весь кадр уходит в белое.
        scatter *= uIntensity;
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
            for (int i = 0; i < 64; ++i) {
                if (i >= uCloudSteps || trans < 0.02) break;
                vec3 p = uCamPos + dir * t;
                float d = cloudAt(p);
                if (d > 0.001) {
                    float lit = cloudLight(p);
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

    FragColor = vec4(cloudColor + scatter, clamp(cloudAlpha, 0.0, 1.0));
}
)";

// --- Подъём результата в полный размер ----------------------------------------
//
// Обычная билинейная фильтрация протекла бы через силуэты: за краем мачты
// лежит небо, и половина выборок пришла бы оттуда — вокруг тонких предметов
// появилась бы светящаяся кайма. Поэтому выборки взвешиваются по близости
// глубины: чужая глубина — почти нулевой вес.
const char* kUpsampleFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uVolume;
uniform sampler2D uDepthLow;
uniform sampler2D uDepthFull;
uniform vec2 uLowTexel;

void main() {
    float dFull = texture(uDepthFull, vUV).r;
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 o = vec2(float(x), float(y)) * uLowTexel;
            float dLow = texture(uDepthLow, vUV + o).r;
            float w = 1.0 / (1.0 + abs(dLow - dFull) * 900.0);
            sum += texture(uVolume, vUV + o) * w;
            wsum += w;
        }
    }
    FragColor = sum / max(wsum, 1e-4);
}
)";

Shader& MarchShader() {
    static Shader* s = new Shader(Shader::FromSource(kVert, kMarchFrag, "Volumetrics.March"));
    return *s;
}
Shader& UpsampleShader() {
    static Shader* s = new Shader(Shader::FromSource(kVert, kUpsampleFrag, "Volumetrics.Upsample"));
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
    return v;
}

void Volumetrics::EnsureTargets(int w, int h) {
    if (w == m_w && h == m_h && m_march) return;
    m_w = w;
    m_h = h;
    m_march = MakeColor(w, h);
}

void Volumetrics::Render(Framebuffer& target, sage::rhi::TextureHandle sceneDepth, int w, int h,
                         const glm::mat4& proj, const glm::mat4& view, const glm::vec3& camPos,
                         const LightingEnvironment& env, const ShadowBinding& shadows,
                         const VolumetricSettings& s, float time) {
    if (!s.Enabled || (!s.LightShafts && !s.Clouds)) return;
    if (!sceneDepth.Valid() || w < 8 || h < 8) return;
    SAGE_PROFILE("Объёмный свет");

    GraphicsDevice& device = GraphicsDevice::Get();
    if (!m_fsTri) m_fsTri = device.CreateGeometry(VertexLayout{});

    const float scale = glm::clamp(s.Scale, 0.25f, 1.0f);
    const int lw = std::max(8, (int)((float)w * scale));
    const int lh = std::max(8, (int)((float)h * scale));
    EnsureTargets(lw, lh);

    const glm::mat4 invViewProj = glm::inverse(proj * view);
    const glm::vec3 sunDir = glm::normalize(-env.Sun.Direction);   // НА солнце

    // --- Марш в уменьшенном буфере ---
    m_march->Bind();
    device.SetDepthTest(false);
    device.SetBlend(false);
    device.SetClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    device.Clear();

    Shader& march = MarchShader();
    march.Use();
    march.SetInt("uDepth", 0);
    device.BindTexture2D(0, sceneDepth);
    march.SetMat4("uInvViewProj", invViewProj);
    march.SetVec3("uCamPos", camPos);
    march.SetVec3("uSunDir", sunDir);
    march.SetVec3("uSunColor", glm::vec3(env.Sun.Color) * env.Sun.Intensity);
    march.SetVec3("uSkyTop", glm::vec3(env.SkyColor));
    march.SetVec3("uSkyHorizon", glm::vec3(env.Fog.Color));
    march.SetFloat("uTime", time);
    march.SetVec2("uJitter", glm::vec2(1.0f / (float)lw, 1.0f / (float)lh));

    march.SetInt("uShafts", s.LightShafts ? 1 : 0);
    march.SetFloat("uDensity", s.Density);
    march.SetFloat("uAniso", glm::clamp(s.Anisotropy, -0.95f, 0.95f));
    march.SetFloat("uMaxDist", s.MaxDistance);
    march.SetFloat("uIntensity", s.Intensity);
    march.SetFloat("uHeightFalloff", s.HeightFalloff);
    march.SetFloat("uBaseHeight", s.BaseHeight);
    march.SetInt("uSteps", glm::clamp(s.Steps, 4, 64));

    march.SetInt("uClouds", s.Clouds ? 1 : 0);
    march.SetFloat("uCloudBottom", s.CloudBottom);
    march.SetFloat("uCloudTop", std::max(s.CloudTop, s.CloudBottom + 10.0f));
    march.SetFloat("uCoverage", glm::clamp(s.Coverage, 0.0f, 1.0f));
    march.SetFloat("uCloudDensity", s.CloudDensity);
    march.SetFloat("uCloudScale", s.CloudScale);
    march.SetVec2("uWind", s.Wind);
    march.SetVec3("uTint", s.Tint);
    march.SetInt("uCloudSteps", glm::clamp(s.CloudSteps, 8, 64));
    march.SetInt("uCloudLightSteps", glm::clamp(s.CloudLightSteps, 1, 8));

    const bool useShadows = shadows.Enabled && s.LightShafts;
    march.SetInt("uShadowsOn", useShadows ? 1 : 0);
    march.SetInt("uCascades", useShadows ? std::min(shadows.Count, 4) : 0);
    for (int i = 0; i < 4; ++i) {
        const std::string idx = "uLightMat[" + std::to_string(i) + "]";
        march.SetMat4(idx.c_str(), shadows.Matrices[std::min(i, ShadowMap::kMaxCascades - 1)]);
        const std::string name = "uShadow" + std::to_string(i);
        march.SetInt(name.c_str(), 1 + i);
        if (useShadows && i < shadows.Count)
            device.BindTexture2D(1 + i, shadows.Textures[i]);
        else
            device.BindTexture2D(1 + i, sceneDepth);   // юнит обязан быть привязан
    }
    m_fsTri->DrawArrays(3);

    // --- Композит в буфер сцены ---
    //
    // Смешивание для ПРЕДУМНОЖЕННОЙ альфы: цвет складывается, а закрытое
    // облаком небо гасится ровно на его непрозрачность. Одна формула на оба
    // эффекта — второй проход не нужен.
    target.Bind();
    device.SetBlend(true);
    device.SetBlendMode(GraphicsDevice::BlendMode::Premultiplied);

    Shader& up = UpsampleShader();
    up.Use();
    up.SetInt("uVolume", 0);
    up.SetInt("uDepthLow", 1);
    up.SetInt("uDepthFull", 2);
    device.BindTexture2D(0, m_march->ColorTextureHandle());
    // Глубина у прохода одна и та же (марш читает полноразмерную): «низкая»
    // выборка отличается только смещением на тексель уменьшенного буфера, и
    // этого достаточно, чтобы вес упал на чужом силуэте.
    device.BindTexture2D(1, sceneDepth);
    device.BindTexture2D(2, sceneDepth);
    up.SetVec2("uLowTexel", glm::vec2(1.0f / (float)lw, 1.0f / (float)lh));
    m_fsTri->DrawArrays(3);

    device.SetBlendMode(GraphicsDevice::BlendMode::Alpha);
    device.SetBlend(false);
    device.SetDepthTest(true);
}

} // namespace sage::render
