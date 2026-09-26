#include "sage/render/Reflection.h"
#include "sage/core/Profiler.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/core/Log.h"
#include "sage/render/Shader.h"
#include "sage/render/SkyRenderer.h"
#include "sage/render/Skybox.h"
#include "sage/scene/Light.h"
#include "sage/scene/Scene.h"

namespace sage::render {

namespace {

// Направления взгляда и «верх» для шести граней куба, в порядке +X,-X,+Y,-Y,+Z,-Z.
//
// Знаки не «подобраны, пока не совпало»: cubemap в OpenGL наследует соглашение
// RenderMan, где грани смотрят из центра куба, а вертикальная ось перевёрнута
// относительно обычной камеры. Отсюда -Y в качестве «верха» у четырёх боковых
// граней. Ошибка здесь не роняет ничего — отражение просто оказывается
// зеркальным или повёрнутым на 90°, и заметить это можно только на
// несимметричной сцене.
const glm::vec3 kFaceDir[6] = {
    { 1.0f,  0.0f,  0.0f}, {-1.0f,  0.0f,  0.0f},
    { 0.0f,  1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f},
    { 0.0f,  0.0f,  1.0f}, { 0.0f,  0.0f, -1.0f},
};
const glm::vec3 kFaceUp[6] = {
    { 0.0f, -1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f},
    { 0.0f,  0.0f,  1.0f}, { 0.0f,  0.0f, -1.0f},
    { 0.0f, -1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f},
};

// СВЁРТКА ПО GGX — то, чем мип куба становится шероховатостью.
//
// Раньше мипы строила аппаратная фильтрация (среднее четырёх текселей). Это
// уменьшенная копия, а не размытое отражение: на средней шероховатости матовый
// материал показывал узнаваемую, только крупно-пиксельную картинку комнаты —
// кубики на пластике, «отражение там, где его быть не должно», — а на стыках
// граней ступеньки. Здесь каждый мип — честный интеграл окружения по лепестку
// GGX своей шероховатости (importance sampling, Karis 2013), с выборкой из
// мипа источника по плотности сэмпла (filtered importance sampling): 64 луча
// дают гладкий результат без шума.
const char* kPrefilterVS = R"GLSL(
#version 330 core
out vec3 vDir;
uniform mat4 uInvProj;
uniform mat4 uInvViewRot;
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vec2 ndc = pos * 2.0 - 1.0;
    vec4 vp = uInvProj * vec4(ndc, 1.0, 1.0);
    // Не нормализуем до фрагментного шейдера — см. SkyRenderer.cpp.
    vDir = mat3(uInvViewRot) * (vp.xyz / vp.w);
    gl_Position = vec4(ndc, 1.0, 1.0);
}
)GLSL";

const char* kPrefilterFS = R"GLSL(
#version 330 core
in vec3 vDir;
out vec4 FragColor;
uniform samplerCube uSrc;
uniform float uRough;
uniform float uSrcSize;
uniform float uSrcMaxLod;
const float PI = 3.14159265;
const uint kSamples = 64u;

float RadicalInverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

void main() {
    vec3 N = normalize(vDir);
    // Нулевой мип — зеркало: копия без свёртки.
    if (uRough < 1e-3) {
        FragColor = vec4(textureLod(uSrc, N, 0.0).rgb, 1.0);
        return;
    }
    float a = uRough * uRough;
    float a2 = a * a;
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);
    // Телесный угол одного текселя нулевого мипа источника.
    float saTexel = 4.0 * PI / (6.0 * uSrcSize * uSrcSize);
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (uint i = 0u; i < kSamples; ++i) {
        vec2 xi = vec2(float(i) / float(kSamples), RadicalInverse(i));
        float phi = 2.0 * PI * xi.x;
        float cosT = sqrt((1.0 - xi.y) / (1.0 + (a2 - 1.0) * xi.y));
        float sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
        vec3 H = T * (cos(phi) * sinT) + B * (sin(phi) * sinT) + N * cosT;
        vec3 L = 2.0 * dot(N, H) * H - N;
        float NdotL = dot(N, L);
        if (NdotL <= 0.0) continue;
        // Плотность сэмпла: D·NdotH / (4·VdotH), а при N = V это D / 4.
        float d = cosT * cosT * (a2 - 1.0) + 1.0;
        float pdf = a2 / (PI * d * d) * 0.25;
        float saSample = 1.0 / (float(kSamples) * pdf + 1e-4);
        float lod = clamp(0.5 * log2(saSample / saTexel) + 1.0, 0.0, uSrcMaxLod);
        sum += textureLod(uSrc, L, lod).rgb * NdotL;
        wsum += NdotL;
    }
    FragColor = vec4(sum / max(wsum, 1e-4), 1.0);
}
)GLSL";

// Сколько мипов у свёрнутого куба. Шесть (128 → 4) — шкала «зеркало →
// матовое»: грань в 4 текселя уже ровнее любого лепестка, а ещё меньшие мипы
// только стоят времени свёртки.
constexpr int kFilteredMips = 6;

} // namespace

SkyCelestials ReflectedSky(const LightingEnvironment& env) {
    // Форма неба, низ и облака — те же, что в кадре: иначе вода под небом
    // с блочным небом отражала бы небо движка по умолчанию.
    SkyCelestials c = CelestialsFromEnvironment(env);
    // Диски светил в куб НЕ идут. Солнце уже даёт блик прямым светом (см.
    // PbrContrib), и его диск в отражении — второй блик рядом с первым; к тому
    // же он в шесть раз ярче неба и при размытии по шероховатости растекается
    // пятном по всему матовому материалу.
    c.Enabled = false;
    return c;
}

std::string SkyShapeKey(const SkyCelestials& c) {
    // Строкой, а не operator==: полей три десятка, и забытое в сравнении поле
    // означало бы отражение, которое не обновляется от одной-единственной
    // настройки. Время в ключ не входит: облака в отражении не плывут —
    // переснимать куб каждый кадр ради них слишком дорого.
    char buf[768];
    std::snprintf(buf, sizeof(buf),
                  "%g %g %g %d %g %g %g %g|%g %g %d|%g %g|%d %g %g %g %g %g %g %g %g %g %g|"
                  "%d %g %g %g %g %g %g %g %g %g %g",
                  c.GradientExponent, c.HorizonSoftness, c.HorizonOffset, (int)c.Ground,
                  c.GroundColor.x, c.GroundColor.y, c.GroundColor.z, c.GroundBlend,
                  c.SunBrightness, c.SunGlow, (int)c.MoonPhase,
                  c.StarDensity, c.StarSize,
                  (int)c.Clouds, c.CloudColor.x, c.CloudColor.y, c.CloudColor.z,
                  c.CloudHeight, c.CloudScale, c.CloudCoverage, c.CloudOpacity, c.CloudFade,
                  c.CloudWind.x, c.CloudWind.y,
                  (int)c.HeightFog, c.FogColor.x, c.FogColor.y, c.FogColor.z, c.FogDensity,
                  c.FogFalloff, c.FogHeight, c.FogMaxOpacity, c.FogSunScatter, c.FogSunExponent);
    return std::string(buf) + "|" + c.SunTexture + "|" + c.MoonTexture;
}

glm::mat4 EnvironmentMap::FaceView(int face, const glm::vec3& pos) {
    if (face < 0 || face > 5) face = 0;
    return glm::lookAt(pos, pos + kFaceDir[face], kFaceUp[face]);
}

glm::mat4 EnvironmentMap::FaceProj(float nearClip, float farClip) {
    // Ровно 90° и квадратный кадр — иначе грани не сойдутся на стыках.
    return glm::perspective(glm::radians(90.0f), 1.0f, nearClip, farClip);
}

EnvironmentMap::EnvironmentMap(int size) {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    // Два куба: СЫРОЙ — в него снимается сцена (с глубиной и обычными
    // мипами для выборки свёртки), и СВЁРНУТЫЙ — его читает шейдер материала.
    // Один куб на оба дела нельзя: свёртка читает нулевой мип и пишет в
    // остальные, а читать и писать одну текстуру за проход GL не разрешает.
    sage::rhi::CubeRenderTargetDesc raw;
    raw.Size = size;
    raw.WithDepth = true;
    m_raw = device.CreateCubeRenderTarget(raw);
    sage::rhi::CubeRenderTargetDesc filtered;
    filtered.Size = size;
    filtered.WithDepth = false;
    filtered.MipLevels = kFilteredMips;
    m_cube = device.CreateCubeRenderTarget(filtered);
    if (m_raw && m_cube) {
        m_filter = device.CreateShaderProgram(kPrefilterVS, kPrefilterFS);
        m_quad = device.CreateGeometry(sage::rhi::VertexLayout{});
    }
    if (!m_raw || !m_cube || !m_filter || !m_quad) {
        LOG_WARN("Reflection") << "Бэкенд не умеет кубические таргеты — отражения окружения выключены";
        m_raw.reset();
        m_cube.reset();
    }
}

EnvironmentMap::~EnvironmentMap() = default;

void EnvironmentMap::Prefilter() {
    SAGE_PROFILE("Свёртка куба отражений");
    sage::rhi::GraphicsDevice& dev = sage::rhi::GraphicsDevice::Get();
    m_raw->GenerateMips();
    dev.SetBlend(false);
    dev.SetDepthTest(false);
    dev.SetDepthWrite(false);
    dev.SetCullMode(sage::rhi::CullMode::Off);
    m_filter->Use();
    m_raw->Bind(0);
    m_filter->SetInt("uSrc", 0);
    m_filter->SetFloat("uSrcSize", (float)m_raw->Size());
    m_filter->SetFloat("uSrcMaxLod", (float)(m_raw->MipLevels() - 1));
    m_filter->SetMat4("uInvProj", glm::inverse(FaceProj(0.1f, 10.0f)));
    const int mips = m_cube->MipLevels();
    for (int mip = 0; mip < mips; ++mip) {
        // Шероховатость мипа — та же линейная шкала, по которой его выбирает
        // шейдер материала (rough * uEnvMaxLod, см. SpecularIBL).
        m_filter->SetFloat("uRough", mips > 1 ? (float)mip / (float)(mips - 1) : 0.0f);
        for (int face = 0; face < 6; ++face) {
            m_cube->BindFace(face, mip);
            m_filter->SetMat4("uInvViewRot",
                              glm::mat4(glm::transpose(glm::mat3(FaceView(face, glm::vec3(0.0f))))));
            m_quad->DrawArrays(3);
        }
    }
    dev.SetCullMode(sage::rhi::CullMode::Back);
    dev.SetDepthWrite(true);
    dev.SetDepthTest(true);
}

void EnvironmentMap::Capture(const glm::vec3& pos, float nearClip, float farClip,
                             const FaceDraw& draw) {
    if (!Valid() || !draw) return;
    m_position = pos;
    const glm::mat4 proj = FaceProj(nearClip, farClip);
    for (int face = 0; face < 6; ++face) {
        m_raw->BindFace(face, 0);
        draw(FaceView(face, pos), proj);
    }
    Prefilter();
}

void EnvironmentMap::CaptureSky(SkyRenderer& sky, const LightingEnvironment& env,
                                const Skybox* cubemap) {
    SAGE_PROFILE("Куб окружения");
    if (!Valid()) return;
    m_position = glm::vec3(0.0f);
    m_hasBox = false;      // небо бесконечно, коробку к нему приложить не к чему
    const glm::mat4 proj = FaceProj(0.1f, 10.0f);
    sage::rhi::GraphicsDevice& dev = sage::rhi::GraphicsDevice::Get();
    for (int face = 0; face < 6; ++face) {
        m_raw->BindFace(face, 0);
        dev.Clear(true, true);
        const glm::mat4 view = FaceView(face, glm::vec3(0.0f));
        if (cubemap) cubemap->Draw(view, proj, env.Skybox.Intensity, env.Skybox.RotationDeg);
        // ЦВЕТА РАЗРЕШЁННЫЕ, а не авторские: в них учтено время суток (см.
        // sage/render/SkyModel.h). С полями настроек вода ночью отражала бы
        // полуденное небо — то есть светилась бы ярче, чем всё вокруг.
        else sky.Draw(view, proj, env.SkyTop(), env.SkyHorizon(), ReflectedSky(env));
    }
    Prefilter();
}

// ---------------------------------------------------------------------------
//  Зонды в сцене
// ---------------------------------------------------------------------------

int UpdateReflectionProbes(Scene& scene, const EnvironmentMap::FaceDraw& draw, int maxPerCall) {
    SAGE_PROFILE("Зонды отражений");
    if (!draw) return 0;
    int captured = 0;
    auto view = scene.Registry().view<ReflectionProbeComponent, Transform>();
    for (auto e : view) {
        if (captured >= maxPerCall) break;
        ReflectionProbeComponent& probe = view.get<ReflectionProbeComponent>(e);
        if (!probe.Dirty && !probe.Realtime) continue;

        std::shared_ptr<EnvironmentMap> env =
            std::static_pointer_cast<EnvironmentMap>(probe.Runtime);
        // Пересоздаём и при смене разрешения: менять сторону граней у готового
        // куба нельзя, а художник крутит это число прямо в инспекторе.
        if (!env || env->Size() != std::max(16, probe.Resolution)) {
            env = std::make_shared<EnvironmentMap>(std::max(16, probe.Resolution));
            probe.Runtime = env;
        }
        if (!env->Valid()) { probe.Dirty = false; continue; }

        const glm::vec3 pos(scene.WorldMatrix(e)[3]);
        env->Capture(pos, 0.1f, std::max(1.0f, probe.FarClip), draw);
        if (probe.BoxParallax) env->SetBox(pos - probe.BoxHalfExtents, pos + probe.BoxHalfExtents);
        else env->ClearBox();
        probe.Dirty = false;
        ++captured;
    }
    return captured;
}

int ReflectionProbeSet::Pick(const glm::vec3& p) const {
    for (int i = 0; i < (int)Entries.size(); ++i) {
        const Entry& e = Entries[(size_t)i];
        if (glm::all(glm::greaterThanEqual(p, e.Min)) && glm::all(glm::lessThanEqual(p, e.Max)))
            return i;
    }
    return -1;
}

ReflectionProbeSet CollectReflectionProbes(Scene& scene) {
    ReflectionProbeSet set;
    auto view = scene.Registry().view<ReflectionProbeComponent, Transform>();
    for (auto e : view) {
        const ReflectionProbeComponent& probe = view.get<ReflectionProbeComponent>(e);
        const auto env = std::static_pointer_cast<EnvironmentMap>(probe.Runtime);
        if (!env || !env->Valid() || probe.Dirty) continue;   // ещё не снят
        const glm::vec3 pos(scene.WorldMatrix(e)[3]);
        const glm::vec3 half = glm::abs(probe.BoxHalfExtents);
        set.Entries.push_back({env.get(), probe.Intensity, pos - half, pos + half});
    }
    std::stable_sort(set.Entries.begin(), set.Entries.end(),
                     [](const ReflectionProbeSet::Entry& a, const ReflectionProbeSet::Entry& b) {
                         const glm::vec3 da = a.Max - a.Min, db = b.Max - b.Min;
                         return da.x * da.y * da.z < db.x * db.y * db.z;
                     });
    return set;
}

ReflectionBinding ReflectionBinding::ForProbe(int probe) const {
    ReflectionBinding b = *this;
    if (Probes && probe >= 0 && probe < (int)Probes->Entries.size()) {
        const ReflectionProbeSet::Entry& e = Probes->Entries[(size_t)probe];
        b.Env = e.Env;
        // Общая сила отражений сцены действует и на зонды: иначе ползунок
        // «Отражения» в окружении гасил бы небо и не трогал комнаты.
        b.Intensity = Intensity * e.Intensity;
    }
    return b;
}

// ---------------------------------------------------------------------------
//  ReflectionSystem
// ---------------------------------------------------------------------------

EnvironmentMap& ReflectionSystem::Ensure() {
    if (!m_env) m_env = std::make_unique<EnvironmentMap>(128);
    return *m_env;
}

void ReflectionSystem::UpdateSky(SkyRenderer& sky, const LightingEnvironment& env,
                                 const Skybox* cubemap) {
    if (!m_enabled) return;
    // Пересъёмка только на изменение неба. Сравнение по ЗНАЧЕНИЯМ, а не по
    // флагу «поменялось»: небо правит и редактор, и скрипт, и загрузка сцены,
    // и уговориться, что все они дёргают флаг, не выйдет.
    // Ключ описывает ИСТОЧНИК неба целиком, а не только путь к каталогу: в
    // режиме «по кусочкам» каталога нет вовсе, а оставшаяся от прежнего режима
    // строка одинакова для любых шести файлов — по ней смена набора выглядела
    // бы как «ничего не изменилось», и вода отражала бы прежнее небо.
    std::string dir;
    if (cubemap) {
        dir = std::to_string((int)env.Skybox.Kind) + "|" + env.Skybox.CubemapDir;
        for (int i = 0; i < 6; ++i) dir += "|" + env.Skybox.FacePaths[i];
    }
    // Сравниваются РАЗРЕШЁННЫЕ цвета: по авторским куб не пересняли бы ни разу
    // за весь заход солнца — они не меняются, меняется время суток.
    // Форма неба — тоже часть ключа: поменяли низ или облака — вода обязана
    // это отразить, хотя цвета зенита и горизонта остались прежними.
    const std::string shape = cubemap ? std::string() : SkyShapeKey(ReflectedSky(env));
    if (m_captured && m_skyTop == env.SkyTop() &&
        m_skyHorizon == env.SkyHorizon() && m_skyCubemap == dir && m_skyShape == shape &&
        m_skyIntensity == env.Skybox.Intensity && m_skyRotation == env.Skybox.RotationDeg) {
        return;
    }
    EnvironmentMap& e = Ensure();
    if (!e.Valid()) return;
    e.CaptureSky(sky, env, cubemap);
    m_skyTop = env.SkyTop();
    m_skyHorizon = env.SkyHorizon();
    m_skyShape = shape;
    m_skyCubemap = dir;
    m_skyIntensity = env.Skybox.Intensity;
    m_skyRotation = env.Skybox.RotationDeg;
    m_captured = true;
}

void ReflectionSystem::CaptureScene(const glm::vec3& pos, float nearClip, float farClip,
                                    const EnvironmentMap::FaceDraw& draw) {
    if (!m_enabled) return;
    EnvironmentMap& e = Ensure();
    if (!e.Valid()) return;
    e.Capture(pos, nearClip, farClip, draw);
    m_captured = true;
    // Небо больше не «то, что снято»: следующий UpdateSky обязан пересобрать
    // куб, иначе он молча оставил бы в нём снимок сцены.
    m_skyTop = glm::vec3(-1.0f);
    m_skyHorizon = glm::vec3(-1.0f);
    m_skyShape.clear();
    m_skyCubemap.clear();
    m_skyIntensity = -1.0f;
}

void ReflectionSystem::SetBox(const glm::vec3& mn, const glm::vec3& mx) { Ensure().SetBox(mn, mx); }
void ReflectionSystem::ClearBox() { if (m_env) m_env->ClearBox(); }

ReflectionBinding ReflectionSystem::Binding(int screenW, int screenH,
                                            sage::rhi::TextureHandle planarTexture) const {
    ReflectionBinding b;
    if (m_enabled && m_captured) b.Env = Env();
    b.Intensity = m_intensity;
    b.PlanarTexture = planarTexture;
    b.ScreenTexel = glm::vec2(screenW > 0 ? 1.0f / (float)screenW : 0.0f,
                              screenH > 0 ? 1.0f / (float)screenH : 0.0f);
    return b;
}

void UploadReflection(Shader& shader, const ReflectionBinding& b) {
    sage::rhi::GraphicsDevice& dev = sage::rhi::GraphicsDevice::Get();

    // Юниты сэмплеров выставляются ВСЕГДА, даже когда источник выключен.
    // Невыставленный uniform-сэмплер равен нулю, то есть юниту albedo, где
    // лежит обычная sampler2D-текстура, — и получается тот самый конфликт
    // типов на одном юните, который драйвер вправе разрешить как угодно. На
    // первом заходе это вылезло не отсутствием отражений, а «плавающим»
    // тестом блендшейпов: два одинаковых кадра переставали совпадать.
    shader.SetInt("uEnvMap", ReflectionBinding::kEnvUnit);
    shader.SetInt("uPlanarMap", ReflectionBinding::kPlanarUnit);

    const bool env = b.Env && b.Env->Valid();
    shader.SetInt("uEnvEnabled", env ? 1 : 0);
    if (env) {
        b.Env->Bind(ReflectionBinding::kEnvUnit);
        shader.SetFloat("uEnvIntensity", b.Intensity);
        shader.SetFloat("uEnvMaxLod", b.Env->MaxLod());
        shader.SetInt("uEnvBoxParallax", b.Env->HasBox() ? 1 : 0);
        if (b.Env->HasBox()) {
            shader.SetVec3("uEnvBoxMin", b.Env->BoxMin());
            shader.SetVec3("uEnvBoxMax", b.Env->BoxMax());
            shader.SetVec3("uEnvProbePos", b.Env->Position());
        }
    } else {
        shader.SetInt("uEnvBoxParallax", 0);
    }

    shader.SetInt("uPlanarEnabled", b.PlanarTexture.Valid() ? 1 : 0);
    if (b.PlanarTexture.Valid()) dev.BindTexture2D(ReflectionBinding::kPlanarUnit, b.PlanarTexture);
    shader.SetVec2("uScreenTexel", b.ScreenTexel);
}


// ---------------------------------------------------------------------------
//  PlanarReflection
// ---------------------------------------------------------------------------

glm::mat4 PlanarReflection::MirrorMatrix(const glm::vec4& p) {
    // Отражение относительно плоскости dot(n,x)+d=0: x' = x - 2*(dot(n,x)+d)*n.
    // В матричном виде — тождественная минус удвоенное внешнее произведение.
    const glm::vec3 n(p);
    glm::mat4 m(1.0f);
    m[0][0] = 1.0f - 2.0f * n.x * n.x; m[1][0] = -2.0f * n.x * n.y;
    m[2][0] = -2.0f * n.x * n.z;       m[3][0] = -2.0f * n.x * p.w;
    m[0][1] = -2.0f * n.y * n.x;       m[1][1] = 1.0f - 2.0f * n.y * n.y;
    m[2][1] = -2.0f * n.y * n.z;       m[3][1] = -2.0f * n.y * p.w;
    m[0][2] = -2.0f * n.z * n.x;       m[1][2] = -2.0f * n.z * n.y;
    m[2][2] = 1.0f - 2.0f * n.z * n.z; m[3][2] = -2.0f * n.z * p.w;
    return m;
}

glm::mat4 PlanarReflection::ObliqueNearPlane(const glm::mat4& proj, const glm::vec4& planeView) {
    // Приём Ланселя/Эрикссона: третья строка проекции подменяется уравнением
    // плоскости, и всё, что за ней, отсекается штатным клиппером. Дальняя
    // плоскость при этом уезжает — для отражения это безразлично, а точность
    // глубины у воды и так не критична.
    glm::mat4 m = proj;
    // Угол фрустума, ПРОТИВОПОЛОЖНЫЙ плоскости отсечения: подмена третьей
    // строки должна оставить его на дальней плоскости, иначе фрустум схлопнется.
    glm::vec4 corner;
    corner.x = ((planeView.x < 0.0f ? -1.0f : 1.0f) - m[2][0]) / m[0][0];
    corner.y = ((planeView.y < 0.0f ? -1.0f : 1.0f) - m[2][1]) / m[1][1];
    corner.z = -1.0f;
    corner.w = (1.0f + m[2][2]) / m[3][2];

    const float denom = glm::dot(planeView, corner);
    if (std::abs(denom) < 1e-6f) return proj;  // плоскость параллельна лучу — правки нет
    const glm::vec4 c = planeView * (2.0f / denom);
    m[0][2] = c.x;
    m[1][2] = c.y;
    m[2][2] = c.z + 1.0f;
    m[3][2] = c.w;
    return m;
}

bool PlanarReflection::Capture(const glm::vec4& plane, const glm::mat4& view,
                               const glm::mat4& proj, int screenW, int screenH,
                               const EnvironmentMap::FaceDraw& draw) {
    SAGE_PROFILE("Плоское отражение");
    m_valid = false;
    if (!draw || screenW <= 0 || screenH <= 0) return false;

    // Камера с изнанки плоскости — отражать нечего (см. комментарий в заголовке).
    const glm::mat4 invView = glm::inverse(view);
    const glm::vec3 eye(invView[3]);
    const float side = glm::dot(glm::vec3(plane), eye) + plane.w;
    if (side <= 0.0f) return false;

    const int w = std::max(1, (int)(screenW * m_scale));
    const int h = std::max(1, (int)(screenH * m_scale));
    if (!m_target) {
        sage::rhi::RenderTargetDesc desc;
        desc.Width = w;
        desc.Height = h;
        desc.Kind = sage::rhi::RenderTargetKind::ColorHDRWithDepth;
        m_target = sage::rhi::GraphicsDevice::Get().CreateRenderTarget(desc);
    } else if (m_target->Width() != w || m_target->Height() != h) {
        m_target->Resize(w, h);
    }
    if (!m_target) return false;

    const glm::mat4 mirrorView = view * MirrorMatrix(plane);
    // Плоскость в пространстве ОТРАЖЁННОГО вида: косое отсечение работает там.
    const glm::vec4 planeView =
        glm::transpose(glm::inverse(mirrorView)) * plane;
    const glm::mat4 mirrorProj = ObliqueNearPlane(proj, planeView);

    sage::rhi::GraphicsDevice& dev = sage::rhi::GraphicsDevice::Get();
    m_target->Bind();
    dev.Clear(true, true);
    // Отражение выворачивает обход треугольников: то, что было лицом, стало
    // изнанкой. Переключаем ОБМОТКУ, а не режим отсечения.
    //
    // Раньше здесь стояло SetCullMode(Front) — и не работало. Отсечение
    // выставляется тут один раз, а батч сцены задаёт своё ВНУТРИ draw
    // (непрозрачное — Back, двусторонняя прозрачность — по проходу) и затирает
    // внешнюю установку первым же вызовом. В отражении показывалась изнанка
    // объектов: у куба — его внутренние стенки вместо наружных.
    //
    // Обмотка — независимое состояние: батч по-прежнему говорит «отсекай
    // заднюю», а какая сторона задняя, решает зеркало.
    dev.SetFrontFace(sage::rhi::FrontFace::Clockwise);
    draw(mirrorView, mirrorProj);
    dev.SetFrontFace(sage::rhi::FrontFace::CounterClockwise);
    dev.SetCullMode(sage::rhi::CullMode::Back);
    m_target->Resolve();
    // Возвращаем экранный буфер: проход менял и привязку, и viewport, и
    // оставлять их за собой — верный способ отрисовать следующий шаг кадра в
    // чужую текстуру. Потребитель, рисующий не в экран (вьюпорт редактора),
    // привязывает свой буфер сам — он всё равно делает это перед проходом сцены.
    dev.BindDefaultFramebuffer();
    dev.SetViewport(0, 0, screenW, screenH);
    m_valid = true;
    return true;
}

sage::rhi::TextureHandle PlanarReflection::Texture() const {
    return (m_valid && m_target) ? m_target->ColorTextureHandle() : sage::rhi::TextureHandle{};
}

} // namespace sage::render
