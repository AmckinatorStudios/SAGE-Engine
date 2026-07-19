#include "sage/ecs/RenderBatch.h"

#include <algorithm>
#include <string>

#include "sage/ecs/RenderSystem.h"
#include "sage/render/Frustum.h"
#include "sage/render/LightingUpload.h"
#include "sage/render/Material.h"
#include "sage/render/PbrShader.h"
#include "sage/render/Shader.h"
#include "sage/render/Texture.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Components.h"
#include "sage/scene/Light.h"
#include "sage/scene/Scene.h"

using namespace sage::render;

namespace sage::ecs {

// ============================================================================
//  Встроенные PBR-шейдеры (лениво, не уничтожаются — как скиннинг-шейдер).
//  Освещение — общий блок kPbrSharedGlsl (Cook-Torrance), одинаковый для
//  инстансного (flat) и текстурного (normal-mapped) путей.
// ============================================================================
namespace {

// --- Инстансный flat-путь: цвет/metallic/roughness из per-instance атрибутов ---
const char* kLitVert = R"(#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec4 aTangent;
layout (location = 4) in vec4 iM0;
layout (location = 5) in vec4 iM1;
layout (location = 6) in vec4 iM2;
layout (location = 7) in vec4 iM3;
layout (location = 8) in vec3 iColor;
layout (location = 9) in float iMetallic;
layout (location = 10) in float iRoughness;

out vec3 FragPos;
out vec3 Normal;
out vec3 vColor;
out vec4 FragPosLightSpace;
out float vMetallic;
out float vRoughness;

uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uLightSpace;

void main() {
    mat4 model = mat4(iM0, iM1, iM2, iM3);
    vec4 world = model * vec4(aPos, 1.0);
    FragPos = world.xyz;
    // Нормальная матрица = обратно-транспонированная mat3(model): корректные
    // нормали при неравномерном/отрицательном масштабе (иначе перекос/инверсия).
    Normal = transpose(inverse(mat3(model))) * aNormal;
    vColor = iColor;
    vMetallic = iMetallic;
    vRoughness = iRoughness;
    FragPosLightSpace = uLightSpace * world;
    gl_Position = uProjection * uView * world;
}
)";

std::string LitFragSource() {
    return std::string(R"(#version 330 core
in vec3 FragPos;
in vec3 Normal;
in vec3 vColor;
in vec4 FragPosLightSpace;
in float vMetallic;
in float vRoughness;
out vec4 FragColor;
)") + kPbrSharedGlsl + R"(
void main() {
    vec3 N = normalize(Normal);
    if (uShadingMode == 2) { FragColor = vec4(N * 0.5 + 0.5, 1.0); return; }
    if (uShadingMode == 1) { FragColor = vec4(vColor, 1.0); return; }
    FragColor = vec4(ShadePBR(N, FragPos, FragPosLightSpace, vColor, vMetallic, vRoughness), 1.0);
}
)";
}

// --- Текстурный PBR-путь: albedo/normal-карты + TBN (нормал-маппинг) ---
const char* kTexVert = R"(#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec4 aTangent;

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoords;
out vec4 FragPosLightSpace;
out mat3 TBN;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uLightSpace;

void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    FragPos = world.xyz;
    // Нормальная матрица (обратно-транспонированная) — верные N/T при любом масштабе.
    mat3 nm = transpose(inverse(mat3(uModel)));
    vec3 N = normalize(nm * aNormal);
    vec3 T = normalize(nm * aTangent.xyz);
    T = normalize(T - N * dot(N, T));       // Gram-Schmidt в мировом пространстве
    vec3 B = cross(N, T) * aTangent.w;      // знак ориентации (handedness) UV-развёртки
    TBN = mat3(T, B, N);
    Normal = N;
    TexCoords = aUV;
    FragPosLightSpace = uLightSpace * world;
    gl_Position = uProjection * uView * world;
}
)";

std::string TexFragSource() {
    return std::string(R"(#version 330 core
in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoords;
in vec4 FragPosLightSpace;
in mat3 TBN;
out vec4 FragColor;

uniform vec3 uAlbedoFactor;
uniform float uMetallic;
uniform float uRoughness;
uniform sampler2D uAlbedoMap;
uniform bool uHasAlbedo;
uniform sampler2D uNormalMap;
uniform bool uHasNormal;
uniform sampler2D uMetallicMap;
uniform bool uHasMetallic;
uniform sampler2D uRoughnessMap;
uniform bool uHasRoughness;
uniform sampler2D uAOMap;
uniform bool uHasAO;
)") + kPbrSharedGlsl + R"(
void main() {
    vec3 albedo = uAlbedoFactor;
    if (uHasAlbedo) albedo *= texture(uAlbedoMap, TexCoords).rgb;

    vec3 N = normalize(Normal);
    if (uHasNormal) {
        vec3 n = texture(uNormalMap, TexCoords).rgb * 2.0 - 1.0;
        N = normalize(TBN * n);
    }
    // metallic/roughness/ao — из карт (R-канал) × фактор, иначе только фактор.
    float metallic = uMetallic;
    if (uHasMetallic) metallic *= texture(uMetallicMap, TexCoords).r;
    float rough = uRoughness;
    if (uHasRoughness) rough *= texture(uRoughnessMap, TexCoords).r;
    float ao = uHasAO ? texture(uAOMap, TexCoords).r : 1.0;

    if (uShadingMode == 2) { FragColor = vec4(N * 0.5 + 0.5, 1.0); return; }
    if (uShadingMode == 1) { FragColor = vec4(albedo, 1.0); return; }
    FragColor = vec4(ShadePBRao(N, FragPos, FragPosLightSpace, albedo, metallic, rough, ao), 1.0);
}
)";
}

// --- Depth-шейдеры для карты теней (инстансный и uModel — для текстурных) ---
const char* kDepthInstVert = R"(#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 4) in vec4 iM0;
layout (location = 5) in vec4 iM1;
layout (location = 6) in vec4 iM2;
layout (location = 7) in vec4 iM3;
uniform mat4 uLightSpace;
void main() { gl_Position = uLightSpace * mat4(iM0, iM1, iM2, iM3) * vec4(aPos, 1.0); }
)";
const char* kDepthUModelVert = R"(#version 330 core
layout (location = 0) in vec3 aPos;
uniform mat4 uLightSpace;
uniform mat4 uModel;
void main() { gl_Position = uLightSpace * uModel * vec4(aPos, 1.0); }
)";
const char* kDepthFrag = R"(#version 330 core
void main() {}
)";

Shader& LitShader() { static Shader* s = new Shader(Shader::FromSource(kLitVert, LitFragSource(), "RenderBatchLit")); return *s; }
Shader& TexShader() { static Shader* s = new Shader(Shader::FromSource(kTexVert, TexFragSource(), "RenderBatchTex")); return *s; }
Shader& DepthInstShader() { static Shader* s = new Shader(Shader::FromSource(kDepthInstVert, kDepthFrag, "RenderBatchDepthInst")); return *s; }
Shader& DepthUModelShader() { static Shader* s = new Shader(Shader::FromSource(kDepthUModelVert, kDepthFrag, "RenderBatchDepthU")); return *s; }

} // namespace

void RenderBatch::CollectVisible(Scene& scene, const glm::mat4& cullMatrix) {
    for (auto& kv : m_groups) kv.second.clear(); // переиспользуем ёмкость
    m_textured.clear();
    Frustum frustum = Frustum::FromViewProj(cullMatrix);

    // Все мировые матрицы кадра одним O(n)-проходом (мемоизация общих
    // родительских цепочек) — вместо рекурсивного WorldMatrix per-entity.
    scene.ComputeWorldMatrices(m_worldCache);

    ForEachRenderableEntity(scene, [&](entt::entity e, Transform&, MeshRendererComponent& mr) {
        ++m_stats.Total;
        Mesh* mesh = mr.MeshPtr.get();
        auto wit = m_worldCache.find(e);
        glm::mat4 model = wit != m_worldCache.end() ? wit->second : scene.WorldMatrix(e);

        glm::vec3 center = glm::vec3(model * glm::vec4(mesh->BoundsCenter(), 1.0f));
        // Масштаб для радиуса — из столбцов мировой матрицы (учитывает масштаб родителей).
        float sx = glm::length(glm::vec3(model[0])), sy = glm::length(glm::vec3(model[1])), sz = glm::length(glm::vec3(model[2]));
        float radius = mesh->BoundsRadius() * glm::max(sx, glm::max(sy, sz));
        if (!frustum.IntersectsSphere(center, radius)) { ++m_stats.Culled; return; }
        ++m_stats.Drawn;

        const Material* mat = mr.MaterialPtr.get();
        if (mat && mat->HasMaps()) {
            // Есть текстурные карты — индивидуальный текстурный PBR-путь.
            m_textured.push_back({mesh, model, mat});
        } else {
            // Плоский цвет — быстрый инстансный путь. Metallic/roughness из
            // материала (если назначен), иначе дефолты MeshInstance.
            MeshInstance inst;
            inst.Model = model;
            inst.Color = EffectiveColor(mr);
            if (mat) { inst.Metallic = mat->Metallic; inst.Roughness = mat->Roughness; }
            m_groups[mesh].push_back(inst);
        }
    });
}

RenderStats RenderBatch::RenderColor(Scene& scene, const glm::mat4& view, const glm::mat4& proj,
                                     const glm::vec3& viewPos, const LightingEnvironment& env,
                                     const glm::mat4& lightMatrix, unsigned int shadowMap,
                                     bool shadowsEnabled, int shadingMode) {
    m_stats = {};
    CollectVisible(scene, proj * view);
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();

    auto setupCommon = [&](Shader& sh) {
        sh.Use();
        sh.SetMat4("uView", view);
        sh.SetMat4("uProjection", proj);
        sh.SetVec3("uViewPos", viewPos);
        sh.SetInt("uShadingMode", shadingMode);
        UploadLighting(sh, env);
        if (shadowsEnabled && shadowMap) device.BindTexture2D(1, shadowMap);
        UploadShadowUniforms(sh, lightMatrix, /*unit=*/1, shadowsEnabled);
    };

    // 1. Flat-инстансный проход.
    Shader& lit = LitShader();
    setupCommon(lit);
    for (auto& kv : m_groups) {
        if (kv.second.empty()) continue;
        kv.first->SetInstances(kv.second.data(), kv.second.size());
        kv.first->DrawInstances(kv.second.size());
        ++m_stats.Batches;
    }

    // 2. Текстурный PBR-проход (albedo/normal-карты, нормал-маппинг).
    if (!m_textured.empty()) {
        Shader& tex = TexShader();
        setupCommon(tex);
        // Юниты текстур: albedo=0, shadow=1, normal=2, metallic=3, roughness=4, ao=5.
        tex.SetInt("uAlbedoMap", 0);
        tex.SetInt("uNormalMap", 2);
        tex.SetInt("uMetallicMap", 3);
        tex.SetInt("uRoughnessMap", 4);
        tex.SetInt("uAOMap", 5);
        for (const TexturedItem& it : m_textured) {
            tex.SetMat4("uModel", it.Model);
            tex.SetVec3("uAlbedoFactor", it.Mat->Albedo);
            tex.SetFloat("uMetallic", it.Mat->Metallic);
            tex.SetFloat("uRoughness", it.Mat->Roughness);
            tex.SetInt("uHasAlbedo", it.Mat->AlbedoTex ? 1 : 0);
            tex.SetInt("uHasNormal", it.Mat->NormalTex ? 1 : 0);
            tex.SetInt("uHasMetallic", it.Mat->MetallicTex ? 1 : 0);
            tex.SetInt("uHasRoughness", it.Mat->RoughnessTex ? 1 : 0);
            tex.SetInt("uHasAO", it.Mat->AOTex ? 1 : 0);
            if (it.Mat->AlbedoTex) it.Mat->AlbedoTex->Bind(0);
            if (it.Mat->NormalTex) it.Mat->NormalTex->Bind(2);
            if (it.Mat->MetallicTex) it.Mat->MetallicTex->Bind(3);
            if (it.Mat->RoughnessTex) it.Mat->RoughnessTex->Bind(4);
            if (it.Mat->AOTex) it.Mat->AOTex->Bind(5);
            it.Mesh_->Draw();
            ++m_stats.Batches;
        }
    }
    return m_stats;
}

void RenderBatch::RenderDepth(Scene& scene, const glm::mat4& lightMatrix) {
    m_stats = {};
    CollectVisible(scene, lightMatrix); // отсечение по фрустуму света

    // Flat — инстансно.
    Shader& di = DepthInstShader();
    di.Use();
    di.SetMat4("uLightSpace", lightMatrix);
    for (auto& kv : m_groups) {
        if (kv.second.empty()) continue;
        kv.first->SetInstances(kv.second.data(), kv.second.size());
        kv.first->DrawInstances(kv.second.size());
    }
    // Текстурные — индивидуально (uModel).
    if (!m_textured.empty()) {
        Shader& du = DepthUModelShader();
        du.Use();
        du.SetMat4("uLightSpace", lightMatrix);
        for (const TexturedItem& it : m_textured) {
            du.SetMat4("uModel", it.Model);
            it.Mesh_->Draw();
        }
    }
}

} // namespace sage::ecs
