#include "sage/ecs/RenderBatch.h"

#include <algorithm>
#include <string>

#include "sage/core/JobSystem.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/render/Frustum.h"
#include "sage/render/LightingUpload.h"
#include "sage/render/Material.h"
#include "sage/render/PbrShader.h"
#include "sage/render/Shader.h"
#include "sage/render/Texture.h"
#include "sage/gi/GI.h"
#include "sage/gi/GIUpload.h"
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
layout (location = 11) in vec2 aUV2;

out vec3 FragPos;
out vec3 Normal;
out vec3 vColor;
out vec4 FragPosLightSpace;
out float vMetallic;
out float vRoughness;
out vec2 vUV2;

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
    vUV2 = aUV2;
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
in vec2 vUV2;
out vec4 FragColor;

// Лайтмапа GI текущей группы (uLightmapEnabled — группа запечена).
uniform bool uLightmapEnabled;
uniform sampler2D uLightmap;
)") + kPbrSharedGlsl + R"(
void main() {
    vec3 N = normalize(Normal);
    if (uShadingMode == 2) { FragColor = vec4(N * 0.5 + 0.5, 1.0); return; }
    if (uShadingMode == 1) { FragColor = vec4(vColor, 1.0); return; }
    // Непрямой свет: лайтмапа (статика) или GI-объём/полусфера (DefaultIndirect).
    vec3 indirect = uLightmapEnabled ? texture(uLightmap, vUV2).rgb
                                     : DefaultIndirect(FragPos, N);
    FragColor = vec4(ShadePBRgi(N, FragPos, FragPosLightSpace, vColor, vMetallic, vRoughness, 1.0, indirect), 1.0);
}
)";
}

// --- Текстурный PBR-путь: albedo/normal-карты + TBN (нормал-маппинг) ---
const char* kTexVert = R"(#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec4 aTangent;
layout (location = 11) in vec2 aUV2;

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoords;
out vec4 FragPosLightSpace;
out mat3 TBN;
out vec2 vUV2;

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
    vUV2 = aUV2;
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
in vec2 vUV2;
out vec4 FragColor;

// Лайтмапа GI текущей сущности (uLightmapEnabled — сущность запечена).
uniform bool uLightmapEnabled;
uniform sampler2D uLightmap;

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
    vec3 indirect = uLightmapEnabled ? texture(uLightmap, vUV2).rgb
                                     : DefaultIndirect(FragPos, N);
    FragColor = vec4(ShadePBRgi(N, FragPos, FragPosLightSpace, albedo, metallic, rough, ao, indirect), 1.0);
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
    for (auto& kv : m_groups) kv.second.Instances.clear(); // переиспользуем ёмкость
    m_textured.clear();
    m_cull.clear();
    Frustum frustum = Frustum::FromViewProj(cullMatrix);

    // Все мировые матрицы кадра одним O(n)-проходом (мемоизация общих
    // родительских цепочек) — вместо рекурсивного WorldMatrix per-entity.
    // Иерархия задаёт зависимость родитель→ребёнок, поэтому этот проход —
    // последовательный (дёшев: O(n) без тяжёлой математики).
    scene.ComputeWorldMatrices(m_worldCache);

    // Запечённое GI сцены: у лайтмапнутых сущностей вместо общего меша —
    // персональный меш с лайтмап-UV (создаётся лениво: GL-контекст здесь есть,
    // CollectVisible зовётся только из рендера).
    sage::gi::GIState* gi = scene.GI && scene.GI->Baked ? scene.GI.get() : nullptr;

    // 1) ПОСЛЕДОВАТЕЛЬНЫЙ сбор: итерация реестра (не потокобезопасна на
    //    структурные правки) в плоский список — только указатели/матрицы/цвет.
    ForEachRenderableEntity(scene, [&](entt::entity e, Transform&, MeshRendererComponent& mr) {
        auto wit = m_worldCache.find(e);
        glm::mat4 model = wit != m_worldCache.end() ? wit->second : scene.WorldMatrix(e);
        const Material* mat = mr.MaterialPtr.get();
        Mesh* mesh = mr.MeshPtr.get();
        int lmPage = -1;
        if (gi) {
            if (const auto* idc = scene.Registry().try_get<IdComponent>(e)) {
                auto it = gi->Entities.find(idc->Id);
                if (it != gi->Entities.end()) {
                    if (Mesh* baked = sage::gi::GetOrCreateGpuMesh(it->second)) {
                        mesh = baked;
                        lmPage = it->second.Page;
                    }
                }
            }
        }
        m_cull.push_back(CullItem{mesh, model, mat, EffectiveColor(mr), lmPage,
                                  mat && mat->HasMaps(), /*visible*/ false});
    });

    // 2) ПАРАЛЛЕЛЬНОЕ отсечение: каждый элемент независим — поток пишет только
    //    свой Visible, читает общий frustum/bounds меша лишь на чтение. Гонок нет.
    //    Это горячая математика кадра (матрица·вектор, длины столбцов, 6 плоскостей)
    //    — она и масштабируется по ядрам. При 1 ядре/выключенном MT — фолбэк.
    sage::JobSystem::Get().ParallelFor(m_cull.size(), [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            CullItem& c = m_cull[i];
            glm::vec3 center = glm::vec3(c.Model * glm::vec4(c.Mesh_->BoundsCenter(), 1.0f));
            // Масштаб для радиуса — из столбцов мировой матрицы (учитывает масштаб родителей).
            float sx = glm::length(glm::vec3(c.Model[0]));
            float sy = glm::length(glm::vec3(c.Model[1]));
            float sz = glm::length(glm::vec3(c.Model[2]));
            float radius = c.Mesh_->BoundsRadius() * glm::max(sx, glm::max(sy, sz));
            c.Visible = frustum.IntersectsSphere(center, radius);
        }
    });

    // 3) ПОСЛЕДОВАТЕЛЬНОЕ слияние в бакеты — в исходном порядке сбора, так что
    //    результат кадра детерминирован и совпадает с однопоточным.
    m_stats.Total = (int)m_cull.size();
    for (CullItem& c : m_cull) {
        if (!c.Visible) { ++m_stats.Culled; continue; }
        ++m_stats.Drawn;
        if (c.Textured) {
            // Есть текстурные карты — индивидуальный текстурный PBR-путь.
            m_textured.push_back({c.Mesh_, c.Model, c.Mat, c.LmPage});
        } else {
            // Плоский цвет — быстрый инстансный путь. Metallic/roughness из
            // материала (если назначен), иначе дефолты MeshInstance.
            MeshInstance inst;
            inst.Model = c.Model;
            inst.Color = c.Color;
            if (c.Mat) { inst.Metallic = c.Mat->Metallic; inst.Roughness = c.Mat->Roughness; }
            Group& g = m_groups[c.Mesh_];
            g.Instances.push_back(inst);
            g.LmPage = c.LmPage; // у запечённой статики меш уникален — страница одна
        }
    }
}

RenderStats RenderBatch::RenderColor(Scene& scene, const glm::mat4& view, const glm::mat4& proj,
                                     const glm::vec3& viewPos, const LightingEnvironment& env,
                                     const glm::mat4& lightMatrix, unsigned int shadowMap,
                                     bool shadowsEnabled, int shadingMode) {
    m_stats = {};
    CollectVisible(scene, proj * view);
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();

    sage::gi::GIState* gi = scene.GI && scene.GI->Baked ? scene.GI.get() : nullptr;
    sage::gi::GIState* giVolume = scene.GI ? scene.GI.get() : nullptr; // объём проб живёт и без лайтмап

    auto setupCommon = [&](Shader& sh) {
        sh.Use();
        sh.SetMat4("uView", view);
        sh.SetMat4("uProjection", proj);
        sh.SetVec3("uViewPos", viewPos);
        sh.SetInt("uShadingMode", shadingMode);
        UploadLighting(sh, env);
        // GI-объём проб (или выключение) + юниты сэмплеров GI (обязательны
        // всегда — см. SetGISamplerUnits).
        sage::gi::UploadGIVolume(sh, giVolume);
        if (shadowsEnabled && shadowMap) device.BindTexture2D(1, shadowMap);
        UploadShadowUniforms(sh, lightMatrix, /*unit=*/1, shadowsEnabled);
    };

    // Привязка лайтмапы группы/сущности (или выключение для незапечённых).
    auto bindLightmap = [&](Shader& sh, int lmPage) {
        if (gi && lmPage >= 0 && lmPage < (int)gi->Pages.size()) {
            sage::gi::EnsureLightmapGpu(gi->Pages[lmPage]);
            if (gi->Pages[lmPage].Gpu) {
                gi->Pages[lmPage].Gpu->Bind(sage::gi::kLightmapUnit);
                sh.SetInt("uLightmapEnabled", 1);
                return;
            }
        }
        sh.SetInt("uLightmapEnabled", 0);
    };

    // 1. Flat-инстансный проход.
    Shader& lit = LitShader();
    setupCommon(lit);
    for (auto& kv : m_groups) {
        if (kv.second.Instances.empty()) continue;
        bindLightmap(lit, kv.second.LmPage);
        kv.first->SetInstances(kv.second.Instances.data(), kv.second.Instances.size());
        kv.first->DrawInstances(kv.second.Instances.size());
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
            bindLightmap(tex, it.LmPage);
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
        if (kv.second.Instances.empty()) continue;
        kv.first->SetInstances(kv.second.Instances.data(), kv.second.Instances.size());
        kv.first->DrawInstances(kv.second.Instances.size());
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
