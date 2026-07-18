#include "sage/ecs/RenderBatch.h"

#include <algorithm>

#include "sage/ecs/RenderSystem.h"
#include "sage/render/Frustum.h"
#include "sage/render/LightingUpload.h"
#include "sage/render/Shader.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Components.h"
#include "sage/scene/Light.h"
#include "sage/scene/Scene.h"

using namespace sage::render;

namespace sage::ecs {

// ============================================================================
//  Встроенные инстансные шейдеры (лениво, не уничтожаются — как скиннинг-шейдер).
//  Вершинная стадия читает модельную матрицу и цвет из per-instance атрибутов
//  (loc 3..7). Фрагментная — та же модель освещения, что и статический lit.frag.
// ============================================================================
namespace {

const char* kLitVert = R"(#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec4 iM0;
layout (location = 4) in vec4 iM1;
layout (location = 5) in vec4 iM2;
layout (location = 6) in vec4 iM3;
layout (location = 7) in vec3 iColor;

out vec3 FragPos;
out vec3 Normal;
out vec3 vColor;
out vec4 FragPosLightSpace;

uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uLightSpace;

void main() {
    mat4 model = mat4(iM0, iM1, iM2, iM3);
    vec4 world = model * vec4(aPos, 1.0);
    FragPos = world.xyz;
    Normal = mat3(model) * aNormal;
    vColor = iColor;
    FragPosLightSpace = uLightSpace * world;
    gl_Position = uProjection * uView * world;
}
)";

// Фрагмент — копия lit.frag (цвет берётся из per-instance vColor, без текстур).
const char* kLitFrag = R"(#version 330 core
in vec3 FragPos;
in vec3 Normal;
in vec3 vColor;
in vec4 FragPosLightSpace;
out vec4 FragColor;

#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS 8
struct PointLight { vec3 position; vec3 color; float intensity; float constant; float linear; float quadratic; };
struct SpotLight { vec3 position; vec3 direction; vec3 color; float intensity; float constant; float linear; float quadratic; float cosInner; float cosOuter; };

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
uniform sampler2D uShadowMap;
uniform bool uShadowsEnabled;
uniform int uShadingMode;
uniform bool uFogEnabled;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;

float CalcSunShadow(vec4 fragPosLightSpace, vec3 normal, vec3 sunDir) {
    vec3 proj = fragPosLightSpace.xyz / fragPosLightSpace.w;
    proj = proj * 0.5 + 0.5;
    if (proj.z > 1.0) return 0.0;
    float currentDepth = proj.z;
    float bias = max(0.0025 * (1.0 - dot(normal, sunDir)), 0.0008);
    float shadow = 0.0;
    vec2 texelSize = 1.0 / textureSize(uShadowMap, 0);
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y) {
            float pcfDepth = texture(uShadowMap, proj.xy + vec2(x, y) * texelSize).r;
            shadow += (currentDepth - bias > pcfDepth) ? 1.0 : 0.0;
        }
    return shadow / 9.0;
}
vec3 CalcHemisphereAmbient(vec3 normal) {
    float skyWeight = normal.y * 0.5 + 0.5;
    return mix(uAmbientGround, uAmbientSky, skyWeight) * uAmbientStrength;
}
vec3 CalcPointLight(PointLight light, vec3 normal, vec3 fragPos, vec3 viewDir) {
    vec3 toLight = light.position - fragPos;
    float dist = length(toLight);
    vec3 lightDir = toLight / max(dist, 0.0001);
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);
    float attenuation = 1.0 / (light.constant + light.linear * dist + light.quadratic * dist * dist);
    return (diff * light.color * light.intensity + 0.5 * spec * light.color * light.intensity) * attenuation;
}
vec3 CalcSpotLight(SpotLight light, vec3 normal, vec3 fragPos, vec3 viewDir) {
    vec3 toLight = light.position - fragPos;
    float dist = length(toLight);
    vec3 lightDir = toLight / max(dist, 0.0001);
    float theta = dot(lightDir, normalize(-light.direction));
    float epsilon = max(light.cosInner - light.cosOuter, 0.0001);
    float cone = clamp((theta - light.cosOuter) / epsilon, 0.0, 1.0);
    if (cone <= 0.0) return vec3(0.0);
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);
    float attenuation = 1.0 / (light.constant + light.linear * dist + light.quadratic * dist * dist);
    return (diff * light.color * light.intensity + 0.5 * spec * light.color * light.intensity) * attenuation * cone;
}

void main() {
    vec3 baseColor = vColor;
    vec3 norm = normalize(Normal);
    if (uShadingMode == 2) { FragColor = vec4(norm * 0.5 + 0.5, 1.0); return; }
    if (uShadingMode == 1) { FragColor = vec4(baseColor, 1.0); return; }

    vec3 viewDir = normalize(uViewPos - FragPos);
    vec3 ambient = CalcHemisphereAmbient(norm);

    vec3 sunDir = normalize(-uSunDir);
    float sunDiff = max(dot(norm, sunDir), 0.0);
    vec3 sunReflect = reflect(-sunDir, norm);
    float sunSpec = pow(max(dot(viewDir, sunReflect), 0.0), 32.0);
    vec3 sunLight = (sunDiff + 0.5 * sunSpec) * uSunColor * uSunIntensity;
    float shadow = uShadowsEnabled ? CalcSunShadow(FragPosLightSpace, norm, sunDir) : 0.0;
    sunLight *= (1.0 - shadow);

    vec3 pointLight = vec3(0.0);
    for (int i = 0; i < uNumPointLights; ++i)
        pointLight += CalcPointLight(uPointLights[i], norm, FragPos, viewDir);
    vec3 spotLight = vec3(0.0);
    for (int i = 0; i < uNumSpotLights; ++i)
        spotLight += CalcSpotLight(uSpotLights[i], norm, FragPos, viewDir);

    vec3 result = (ambient + sunLight + pointLight + spotLight) * baseColor;
    if (uFogEnabled) {
        float dist = length(uViewPos - FragPos);
        float f = clamp((uFogEnd - dist) / max(uFogEnd - uFogStart, 0.0001), 0.0, 1.0);
        result = mix(uFogColor, result, f);
    }
    FragColor = vec4(result, 1.0);
}
)";

const char* kDepthVert = R"(#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 3) in vec4 iM0;
layout (location = 4) in vec4 iM1;
layout (location = 5) in vec4 iM2;
layout (location = 6) in vec4 iM3;
uniform mat4 uLightSpace;
void main() {
    mat4 model = mat4(iM0, iM1, iM2, iM3);
    gl_Position = uLightSpace * model * vec4(aPos, 1.0);
}
)";

const char* kDepthFrag = R"(#version 330 core
void main() {}
)";

Shader& LitShader() {
    static Shader* s = new Shader(Shader::FromSource(kLitVert, kLitFrag, "RenderBatchLit"));
    return *s;
}
Shader& DepthShader() {
    static Shader* s = new Shader(Shader::FromSource(kDepthVert, kDepthFrag, "RenderBatchDepth"));
    return *s;
}

} // namespace

void RenderBatch::CollectVisible(Scene& scene, const glm::mat4& cullMatrix) {
    for (auto& kv : m_groups) kv.second.clear(); // переиспользуем ёмкость векторов
    Frustum frustum = Frustum::FromViewProj(cullMatrix);

    ForEachRenderable(scene, [&](Transform& tr, MeshRendererComponent& mr) {
        ++m_stats.Total;
        Mesh* mesh = mr.MeshPtr.get();
        glm::mat4 model = tr.GetMatrix();

        // Мировая ограничивающая сфера меша: центр*model, радиус*max|scale|.
        glm::vec3 center = glm::vec3(model * glm::vec4(mesh->BoundsCenter(), 1.0f));
        glm::vec3 s = glm::abs(tr.Scale);
        float radius = mesh->BoundsRadius() * glm::max(s.x, glm::max(s.y, s.z));
        if (!frustum.IntersectsSphere(center, radius)) { ++m_stats.Culled; return; }

        ++m_stats.Drawn;
        m_groups[mesh].push_back({model, EffectiveColor(mr)});
    });
}

RenderStats RenderBatch::RenderColor(Scene& scene, const glm::mat4& view, const glm::mat4& proj,
                                     const glm::vec3& viewPos, const LightingEnvironment& env,
                                     const glm::mat4& lightMatrix, unsigned int shadowMap,
                                     bool shadowsEnabled, int shadingMode) {
    m_stats = {};
    CollectVisible(scene, proj * view);

    Shader& sh = LitShader();
    sh.Use();
    sh.SetMat4("uView", view);
    sh.SetMat4("uProjection", proj);
    sh.SetVec3("uViewPos", viewPos);
    sh.SetInt("uShadingMode", shadingMode);
    UploadLighting(sh, env);
    if (shadowsEnabled && shadowMap)
        sage::rhi::GraphicsDevice::Get().BindTexture2D(1, shadowMap);
    UploadShadowUniforms(sh, lightMatrix, /*unit=*/1, shadowsEnabled);

    for (auto& kv : m_groups) {
        if (kv.second.empty()) continue;
        kv.first->SetInstances(kv.second.data(), kv.second.size());
        kv.first->DrawInstances(kv.second.size());
        ++m_stats.Batches;
    }
    return m_stats;
}

void RenderBatch::RenderDepth(Scene& scene, const glm::mat4& lightMatrix) {
    m_stats = {};
    CollectVisible(scene, lightMatrix); // отсечение по фрустуму света

    Shader& sh = DepthShader();
    sh.Use();
    sh.SetMat4("uLightSpace", lightMatrix);
    for (auto& kv : m_groups) {
        if (kv.second.empty()) continue;
        kv.first->SetInstances(kv.second.data(), kv.second.size());
        kv.first->DrawInstances(kv.second.size());
    }
}

} // namespace sage::ecs
