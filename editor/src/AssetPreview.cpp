#include "AssetPreview.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/core/Application.h"
#include "sage/ecs/RenderBatch.h"
#include "sage/render/Material.h"
#include "sage/render/Mesh.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/ScenePasses.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"

namespace {

// Студийный свет превью — ФИКСИРОВАННЫЙ, а не из текущей сцены.
//
// Так и надо: материал настраивают, чтобы он выглядел правильно ВООБЩЕ, а не
// при том освещении, которое сейчас в уровне. Судить о нём под чужим закатным
// солнцем — значит перекрасить его, а потом обнаружить, что днём он серый.
LightingEnvironment StudioLight() {
    LightingEnvironment env;
    env.Sun.Direction = glm::normalize(glm::vec3(-0.4f, -0.7f, -0.55f));
    env.Sun.Color = glm::vec3(1.0f, 0.97f, 0.92f);
    env.Sun.Intensity = 2.6f;
    env.AmbientStrength = 0.55f;
    // Полусферический ambient: холодное небо сверху, тёплая земля снизу. На
    // плоском сером фоне металл и диэлектрик выглядят почти одинаково — вся
    // разница между ними в том, ЧТО они отражают.
    env.SkyColor = glm::vec3(0.45f, 0.58f, 0.78f);
    env.GroundColor = glm::vec3(0.24f, 0.20f, 0.17f);
    env.Fog.Enabled = false;
    // Небо превью включено: оно и есть то, что отражает металл.
    env.Skybox.Enabled = true;
    env.Skybox.TopColor = glm::vec3(0.20f, 0.34f, 0.62f);
    env.Skybox.HorizonColor = glm::vec3(0.78f, 0.80f, 0.86f);
    env.Skybox.Intensity = 1.0f;
    return env;
}

// Габаритный радиус меша — чтобы вписать модель в кадр независимо от её
// размера. Модель на десять метров и модель на десять сантиметров должны
// смотреться в превью одинаково: превью отвечает на вопрос «как выглядит», а
// не «насколько большая».
float BoundingRadius(const Mesh& mesh) {
    const std::vector<Vertex>* verts = mesh.CpuVertices();
    if (!verts) return 1.0f;   // геометрия только на GPU — считать нечем
    float r = 0.0f;
    for (const Vertex& v : *verts) r = std::max(r, glm::length(v.Position));
    return r > 1e-4f ? r : 1.0f;
}

} // namespace

void AssetPreview::Init() {
    if (m_ready) return;
    m_sphere = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Sphere);
    if (!m_sky) m_sky.emplace();
    // Куб окружения снимается ОДИН раз: небо превью не меняется, и пересъёмка
    // его каждый кадр стоила бы шести проходов там, где нужен ноль.
    m_reflections.SetEnabled(true);
    m_reflections.UpdateSky(*m_sky, StudioLight());
    m_ready = m_sphere != nullptr;
}

void AssetPreview::Shutdown() {
    m_fbo.reset();
    m_sphere.reset();
    m_sky.reset();
    m_reflections = sage::render::ReflectionSystem{};
    m_ready = false;
}

void AssetPreview::Orbit(float dYaw, float dPitch) {
    m_yaw += dYaw;
    m_pitch = std::clamp(m_pitch + dPitch, -85.0f, 85.0f);
}

void AssetPreview::Zoom(float delta) {
    m_distance = std::clamp(m_distance * (delta > 0 ? 0.88f : 1.14f), 1.2f, 12.0f);
}

void AssetPreview::ResetView() {
    m_yaw = 35.0f;
    m_pitch = 20.0f;
    m_distance = 3.0f;
}

uint64_t AssetPreview::RenderMaterial(const std::shared_ptr<Material>& material, int size) {
    Init();
    if (!m_sphere) return 0;
    return Render(m_sphere, material, size, 1.0f);
}

uint64_t AssetPreview::RenderMesh(const std::shared_ptr<Mesh>& mesh, int size) {
    Init();
    if (!mesh) return 0;
    return Render(mesh, nullptr, size, BoundingRadius(*mesh));
}

uint64_t AssetPreview::Render(const std::shared_ptr<Mesh>& mesh,
                              const std::shared_ptr<Material>& material, int size,
                              float fitRadius) {
    if (!mesh) return 0;
    size = std::clamp(size, 32, 1024);
    if (!m_fbo) m_fbo.emplace(size, size);
    m_fbo->Resize(size, size);
    m_fbo->Bind();

    sage::rhi::GraphicsDevice& device = sage::Application::Get().Device();
    device.SetClearColor(0.13f, 0.14f, 0.17f, 1.0f);
    device.Clear();

    // Сцена превью строится заново каждый кадр и живёт один вызов. Это дёшево
    // (одна сущность) и избавляет от целого класса ошибок: превью не может
    // «залипнуть» на прошлом материале, потому что помнить ему нечем.
    Scene scene("preview");
    GameObject obj = scene.CreateObject("Preview");
    MeshRendererComponent& mr = obj.Renderer();
    mr.MeshPtr = mesh;
    mr.Ref.type = MeshRef::Type::Sphere;
    if (material) {
        mr.MaterialPtr = material;
        mr.MaterialPath = "preview";
    } else {
        mr.Color = glm::vec3(0.78f, 0.78f, 0.80f);
    }

    const float yaw = glm::radians(m_yaw);
    const float pitch = glm::radians(m_pitch);
    const float dist = m_distance * fitRadius;
    const glm::vec3 eye(dist * std::cos(pitch) * std::sin(yaw), dist * std::sin(pitch),
                        dist * std::cos(pitch) * std::cos(yaw));
    const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 proj =
        glm::perspective(glm::radians(35.0f), 1.0f, 0.05f, dist * 8.0f + 10.0f);

    const LightingEnvironment env = StudioLight();
    sage::render::SceneColorInput input;
    input.View = view;
    input.Proj = proj;
    input.ViewPos = eye;
    input.Env = &env;
    // Ни теней, ни отсечения перекрытием: один объект в пустоте, и тень ему не
    // на что отбрасывать. Проход теней здесь стоил бы столько же, сколько само
    // превью, и не давал бы ничего.
    input.Shadows = ShadowBinding();
    input.ShadingMode = 0;
    input.OcclusionCulling = false;
    input.Time = 0.0f;   // анимированные материалы в превью стоят на месте
    input.Reflection = m_reflections.Binding(size, size);

    sage::render::RenderSceneColor(scene, m_batch, input);

    device.BindDefaultFramebuffer();
    return m_fbo->NativeColorTexture();
}
