#include "sage/render/LensFlare.h"

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

// --- Видимость солнца ---------------------------------------------------------
//
// Два вопроса на каждую выборку, и оба обязательны.
//
// ГЛУБИНА отвечает, не закрыто ли солнце геометрией: мачта, парус, борт. Одной
// её мало — облака в буфер глубины не пишутся вовсе, и над затянутым небом
// глубина честно скажет «там даль», а солнца там нет.
//
// ЦВЕТ отвечает, светит ли оттуда что-нибудь. Одного цвета тоже мало: белая
// стена в упор ярче неба, и по цвету солнце «видно» сквозь неё.
//
// Вместе они дают то, что нужно: небо И ярко.
const char* kVisibleFrag = R"(#version 330 core
out vec4 FragColor;

uniform sampler2D uDepth;
uniform sampler2D uColor;
uniform vec2 uSunUV;
uniform float uRadius;     // радиус опроса в долях кадра
uniform float uAspect;
uniform float uThreshold;

const int kTaps = 25;

void main() {
    // Выборки по спирали Вогеля: равномернее концентрических колец и не даёт
    // регулярного узора, из-за которого край мачты то «виден», то нет.
    float sum = 0.0;
    for (int i = 0; i < kTaps; ++i) {
        float f = (float(i) + 0.5) / float(kTaps);
        float r = sqrt(f) * uRadius;
        float a = float(i) * 2.39996323;   // золотой угол
        vec2 uv = uSunUV + vec2(cos(a) * r / uAspect, sin(a) * r);
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) continue;

        float sky = step(0.99990, texture(uDepth, uv).r);
        vec3 c = texture(uColor, uv).rgb;
        float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
        float bright = smoothstep(uThreshold * 0.35, uThreshold, lum);
        sum += sky * bright;
    }
    FragColor = vec4(sum / float(kTaps), 0.0, 0.0, 1.0);
}
)";

// --- Сам блик -----------------------------------------------------------------
const char* kFlareFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uVisible;   // 1x1: доля видимого солнца

uniform vec2  uSunUV;
uniform vec3  uSunColor;
uniform float uAspect;
uniform float uIntensity;
uniform float uEdgeFade;      // 0 у края экрана, 1 в середине

uniform int   uGhosts;
uniform float uGhostSpacing;
uniform float uGhostSize;
uniform int   uBlades;

uniform float uHalo;
uniform float uHaloRadius;
uniform float uHaloWidth;

uniform float uStarburst;
uniform int   uSpikes;
uniform float uGlare;
uniform float uGlareSize;
uniform float uChroma;
uniform float uRoll;          // крен камеры: звезда обязана крутиться вместе с ней

// Изображение диафрагмы: правильный многоугольник со скруглённым краем. При
// uBlades <= 2 вырождается в круг — не у всякой камеры лепестки видны.
float aperture(vec2 p, float r) {
    float d;
    if (uBlades <= 2) {
        d = length(p) / r;
    } else {
        float seg = 6.28318530718 / float(uBlades);
        float a = atan(p.y, p.x);
        d = cos(floor(0.5 + a / seg) * seg - a) * length(p) / r;
    }
    // Край мягкий и с подсветкой по контуру: у настоящего призрака середина
    // темнее ободка, потому что это изображение ОТВЕРСТИЯ, а не диска света.
    float body = 1.0 - smoothstep(0.62, 1.0, d);
    float rim = exp(-pow((d - 0.86) / 0.16, 2.0)) * 0.85;
    return body * 0.55 + rim;
}

// Простой разброс по номеру призрака: размеры и оттенки не должны идти
// ровной лесенкой, иначе цепочка читается как узор, а не как отражения.
float hash(float n) { return fract(sin(n * 43758.5453) * 12345.6789); }

void main() {
    float visible = texture(uVisible, vec2(0.5)).r;
    if (visible <= 0.001) discard;

    // Работаем в координатах, где круг остаётся кругом.
    vec2 uv = vec2(vUV.x * uAspect, vUV.y);
    vec2 sun = vec2(uSunUV.x * uAspect, uSunUV.y);
    vec2 center = vec2(0.5 * uAspect, 0.5);

    vec3 color = vec3(0.0);
    vec2 axis = center - sun;         // прямая, по которой ложатся призраки
    vec2 toSun = uv - sun;
    float distSun = length(toSun);

    float size = max(uGlareSize, 1e-3);

    // --- Свечение вокруг источника ---
    if (uGlare > 0.0) {
        float core = exp(-distSun / size);
        float wide = exp(-distSun / (size * 3.0)) * 0.18;
        color += uSunColor * (core + wide) * uGlare;
    }

    // --- Лучи-звезда ---
    //
    // Лучи КОРОТКИЕ. Длинные, дотягивающиеся до края кадра, заливают собой всё
    // остальное — и призраки, и ореол, ради которых блик и делается: на экране
    // остаётся белое пятно со спицами. У настоящей звезды свет спадает быстро.
    if (uStarburst > 0.0) {
        float ang = atan(toSun.y, toSun.x) + uRoll;
        float spikes = pow(abs(cos(ang * float(uSpikes) * 0.5)), 14.0);
        // Тонкий второй набор лучей вполоборота: у настоящей звезды лучи
        // разной длины, и одинаковые сразу выдают формулу.
        spikes += pow(abs(cos(ang * float(uSpikes) * 0.5 + 0.7854)), 30.0) * 0.45;
        float falloff = exp(-distSun / (size * 2.2));
        color += uSunColor * spikes * falloff * uStarburst;
    }

    // --- Ореол ---
    if (uHalo > 0.0) {
        // Кольцо строится вокруг ЦЕНТРА кадра по расстоянию от него — так оно
        // и ведёт себя на снимках: не привязано к солнцу, а стоит там, куда
        // его ставит оптика.
        vec2 q = uv - center;
        float len = length(q);
        vec3 ring;
        for (int c = 0; c < 3; ++c) {
            float shift = (float(c) - 1.0) * uChroma * uHaloWidth * 0.6;
            float d = abs(len - (uHaloRadius + shift)) / max(uHaloWidth, 1e-3);
            ring[c] = exp(-d * d);
        }
        // Ореол виден тем сильнее, чем ближе солнце к центру кадра.
        float k = 1.0 - clamp(length(axis) / (0.5 * uAspect), 0.0, 1.0);
        color += uSunColor * ring * uHalo * (0.35 + 0.65 * k);
    }

    // --- Призраки ---
    for (int i = 1; i <= 12; ++i) {
        if (i > uGhosts) break;
        float fi = float(i);
        vec2 pos = sun + axis * (uGhostSpacing * fi);
        float size = uGhostSize * (0.45 + 1.25 * hash(fi * 3.1));
        float amp = (0.55 + 0.45 * hash(fi * 7.7)) / (1.0 + fi * 0.35);

        vec2 p = uv - pos;
        vec3 g;
        for (int c = 0; c < 3; ++c) {
            float k = 1.0 + (float(c) - 1.0) * uChroma * 0.09;
            g[c] = aperture(p, size * k);
        }
        // Оттенок призрака: тёплые ближе к солнцу, холодные дальше — так
        // ложится дисперсия в реальном объективе.
        vec3 tint = mix(vec3(1.0, 0.82, 0.55), vec3(0.55, 0.78, 1.0), hash(fi * 11.3));
        color += uSunColor * tint * g * amp;
    }

    color *= visible * uIntensity * uEdgeFade;
    FragColor = vec4(color, 1.0);
}
)";

Shader& VisibleShader() {
    static Shader* s = new Shader(Shader::FromSource(kVert, kVisibleFrag, "LensFlare.Visible"));
    return *s;
}
Shader& FlareShader() {
    static Shader* s = new Shader(Shader::FromSource(kVert, kFlareFrag, "LensFlare.Draw"));
    return *s;
}

} // namespace

LensFlareSettings LensFlareFromConfig(const sage::EngineConfig& cfg) {
    LensFlareSettings f;
    f.Enabled = cfg.LensFlare;
    f.Intensity = cfg.LensFlareIntensity;
    f.Ghosts = cfg.LensFlareGhosts;
    f.GhostSpacing = cfg.LensFlareGhostSpacing;
    f.GhostSize = cfg.LensFlareGhostSize;
    f.ApertureBlades = cfg.LensFlareBlades;
    f.Halo = cfg.LensFlareHalo;
    f.HaloRadius = cfg.LensFlareHaloRadius;
    f.Starburst = cfg.LensFlareStarburst;
    f.Glare = cfg.LensFlareGlare;
    f.Chroma = cfg.LensFlareChroma;
    f.Threshold = cfg.LensFlareThreshold;
    return f;
}

void LensFlare::Render(Framebuffer& target, sage::rhi::TextureHandle sceneColor,
                       sage::rhi::TextureHandle sceneDepth, int w, int h, const glm::mat4& proj,
                       const glm::mat4& view, const LightingEnvironment& env,
                       const LensFlareSettings& s) {
    if (!s.Enabled || s.Intensity <= 0.0f) return;
    if (!sceneColor.Valid() || !sceneDepth.Valid() || w < 8 || h < 8) return;

    // Солнце ниже горизонта не даёт блика — и не потому, что «так решили»:
    // источника в кадре просто нет, а блик без источника читается как грязь на
    // объективе.
    const glm::vec3 sunDir = glm::normalize(-glm::vec3(env.Sun.Direction));
    if (sunDir.y <= -0.02f) return;

    // Куда солнце проецируется на экране. Берём точку далеко по направлению на
    // солнце: у направленного света своего положения нет, а проекция нужна
    // именно точки.
    const glm::vec3 eye = glm::vec3(glm::inverse(view)[3]);
    const glm::vec4 clip = proj * view * glm::vec4(eye + sunDir * 10000.0f, 1.0f);
    if (clip.w <= 0.0f) return;   // за спиной
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    const glm::vec2 sunUV = ndc * 0.5f + 0.5f;

    // Затухание к краю кадра. Обрывать блик ровно на границе экрана нельзя:
    // солнце уходит за край, и вся цепочка призраков гаснет одним кадром.
    const float over = std::max(std::abs(ndc.x), std::abs(ndc.y));
    const float edgeFade = 1.0f - glm::smoothstep(1.0f, 1.6f, over);
    if (edgeFade <= 0.0f) return;

    SAGE_PROFILE("Блик в объективе");

    GraphicsDevice& device = GraphicsDevice::Get();
    if (!m_fsTri) m_fsTri = device.CreateGeometry(VertexLayout{});
    if (!m_visible) {
        RenderTargetDesc d;
        d.Width = 1;
        d.Height = 1;
        d.Kind = RenderTargetKind::ColorHDR;
        m_visible = device.CreateRenderTarget(d);
    }
    if (!m_visible) return;

    const float aspect = (float)w / (float)std::max(h, 1);

    // --- Проход 1: сколько солнца видно ---
    m_visible->Bind();
    device.SetViewport(0, 0, 1, 1);
    device.SetDepthTest(false);
    device.SetBlend(false);

    Shader& vis = VisibleShader();
    vis.Use();
    vis.SetInt("uDepth", 0);
    vis.SetInt("uColor", 1);
    device.BindTexture2D(0, sceneDepth);
    device.BindTexture2D(1, sceneColor);
    vis.SetVec2("uSunUV", sunUV);
    vis.SetFloat("uRadius", 0.035f);
    vis.SetFloat("uAspect", aspect);
    vis.SetFloat("uThreshold", std::max(s.Threshold, 1e-3f));
    m_fsTri->DrawArrays(3);

    // --- Проход 2: блик поверх кадра ---
    target.Bind();
    device.SetViewport(0, 0, w, h);
    device.SetBlend(true);
    device.SetBlendMode(GraphicsDevice::BlendMode::Additive);

    // Крен камеры: третья строка матрицы вида — это «вправо» камеры в мире.
    // Звезда, не поворачивающаяся вместе с камерой, сразу выглядит наклейкой.
    const float roll = std::atan2(view[1][0], view[1][1]);

    Shader& flare = FlareShader();
    flare.Use();
    flare.SetInt("uVisible", 0);
    device.BindTexture2D(0, m_visible->ColorTextureHandle());
    flare.SetVec2("uSunUV", sunUV);
    flare.SetVec3("uSunColor", glm::vec3(env.Sun.Color));
    flare.SetFloat("uAspect", aspect);
    flare.SetFloat("uIntensity", s.Intensity);
    flare.SetFloat("uEdgeFade", edgeFade);
    flare.SetInt("uGhosts", glm::clamp(s.Ghosts, 0, 12));
    flare.SetFloat("uGhostSpacing", s.GhostSpacing);
    flare.SetFloat("uGhostSize", s.GhostSize);
    flare.SetInt("uBlades", glm::clamp(s.ApertureBlades, 0, 12));
    flare.SetFloat("uHalo", s.Halo);
    flare.SetFloat("uHaloRadius", s.HaloRadius);
    flare.SetFloat("uHaloWidth", std::max(s.HaloWidth, 1e-3f));
    flare.SetFloat("uStarburst", s.Starburst);
    flare.SetInt("uSpikes", glm::clamp(s.StarburstSpikes, 2, 32));
    flare.SetFloat("uGlare", s.Glare);
    flare.SetFloat("uGlareSize", std::max(s.GlareSize, 1e-3f));
    flare.SetFloat("uChroma", s.Chroma);
    flare.SetFloat("uRoll", roll);
    m_fsTri->DrawArrays(3);

    device.SetBlendMode(GraphicsDevice::BlendMode::Alpha);
    device.SetBlend(false);
    device.SetDepthTest(true);
}

} // namespace sage::render
