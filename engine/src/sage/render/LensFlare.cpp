#include "sage/render/LensFlare.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

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

// --- Блики ламп: видимость --------------------------------------------------
//
// Тексель i — лампа i. Лампа видна, если по глубине кадра в её точке нет
// ничего БЛИЖЕ неё (с допуском: колба вокруг самой лампы не должна её гасить).
// Несколько выборок вокруг — край предмета даёт плавное, а не мигающее
// исчезновение.
const char* kLightVisibleFrag = R"(#version 330 core
out vec4 FragColor;
uniform sampler2D uDepth;
uniform vec3 uLights[16];   // (u, v, расстояние до камеры вдоль взгляда, м)
uniform int uCount;
uniform float uP22;
uniform float uP32;
uniform int uOrtho;
uniform float uTolerance;
uniform float uAspect;
float ViewDepth(float d) {
    float ndc = d * 2.0 - 1.0;
    return uOrtho == 1 ? -(ndc - uP32) / uP22 : uP32 / (ndc + uP22);
}
void main() {
    int i = int(gl_FragCoord.x);
    if (i >= uCount) { FragColor = vec4(0.0); return; }
    vec3 L = uLights[i];
    float sum = 0.0;
    const int kTaps = 9;
    for (int t = 0; t < kTaps; ++t) {
        float f = (float(t) + 0.5) / float(kTaps);
        float r = sqrt(f) * 0.006;
        float a = float(t) * 2.39996323;
        vec2 uv = L.xy + vec2(cos(a) * r / uAspect, sin(a) * r);
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) continue;
        float d = texture(uDepth, uv).r;
        if (d >= 0.99999) { sum += 1.0; continue; }   // небо за лампой
        sum += step(L.z - uTolerance, ViewDepth(d));
    }
    FragColor = vec4(sum / float(kTaps), 0.0, 0.0, 1.0);
}
)";

// --- Блики ламп: свечение и звезда у каждой видимой лампы --------------------
const char* kLightFlareFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uVisible;
uniform vec4 uLights[16];    // (u, v, размер, сила)
uniform vec3 uColors[16];
uniform int uCount;
uniform float uAspect;
uniform float uRoll;
void main() {
    vec2 uv = vec2(vUV.x * uAspect, vUV.y);
    vec2 center = vec2(0.5 * uAspect, 0.5);
    vec3 color = vec3(0.0);
    for (int i = 0; i < 16; ++i) {
        if (i >= uCount) break;
        float vis = texelFetch(uVisible, ivec2(i, 0), 0).r;
        if (vis <= 0.001) continue;
        vec4 L = uLights[i];
        vec2 p = vec2(L.x * uAspect, L.y);
        vec2 d = uv - p;
        float dist = length(d);
        float size = max(L.z, 1e-4);
        // Свечение: плотное ядро и широкий ореол — как у колбы в объективе.
        float glow = exp(-dist / size) + exp(-dist / (size * 4.0)) * 0.15;
        // Звезда: тонкие лучи, быстро спадающие, — дифракция на диафрагме.
        float ang = atan(d.y, d.x) + uRoll;
        float star = pow(abs(cos(ang * 3.0)), 24.0) + pow(abs(cos(ang * 3.0 + 0.5236)), 40.0) * 0.4;
        star *= exp(-dist / (size * 3.5)) * 0.6;
        // Слабый призрак по ту сторону центра кадра.
        vec2 ghost = center + (center - p) * 0.6;
        float g = exp(-pow(length(uv - ghost) / (size * 2.5), 2.0)) * 0.12;
        color += uColors[i] * (glow + star + g) * L.w * vis;
    }
    FragColor = vec4(color, 1.0);
}
)";

// --- Экранные блики: яркое кадра, в четверти разрешения ---------------------
const char* kScreenBrightFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uScene;
uniform vec2 uTexel;
uniform float uThreshold;
void main() {
    // Четыре выборки на тексель четверти: без них тонкий яркий блик (нить
    // накала, край отражения) проскакивал между выборками и мигал.
    vec3 c = vec3(0.0);
    c += texture(uScene, vUV + uTexel * vec2(-1.0, -1.0)).rgb;
    c += texture(uScene, vUV + uTexel * vec2( 1.0, -1.0)).rgb;
    c += texture(uScene, vUV + uTexel * vec2(-1.0,  1.0)).rgb;
    c += texture(uScene, vUV + uTexel * vec2( 1.0,  1.0)).rgb;
    c = max(c * 0.25, vec3(0.0));
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    // Мягкое колено порога, и сверху — предел: одно солнце в кадре иначе
    // давало бы призраки ярче самой сцены.
    float k = smoothstep(uThreshold, uThreshold * 2.0, l);
    FragColor = vec4(min(c * k, vec3(4.0)), 1.0);
}
)";

// --- Экранные блики: призраки и ореол ---------------------------------------
//
// Тот же приём, что в Unreal и в большинстве движков: блики ложатся на прямую
// «источник — центр кадра» по ту сторону центра, поэтому изображение ярких мест
// ОТРАЖАЕТСЯ через центр и выбирается несколько раз с шагом. Каждый призрак
// гаснет к краю кадра (иначе на краях появляются обрезанные пятна), а каналы
// расходятся — дисперсия стекла.
const char* kScreenGhostFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uBright;
uniform int uGhosts;
uniform float uSpacing;
uniform float uHaloWidth;
uniform float uChroma;
vec3 Chromatic(vec2 uv, vec2 dir) {
    vec2 o = dir * uChroma * 0.006;
    return vec3(texture(uBright, uv + o).r, texture(uBright, uv).g, texture(uBright, uv - o).b);
}
void main() {
    vec2 uv = vec2(1.0) - vUV;                 // отражение через центр
    vec2 ghostVec = (vec2(0.5) - uv) * uSpacing;
    vec2 dir = normalize(ghostVec + vec2(1e-6));
    vec3 color = vec3(0.0);
    for (int i = 0; i < 8; ++i) {
        if (i >= uGhosts) break;
        vec2 off = fract(uv + ghostVec * float(i));
        float w = length(vec2(0.5) - off) / 0.70711;
        w = pow(1.0 - clamp(w, 0.0, 1.0), 6.0);
        // Оттенок призрака по номеру: цепочка не одного цвета, как у оптики.
        vec3 tint = mix(vec3(1.0, 0.75, 0.5), vec3(0.55, 0.8, 1.0), fract(float(i) * 0.37));
        // Дальние переотражения слабее: каждое — ещё один проход через стекло.
        color += Chromatic(off, dir) * w * tint / (1.0 + float(i) * 0.8);
    }
    // Ореол: кольцо вокруг центра, на которое ложится всё яркое.
    vec2 haloUV = fract(uv + dir * uHaloWidth);
    float hw = length(vec2(0.5) - haloUV) / 0.70711;
    hw = pow(1.0 - clamp(abs(hw - 0.55) * 3.0, 0.0, 1.0), 4.0);
    color += Chromatic(haloUV, dir) * hw * 0.35;
    // Блик — отсвет, а не второе изображение: в сумме он заметно слабее
    // источника (у настоящих объективов — проценты от него).
    FragColor = vec4(color * 0.2, 1.0);
}
)";

// Размытие призраков (крест 13 выборок по двум осям за один проход): резкие
// призраки выглядят нарисованными, у настоящих край мягкий.
const char* kScreenBlurFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform vec2 uTexel;
void main() {
    vec3 c = texture(uTex, vUV).rgb * 0.2;
    for (int i = 1; i <= 3; ++i) {
        float w = (4.0 - float(i)) / 15.0;
        c += texture(uTex, vUV + vec2(uTexel.x * float(i) * 1.5, 0.0)).rgb * w;
        c += texture(uTex, vUV - vec2(uTexel.x * float(i) * 1.5, 0.0)).rgb * w;
        c += texture(uTex, vUV + vec2(0.0, uTexel.y * float(i) * 1.5)).rgb * w;
        c += texture(uTex, vUV - vec2(0.0, uTexel.y * float(i) * 1.5)).rgb * w;
    }
    FragColor = vec4(c, 1.0);
}
)";

const char* kScreenAddFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform float uIntensity;
void main() { FragColor = vec4(texture(uTex, vUV).rgb * uIntensity, 1.0); }
)";

Shader& VisibleShader() {
    static Shader* s = new Shader(Shader::FromSource(kVert, kVisibleFrag, "LensFlare.Visible"));
    return *s;
}
Shader& FlareShader() {
    static Shader* s = new Shader(Shader::FromSource(kVert, kFlareFrag, "LensFlare.Draw"));
    return *s;
}

Shader& LightVisibleShader() {
    static Shader* s = new Shader(Shader::FromSource(kVert, kLightVisibleFrag, "LensFlare.LightVisible"));
    return *s;
}
Shader& LightFlareShader() {
    static Shader* s = new Shader(Shader::FromSource(kVert, kLightFlareFrag, "LensFlare.Lights"));
    return *s;
}
Shader& ScreenBrightShader() {
    static Shader* s = new Shader(Shader::FromSource(kVert, kScreenBrightFrag, "LensFlare.ScreenBright"));
    return *s;
}
Shader& ScreenGhostShader() {
    static Shader* s = new Shader(Shader::FromSource(kVert, kScreenGhostFrag, "LensFlare.ScreenGhost"));
    return *s;
}
Shader& ScreenBlurShader() {
    static Shader* s = new Shader(Shader::FromSource(kVert, kScreenBlurFrag, "LensFlare.ScreenBlur"));
    return *s;
}
Shader& ScreenAddShader() {
    static Shader* s = new Shader(Shader::FromSource(kVert, kScreenAddFrag, "LensFlare.ScreenAdd"));
    return *s;
}

std::unique_ptr<RenderTarget> MakeHdr(int w, int h) {
    RenderTargetDesc d;
    d.Width = std::max(1, w);
    d.Height = std::max(1, h);
    d.Kind = RenderTargetKind::ColorHDR;
    return GraphicsDevice::Get().CreateRenderTarget(d);
}

} // namespace


void LensFlare::Render(Framebuffer& target, sage::rhi::TextureHandle sceneColor,
                       sage::rhi::TextureHandle sceneDepth, int w, int h, const glm::mat4& proj,
                       const glm::mat4& view, const LightingEnvironment& env,
                       const LensFlareSettings& s) {
    RenderInto([&target] { target.Bind(); }, sceneColor, sceneDepth, w, h, proj, view, env, s);
}

void LensFlare::Render(sage::rhi::RenderTarget& target, sage::rhi::TextureHandle sceneColor,
                       sage::rhi::TextureHandle sceneDepth, int w, int h, const glm::mat4& proj,
                       const glm::mat4& view, const LightingEnvironment& env,
                       const LensFlareSettings& s) {
    RenderInto([&target] { target.Bind(); }, sceneColor, sceneDepth, w, h, proj, view, env, s);
}

void LensFlare::RenderInto(const std::function<void()>& bindTarget,
                           sage::rhi::TextureHandle sceneColor,
                           sage::rhi::TextureHandle sceneDepth, int w, int h,
                           const glm::mat4& proj, const glm::mat4& view,
                           const LightingEnvironment& env, const LensFlareSettings& s) {
    if (!s.Enabled || s.Intensity <= 0.0f) return;
    if (!sceneColor.Valid() || !sceneDepth.Valid() || w < 8 || h < 8) return;
    if (!m_fsTri) m_fsTri = GraphicsDevice::Get().CreateGeometry(VertexLayout{});
    // Три независимых источника блика. Солнца может не быть в кадре, а лампы —
    // быть, и наоборот: ни один не должен гасить другие своим «нечего рисовать».
    RenderSun(bindTarget, sceneColor, sceneDepth, w, h, proj, view, env, s);
    if (s.Lights > 0.0f) RenderLights(bindTarget, sceneDepth, w, h, proj, view, env, s);
    if (s.Screen > 0.0f) RenderScreen(bindTarget, sceneColor, w, h, s);
    GraphicsDevice& device = GraphicsDevice::Get();
    device.SetBlendMode(GraphicsDevice::BlendMode::Alpha);
    device.SetBlend(false);
    device.SetDepthTest(true);
}

void LensFlare::RenderSun(const std::function<void()>& bindTarget,
                          sage::rhi::TextureHandle sceneColor,
                          sage::rhi::TextureHandle sceneDepth, int w, int h,
                          const glm::mat4& proj, const glm::mat4& view,
                          const LightingEnvironment& env, const LensFlareSettings& s) {

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
    bindTarget();
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

void LensFlare::RenderLights(const std::function<void()>& bindTarget,
                             sage::rhi::TextureHandle sceneDepth, int w, int h,
                             const glm::mat4& proj, const glm::mat4& view,
                             const LightingEnvironment& env, const LensFlareSettings& s) {
    // Лампы, которые видны в кадре: перед камерой, не дальше полутора экранов
    // от края и не слишком далеко по своей же дальности света.
    struct Entry { glm::vec3 Vis; glm::vec4 Draw; glm::vec3 Color; };
    std::vector<Entry> lights;
    const glm::vec3 eye = glm::vec3(glm::inverse(view)[3]);
    auto consider = [&](const glm::vec3& pos, const glm::vec3& color, float intensity, float range,
                        float facing) {
        if (intensity <= 0.0f || facing <= 0.0f || (int)lights.size() >= kMaxLights) return;
        const glm::vec4 vpos = view * glm::vec4(pos, 1.0f);
        const float depth = -vpos.z;
        if (depth < 0.05f) return;                      // за спиной или в упор
        const glm::vec4 clip = proj * vpos;
        if (clip.w <= 0.0f) return;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        if (std::max(std::abs(ndc.x), std::abs(ndc.y)) > 1.05f) return;
        // Далёкая лампа — маленькая и слабая точка, а не прожектор в лицо.
        const float distFade = 1.0f - glm::smoothstep(range * 2.0f, range * 6.0f, depth);
        if (distFade <= 0.0f) return;
        const float size = s.LightSize * glm::clamp(1.0f / std::sqrt(std::max(depth, 0.25f)), 0.15f, 2.0f);
        const float strength = s.Lights * s.Intensity * std::min(intensity, 8.0f) * 0.35f * distFade * facing;
        Entry e;
        e.Vis = glm::vec3(ndc * 0.5f + 0.5f, depth);
        e.Draw = glm::vec4(ndc * 0.5f + 0.5f, size, strength);
        e.Color = color;
        lights.push_back(e);
    };
    for (const PointLight& l : env.PointLights) consider(l.Position, l.Color, l.Intensity, l.Range, 1.0f);
    for (const SpotLight& l : env.SpotLights) {
        // Прожектор горит в объективе, когда смотрят В его конус; сбоку — едва.
        const glm::vec3 toEye = glm::normalize(eye - l.Position);
        const float c = glm::dot(glm::normalize(l.Direction), toEye);
        const float cosOuter = std::cos(glm::radians(l.OuterAngleDeg));
        const float cosInner = std::cos(glm::radians(l.InnerAngleDeg));
        const float facing = 0.12f + 0.88f * glm::smoothstep(cosOuter, cosInner, c);
        consider(l.Position, l.Color, l.Intensity, l.Range, facing);
    }
    if (lights.empty()) return;

    SAGE_PROFILE("Блики ламп");
    GraphicsDevice& device = GraphicsDevice::Get();
    if (!m_lightVisible) {
        RenderTargetDesc d;
        d.Width = kMaxLights;
        d.Height = 1;
        d.Kind = RenderTargetKind::ColorHDR;
        m_lightVisible = device.CreateRenderTarget(d);
    }
    if (!m_lightVisible) return;
    const int count = (int)lights.size();
    const float aspect = (float)w / (float)std::max(h, 1);
    const bool ortho = std::abs(proj[3][3] - 1.0f) < 1e-4f;

    m_lightVisible->Bind();
    device.SetViewport(0, 0, kMaxLights, 1);
    device.SetDepthTest(false);
    device.SetBlend(false);
    Shader& vis = LightVisibleShader();
    vis.Use();
    vis.SetInt("uDepth", 0);
    device.BindTexture2D(0, sceneDepth);
    for (int i = 0; i < count; ++i) vis.SetVec3("uLights[" + std::to_string(i) + "]", lights[(size_t)i].Vis);
    vis.SetInt("uCount", count);
    vis.SetFloat("uP22", proj[2][2]);
    vis.SetFloat("uP32", proj[3][2]);
    vis.SetInt("uOrtho", ortho ? 1 : 0);
    vis.SetFloat("uTolerance", 0.4f);
    vis.SetFloat("uAspect", aspect);
    m_fsTri->DrawArrays(3);

    bindTarget();
    device.SetViewport(0, 0, w, h);
    device.SetBlend(true);
    device.SetBlendMode(GraphicsDevice::BlendMode::Additive);
    Shader& fl = LightFlareShader();
    fl.Use();
    fl.SetInt("uVisible", 0);
    device.BindTexture2D(0, m_lightVisible->ColorTextureHandle());
    for (int i = 0; i < count; ++i) {
        const std::string n = std::to_string(i);
        fl.SetVec4("uLights[" + n + "]", lights[(size_t)i].Draw);
        fl.SetVec3("uColors[" + n + "]", lights[(size_t)i].Color);
    }
    fl.SetInt("uCount", count);
    fl.SetFloat("uAspect", aspect);
    fl.SetFloat("uRoll", std::atan2(view[1][0], view[1][1]));
    m_fsTri->DrawArrays(3);
    device.SetBlend(false);
}

void LensFlare::RenderScreen(const std::function<void()>& bindTarget,
                             sage::rhi::TextureHandle sceneColor, int w, int h,
                             const LensFlareSettings& s) {
    SAGE_PROFILE("Экранные блики");
    GraphicsDevice& device = GraphicsDevice::Get();
    const int qw = std::max(1, w / 4), qh = std::max(1, h / 4);
    if (!m_ssBright || m_ssW != qw || m_ssH != qh) {
        m_ssBright = MakeHdr(qw, qh);
        m_ssGhost = MakeHdr(qw, qh);
        m_ssBlur = MakeHdr(qw, qh);
        m_ssW = qw;
        m_ssH = qh;
    }
    if (!m_ssBright || !m_ssGhost || !m_ssBlur) return;
    device.SetDepthTest(false);
    device.SetBlend(false);

    m_ssBright->Bind();
    Shader& br = ScreenBrightShader();
    br.Use();
    br.SetInt("uScene", 0);
    device.BindTexture2D(0, sceneColor);
    br.SetVec2("uTexel", glm::vec2(1.0f / (float)w, 1.0f / (float)h));
    br.SetFloat("uThreshold", std::max(s.ScreenThreshold, 0.01f));
    m_fsTri->DrawArrays(3);

    m_ssGhost->Bind();
    Shader& gh = ScreenGhostShader();
    gh.Use();
    gh.SetInt("uBright", 0);
    device.BindTexture2D(0, m_ssBright->ColorTextureHandle());
    gh.SetInt("uGhosts", glm::clamp(s.ScreenGhosts, 1, 8));
    gh.SetFloat("uSpacing", glm::clamp(s.GhostSpacing, 0.05f, 1.0f));
    gh.SetFloat("uHaloWidth", glm::clamp(s.HaloRadius, 0.05f, 1.0f));
    gh.SetFloat("uChroma", s.Chroma);
    m_fsTri->DrawArrays(3);

    m_ssBlur->Bind();
    Shader& bl = ScreenBlurShader();
    bl.Use();
    bl.SetInt("uTex", 0);
    bl.SetVec2("uTexel", glm::vec2(1.0f / (float)qw, 1.0f / (float)qh));
    device.BindTexture2D(0, m_ssGhost->ColorTextureHandle());
    m_fsTri->DrawArrays(3);

    bindTarget();
    device.SetViewport(0, 0, w, h);
    device.SetBlend(true);
    device.SetBlendMode(GraphicsDevice::BlendMode::Additive);
    Shader& add = ScreenAddShader();
    add.Use();
    add.SetInt("uTex", 0);
    add.SetFloat("uIntensity", s.Screen * s.Intensity);
    device.BindTexture2D(0, m_ssBlur->ColorTextureHandle());
    m_fsTri->DrawArrays(3);
    device.SetBlend(false);
}

} // namespace sage::render
