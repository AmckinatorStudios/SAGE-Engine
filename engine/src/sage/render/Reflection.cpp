#include "sage/render/Reflection.h"
#include "sage/core/Profiler.h"

#include <algorithm>

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

} // namespace

glm::mat4 EnvironmentMap::FaceView(int face, const glm::vec3& pos) {
    if (face < 0 || face > 5) face = 0;
    return glm::lookAt(pos, pos + kFaceDir[face], kFaceUp[face]);
}

glm::mat4 EnvironmentMap::FaceProj(float nearClip, float farClip) {
    // Ровно 90° и квадратный кадр — иначе грани не сойдутся на стыках.
    return glm::perspective(glm::radians(90.0f), 1.0f, nearClip, farClip);
}

EnvironmentMap::EnvironmentMap(int size) {
    sage::rhi::CubeRenderTargetDesc desc;
    desc.Size = size;
    desc.WithDepth = true;
    m_cube = sage::rhi::GraphicsDevice::Get().CreateCubeRenderTarget(desc);
    if (!m_cube) {
        LOG_WARN("Reflection") << "Бэкенд не умеет кубические таргеты — отражения окружения выключены";
    }
}

void EnvironmentMap::Capture(const glm::vec3& pos, float nearClip, float farClip,
                             const FaceDraw& draw) {
    if (!m_cube || !draw) return;
    m_position = pos;
    const glm::mat4 proj = FaceProj(nearClip, farClip);
    for (int face = 0; face < 6; ++face) {
        m_cube->BindFace(face, 0);
        draw(FaceView(face, pos), proj);
    }
    // Мипы — уже по всем шести граням: строить их по одной нельзя, фильтр
    // соседнего мипа на краю грани заглядывает к соседям.
    m_cube->GenerateMips();
}

void EnvironmentMap::CaptureSky(SkyRenderer& sky, const LightingEnvironment& env,
                                const Skybox* cubemap) {
    SAGE_PROFILE("Куб окружения");
    if (!m_cube) return;
    m_position = glm::vec3(0.0f);
    m_hasBox = false;      // небо бесконечно, коробку к нему приложить не к чему
    const glm::mat4 proj = FaceProj(0.1f, 10.0f);
    sage::rhi::GraphicsDevice& dev = sage::rhi::GraphicsDevice::Get();
    for (int face = 0; face < 6; ++face) {
        m_cube->BindFace(face, 0);
        dev.Clear(true, true);
        const glm::mat4 view = FaceView(face, glm::vec3(0.0f));
        if (cubemap) cubemap->Draw(view, proj, env.Skybox.Intensity, env.Skybox.RotationDeg);
        else sky.Draw(view, proj, env.Skybox.TopColor, env.Skybox.HorizonColor);
    }
    m_cube->GenerateMips();
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

const EnvironmentMap* PickReflectionProbe(Scene& scene, const glm::vec3& cameraPos,
                                          float* outIntensity) {
    const EnvironmentMap* best = nullptr;
    float bestScore = 0.0f;
    float bestIntensity = 1.0f;
    bool bestInside = false;

    auto view = scene.Registry().view<ReflectionProbeComponent, Transform>();
    for (auto e : view) {
        const ReflectionProbeComponent& probe = view.get<ReflectionProbeComponent>(e);
        const auto env = std::static_pointer_cast<EnvironmentMap>(probe.Runtime);
        if (!env || !env->Valid() || probe.Dirty) continue;   // ещё не снят

        const glm::vec3 pos(scene.WorldMatrix(e)[3]);
        const glm::vec3 d = glm::abs(cameraPos - pos);
        const bool inside = glm::all(glm::lessThanEqual(d, probe.BoxHalfExtents));
        // Внутри коробки — «объём» коробки (меньше значит теснее и подробнее);
        // снаружи — расстояние. Разные величины не сравниваются: сначала
        // предпочтение отдаётся любому зонду, внутри которого стоит камера.
        const float score = inside ? (probe.BoxHalfExtents.x * probe.BoxHalfExtents.y *
                                      probe.BoxHalfExtents.z)
                                   : glm::length(cameraPos - pos);
        const bool better = !best || (inside && !bestInside) ||
                            (inside == bestInside && score < bestScore);
        if (!better) continue;
        best = env.get();
        bestScore = score;
        bestIntensity = probe.Intensity;
        bestInside = inside;
    }
    if (outIntensity && best) *outIntensity = bestIntensity;
    return best;
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
    const std::string& dir = cubemap ? env.Skybox.CubemapDir : std::string();
    if (m_captured && m_skyTop == env.Skybox.TopColor &&
        m_skyHorizon == env.Skybox.HorizonColor && m_skyCubemap == dir &&
        m_skyIntensity == env.Skybox.Intensity && m_skyRotation == env.Skybox.RotationDeg) {
        return;
    }
    EnvironmentMap& e = Ensure();
    if (!e.Valid()) return;
    e.CaptureSky(sky, env, cubemap);
    m_skyTop = env.Skybox.TopColor;
    m_skyHorizon = env.Skybox.HorizonColor;
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
    // Отражение выворачивает обход треугольников — без этого видны изнанки.
    dev.SetCullMode(sage::rhi::CullMode::Front);
    draw(mirrorView, mirrorProj);
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
