#include "sage/render/PostFX.h"

#include <algorithm>
#include <string>

#include <glm/gtc/matrix_inverse.hpp>

#include "sage/render/Shader.h"
#include "sage/rhi/GraphicsDevice.h"

using namespace sage::rhi;

namespace sage::render {

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

vec3 ViewPos(vec2 uv) {
    float d = texture(uDepth, uv).r;
    vec4 c = uInvProj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return c.xyz / c.w;
}

void main() {
    float d = texture(uDepth, vUV).r;
    if (d >= 1.0) { FragColor = vec4(1.0); return; } // фон — без затенения

    vec3 P = ViewPos(vUV);
    // Нормаль из производных вид-позиции (не нужен отдельный G-буфер нормалей).
    vec3 N = normalize(cross(dFdx(P), dFdy(P)));
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
        float rangeCheck = smoothstep(0.0, 1.0, uRadius / max(abs(P.z - sampleZ), 1e-4));
        occ += (sampleZ >= sp.z + 0.02 ? 1.0 : 0.0) * rangeCheck;
    }
    float ao = 1.0 - occ / float(N_SAMPLES);
    FragColor = vec4(vec3(ao), 1.0);
}
)";

// Размытие AO (4x4 бокс) — убирает шум выборки.
const char* kAoBlurFrag = R"(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uAO;
uniform vec2 uTexel;
void main() {
    float sum = 0.0;
    for (int x = -2; x < 2; ++x)
        for (int y = -2; y < 2; ++y)
            sum += texture(uAO, vUV + vec2(float(x), float(y)) * uTexel).r;
    FragColor = vec4(vec3(sum / 16.0), 1.0);
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

// Финальный composite: scene*AO + bloom -> экспозиция -> ACES -> насыщенность/
// контраст -> виньетка -> гамма.
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

vec3 ACES(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(uScene, vUV).rgb;
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
Shader& BrightShader()    { static Shader* s = new Shader(Shader::FromSource(kFsVert, kBrightFrag, "PostFX.Bright")); return *s; }
Shader& BlurShader()      { static Shader* s = new Shader(Shader::FromSource(kFsVert, kBlurFrag, "PostFX.Blur")); return *s; }
Shader& CompositeShader() { static Shader* s = new Shader(Shader::FromSource(kFsVert, kCompositeFrag, "PostFX.Composite")); return *s; }

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
}

void PostFX::Render(unsigned int sceneColor, unsigned int sceneDepth, int w, int h,
                    const glm::mat4& proj, const PostFXSettings& s,
                    Framebuffer* output, int outX, int outY, int outW, int outH) {
    GraphicsDevice& device = GraphicsDevice::Get();
    EnsureTargets(w, h);
    device.SetDepthTest(false); // все проходы — полноэкранные, глубина не нужна

    auto drawTri = [&]() { m_fsTri->DrawArrays(3); };

    bool doAO = s.AOEnabled && sceneDepth != 0;
    bool doBloom = s.BloomEnabled;

    // --- 1. SSAO из глубины сцены -> m_ao, затем размытие -> m_aoBlur ---
    if (doAO) {
        glm::mat4 invProj = glm::inverse(proj);
        m_ao->Bind();
        Shader& ao = SsaoShader();
        ao.Use();
        ao.SetInt("uDepth", 0);
        device.BindTexture2D(0, sceneDepth);
        ao.SetMat4("uProj", proj);
        ao.SetMat4("uInvProj", invProj);
        ao.SetFloat("uRadius", s.AORadius);
        drawTri();

        m_aoBlur->Bind();
        Shader& aob = AoBlurShader();
        aob.Use();
        aob.SetInt("uAO", 0);
        device.BindTexture2D(0, m_ao->ColorTextureHandle());
        aob.SetVec2("uTexel", glm::vec2(1.0f / (float)w, 1.0f / (float)h));
        drawTri();
    }

    // --- 2. Bloom: bright-pass -> размытие (2 итерации, ping-pong) ---
    if (doBloom) {
        int hw = m_bright->Width(), hh = m_bright->Height();
        m_bright->Bind();
        Shader& br = BrightShader();
        br.Use();
        br.SetInt("uScene", 0);
        device.BindTexture2D(0, sceneColor);
        br.SetFloat("uThreshold", s.BloomThreshold);
        drawTri();

        Shader& blur = BlurShader();
        blur.Use();
        blur.SetInt("uTex", 0);
        glm::vec2 texel(1.0f / (float)hw, 1.0f / (float)hh);
        unsigned int src = m_bright->ColorTextureHandle();
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

    // --- 3. Composite -> output (FBO вьюпорта) или экран ---
    if (output) {
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
    device.BindTexture2D(0, sceneColor);
    if (doBloom) device.BindTexture2D(1, m_bloomB->ColorTextureHandle());
    if (doAO) device.BindTexture2D(2, m_aoBlur->ColorTextureHandle());
    drawTri();

    device.SetDepthTest(true);
}

} // namespace sage::render
