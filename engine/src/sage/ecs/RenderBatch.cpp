#include "sage/ecs/RenderBatch.h"
#include "sage/core/Profiler.h"

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
layout (location = 14) in vec3 iEmissive;
layout (location = 11) in vec2 aUV2;
layout (location = 12) in float iAlpha;
layout (location = 13) in float iPlanar;

out vec3 FragPos;
out vec3 Normal;
out vec3 vColor;
out vec3 vEmissive;
out float vMetallic;
out float vRoughness;
out vec2 vUV2;
out float vAlpha;
out float vPlanar;

uniform mat4 uView;
uniform mat4 uProjection;

void main() {
    mat4 model = mat4(iM0, iM1, iM2, iM3);
    vec4 world = model * vec4(aPos, 1.0);
    FragPos = world.xyz;
    // Нормальная матрица = обратно-транспонированная mat3(model): корректные
    // нормали при неравномерном/отрицательном масштабе (иначе перекос/инверсия).
    Normal = transpose(inverse(mat3(model))) * aNormal;
    vColor = iColor;
    vEmissive = iEmissive;
    vMetallic = iMetallic;
    vRoughness = iRoughness;
    vUV2 = aUV2;
    vAlpha = iAlpha;
    vPlanar = iPlanar;
    gl_Position = uProjection * uView * world;
}
)";

std::string LitFragSource() {
    return std::string(R"(#version 330 core
in vec3 FragPos;
in vec3 Normal;
in vec3 vColor;
in vec3 vEmissive;
in float vMetallic;
in float vRoughness;
in vec2 vUV2;
in float vAlpha;
in float vPlanar;
out vec4 FragColor;

// Лайтмапа GI текущей группы (uLightmapEnabled — группа запечена).
uniform bool uLightmapEnabled;
uniform sampler2D uLightmap;
)") + kPbrSharedGlsl + R"(
void main() {
    vec3 N = normalize(Normal);
    if (uShadingMode != 0) {
        vec4 dbg;
        if (DebugShade(uShadingMode, N, FragPos, vColor, vMetallic, vRoughness, 1.0, vEmissive,
                       CalcSunShadow(FragPos, N, normalize(-uSunDir)), dbg)) {
            FragColor = vec4(dbg.rgb, vAlpha);
            return;
        }
    }
    // Непрямой свет: лайтмапа (статика) или GI-объём/полусфера (DefaultIndirect).
    vec3 indirect = uLightmapEnabled ? texture(uLightmap, vUV2).rgb
                                     : DefaultIndirect(FragPos, N);
    // Свечение ДОБАВЛЯЕТСЯ к освещению, а не заменяет его: светящийся предмет
    // всё так же ловит тени и блики, просто светит и сам. Замена дала бы плоский
    // силуэт вместо объёма.
    FragColor = vec4(vEmissive + ShadePBRplanar(N, FragPos, vColor, vMetallic, vRoughness, 1.0, indirect,
                                    SamplePlanar(vec2(0.0)), vPlanar), vAlpha);
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
out mat3 TBN;
out vec2 vUV2;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

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
    gl_Position = uProjection * uView * world;
}
)";

std::string TexFragSource() {
    return std::string(R"(#version 330 core
in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoords;
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
uniform float uOpacity;
uniform vec3 uEmissive;
uniform sampler2D uEmissiveMap;
uniform bool uHasEmissive;
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

    if (uShadingMode != 0) {
        vec4 dbg;
        vec3 dbgEmissive = uEmissive;
        if (uHasEmissive) dbgEmissive *= texture(uEmissiveMap, TexCoords).rgb;
        if (DebugShade(uShadingMode, N, FragPos, albedo, metallic, rough, ao, dbgEmissive,
                       CalcSunShadow(FragPos, N, normalize(-uSunDir)), dbg)) {
            FragColor = vec4(dbg.rgb, uOpacity);
            return;
        }
    }
    vec3 indirect = uLightmapEnabled ? texture(uLightmap, vUV2).rgb
                                     : DefaultIndirect(FragPos, N);
    vec3 emissive = uEmissive;
    if (uHasEmissive) emissive *= texture(uEmissiveMap, TexCoords).rgb;
    FragColor = vec4(emissive + ShadePBRgi(N, FragPos, albedo, metallic, rough, ao, indirect),
                     uOpacity);
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

// Заливка пользовательских юниформ материала в программу. Тип берётся из
// самого параметра: залить vec4 туда, где объявлен float, — молча ничего не
// сделать, а разбираться потом с «шейдер не реагирует на настройку».
void UploadParams(Shader& sh, const std::unordered_map<std::string, ShaderParam>& params) {
    for (const auto& [name, p] : params) {
        switch (p.Kind) {
            case ShaderParam::Type::Float: sh.SetFloat(name, p.Value.x); break;
            case ShaderParam::Type::Vec2:  sh.SetVec2(name, glm::vec2(p.Value)); break;
            case ShaderParam::Type::Vec3:  sh.SetVec3(name, glm::vec3(p.Value)); break;
            case ShaderParam::Type::Vec4:  sh.SetVec4(name, p.Value); break;
        }
    }
}

Shader& LitShader() { static Shader* s = new Shader(Shader::FromSource(kLitVert, LitFragSource(), "RenderBatchLit")); return *s; }
Shader& TexShader() { static Shader* s = new Shader(Shader::FromSource(kTexVert, TexFragSource(), "RenderBatchTex")); return *s; }
Shader& DepthInstShader() { static Shader* s = new Shader(Shader::FromSource(kDepthInstVert, kDepthFrag, "RenderBatchDepthInst")); return *s; }
Shader& DepthUModelShader() { static Shader* s = new Shader(Shader::FromSource(kDepthUModelVert, kDepthFrag, "RenderBatchDepthU")); return *s; }

} // namespace

void RenderBatch::CollectVisible(Scene& scene, const glm::mat4& cullMatrix) {
    SAGE_PROFILE("Отсечение");
    for (auto& kv : m_groups) kv.second.Instances.clear(); // переиспользуем ёмкость
    m_textured.clear();
    m_transparent.clear();
    for (auto& kv : m_custom) { kv.second.Instances.clear(); kv.second.Entities.clear();
                                kv.second.Depths.clear();
                                kv.second.AnyOverrides = false; kv.second.Transparent = false; }
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
        const glm::mat4* cached = m_worldCache.Find(e);
        glm::mat4 model = cached ? *cached : scene.WorldMatrix(e);
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
        const sage::render::LodComponent* lod =
            scene.Registry().try_get<sage::render::LodComponent>(e);
        bool paramOverride = scene.Registry().all_of<ShaderParamsComponent>(e);
        m_cull.push_back(CullItem{mesh, model, &mr, paramOverride, lmPage,
                                  /*visible*/ false, e, lod, mesh});
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

            // Ограничивающую сферу запоминаем: её же берёт проход проверки
            // перекрытия, чтобы построить коробку-заменитель.
            c.Center = center;
            c.Radius = radius;

            // Уровень детализации считается ЗДЕСЬ ЖЕ, из той же сферы: она уже
            // построена, и второй проход по сцене ради неё был бы работой на
            // ровном месте. Для отсечённых не считаем — их всё равно не рисуем.
            c.Lod = 0;
            if (c.Visible && c.LodInfo) {
                const float screen =
                    sage::render::ProjectedScreenHeight(cullMatrix, center, radius);
                c.Lod = c.LodInfo->Levels.Select(screen);
                if (c.Lod < 0) {
                    c.Visible = false; // мельче порога — не рисуем вовсе
                } else {
                    c.Mesh_ = c.LodInfo->MeshFor(c.Lod, c.BaseMesh);
                }
            }
        }
    });

    // 3) ПОСЛЕДОВАТЕЛЬНОЕ слияние в бакеты — в исходном порядке сбора, так что
    //    результат кадра детерминирован и совпадает с однопоточным.
    m_stats.Total = (int)m_cull.size();
    m_stats.CulledTiny = 0;
    m_stats.Triangles = 0;
    m_stats.TrianglesAtLod0 = 0;
    m_stats.CulledOccluded = 0;
    // Ответы о перекрытии — ТАБЛИЦА ЭТОГО ВИДА, и берётся она один раз до цикла:
    // внутри это был бы лишний поиск по хэшу на каждую сущность кадра.
    //
    // В ПРОХОДЕ ТЕНЕЙ ответов нет вовсе. Там сбор идёт из точки СВЕТА, а ответ
    // получен из точки камеры: объект, спрятанный за стеной от зрителя,
    // остаётся освещённым солнцем и обязан отбрасывать тень — та вполне может
    // лежать на видимом месте. Пока проверка применялась и здесь, включённое
    // отсечение перекрытием молча выедало тени.
    const OcclusionTable* occlusion =
        (m_occlusionEnabled && !m_collectingShadows) ? &OcclusionSlots() : nullptr;
    for (CullItem& c : m_cull) {
        // Ответ прошлого кадра о перекрытии. Применяется ПОСЛЕ фрустума и LOD:
        // объект, уже отсечённый ими, проверять незачем.
        if (c.Visible && occlusion) {
            auto it = occlusion->find(c.Entity);
            if (it != occlusion->end() && !it->second.Visible) {
                c.Visible = false;
                c.Occluded = true;
            }
        }
        if (!c.Visible) {
            // Три вида отсечения считаются РАЗДЕЛЬНО: смешав их, нельзя понять,
            // что именно сработало — порог LOD, перекрытие или просто камера
            // смотрит в стену.
            if (c.Occluded) ++m_stats.CulledOccluded;
            else if (c.LodInfo && c.Lod < 0) ++m_stats.CulledTiny;
            else ++m_stats.Culled;
            continue;
        }
        ++m_stats.Drawn;
        m_stats.TrianglesAtLod0 += (long long)c.BaseMesh->TriangleCount();

        // РАЗБОР ПО ЧАСТЯМ. Дальше речь идёт не о сущности, а о подмешах её
        // меша: у каждого свой материал, а значит свой путь отрисовки (плоский,
        // текстурный, прозрачный, со своим шейдером) и своя пачка. Объект с
        // одним материалом даёт ровно один проход этого цикла — то есть ровно
        // то, что было до подмешей.
        const MeshRendererComponent& mr = *c.MR;
        // Предметы «на камере» (рука, оружие) в карту теней и в зеркало не
        // идут: см. MeshRendererComponent::CastShadows / InReflections.
        if (m_collectingShadows && !mr.CastShadows) continue;
        if (m_skipPlanarReflectors && !mr.InReflections) continue;
        const std::vector<sage::render::Submesh>& subs = c.Mesh_->Submeshes();
        for (unsigned int si = 0; si < (unsigned int)subs.size(); ++si) {
            const Material* mat = MaterialForSubmesh(mr, si);
            // Зеркало не отражает само себя (см. ReflectionBinding::CapturingPlanar).
            if (m_skipPlanarReflectors && mat && mat->Render.PlanarReflectivity > 0.0f) continue;

            const float opacity = EffectiveOpacity(mr, mat);
            const bool textured = mat && mat->HasMaps();
            Shader* custom = (mat && mat->ShaderPtr) ? mat->ShaderPtr.get() : nullptr;

            m_stats.Triangles += (long long)(subs[si].IndexCount / 3);

            MeshInstance inst;
            inst.Model = c.Model;
            inst.Color = EffectiveColor(mr, mat);
            inst.Alpha = opacity;
            inst.Emissive = EffectiveEmissive(mr, mat);
            if (mat) {
                inst.Metallic = mat->Metallic;
                inst.Roughness = mat->Roughness;
                inst.PlanarReflectivity = mat->Render.PlanarReflectivity;
            }

            if (custom) {
                // Собственный шейдер материала. Батчинг сохраняется: группируем
                // по тройке «часть меша + программа», и тысяча объектов со
                // своим шейдером остаётся одним draw call'ом — иначе своим
                // шейдером нельзя было бы рисовать воду или траву, то есть
                // именно то, ради чего он и нужен.
                CustomGroup& g = m_custom[CustomKey{c.Mesh_, si, custom}];
                g.Instances.push_back(inst);
                g.Entities.push_back(c.Entity);
                g.Depths.push_back(glm::dot(c.Center - m_viewPos, c.Center - m_viewPos));
                g.Mat = mat;
                g.LmPage = c.LmPage;
                if (c.HasParamOverride) g.AnyOverrides = true;
                // Свой шейдер НЕ отменяет прозрачность: материал воды с opacity
                // 0.8 обязан смешиваться, а не рисоваться поверх. Признак
                // группы — непрозрачность материала или любого её инстанса.
                if (opacity < 0.996f) g.Transparent = true;
            } else if (opacity < 0.996f) {
                // Полупрозрачный — в отдельный список, рисуется после всей
                // непрозрачной геометрии и в своём порядке (см. RenderColor).
                m_transparent.push_back({c.Mesh_, si, inst, mat, c.LmPage,
                                         glm::dot(c.Center - m_viewPos, c.Center - m_viewPos),
                                         textured});
            } else if (textured) {
                // Есть текстурные карты — индивидуальный текстурный PBR-путь.
                m_textured.push_back({c.Mesh_, si, c.Model, mat, c.LmPage, opacity,
                                      inst.Color, inst.Emissive});
            } else {
                // Плоский цвет — быстрый инстансный путь. Metallic/roughness из
                // материала (если назначен), иначе дефолты MeshInstance.
                Group& g = m_groups[MeshSlotKey{c.Mesh_, si}];
                g.Instances.push_back(inst);
                g.LmPage = c.LmPage; // у запечённой статики меш уникален — страница одна
            }
        }
    }
}

RenderStats RenderBatch::RenderColor(Scene& scene, const glm::mat4& view, const glm::mat4& proj,
                                     const glm::vec3& viewPos, const LightingEnvironment& env,
                                     const ShadowBinding& shadows, int shadingMode,
                                     const sage::render::ReflectionBinding* reflections) {
    m_stats = {};
    m_viewPos = viewPos; // нужна сборке: по ней считается глубина прозрачных
    m_skipPlanarReflectors = reflections && reflections->CapturingPlanar;
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
        BindAndUploadShadows(sh, shadows);
        // Отражения: пустая привязка тоже заливается — иначе включённый на
        // прошлом объекте куб остался бы висеть в программе шейдера.
        sage::render::UploadReflection(sh, reflections ? *reflections
                                                       : sage::render::ReflectionBinding{});
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

    // Отрисовка одной группы «меш + своя программа». Вынесена, потому что
    // непрозрачные группы рисуются до общего прозрачного прохода, а
    // полупрозрачные — внутри него, с блендингом и в своём порядке.
    auto drawCustom = [&](Shader& sh, Mesh& mesh, unsigned int submesh, CustomGroup& g) {
        setupCommon(sh);
        sh.SetFloat("uTime", m_time);
        bindLightmap(sh, g.LmPage);
        if (g.Mat) {
            sh.SetVec3("uAlbedoFactor", g.Mat->Albedo);
            sh.SetFloat("uMetallic", g.Mat->Metallic);
            sh.SetFloat("uRoughness", g.Mat->Roughness);
            sh.SetFloat("uOpacity", g.Mat->Opacity);
            UploadParams(sh, g.Mat->Params);
        }
        if (!g.AnyOverrides) {
            if (g.Transparent) {
                // Порядок смешивания решается ВНУТРИ пачки: сортируем инстансы
                // от дальних к ближним и рисуем всё тем же одним draw call'ом.
                m_customOrder.resize(g.Instances.size());
                for (unsigned int k = 0; k < m_customOrder.size(); ++k) m_customOrder[k] = k;
                std::sort(m_customOrder.begin(), m_customOrder.end(),
                          [&g](unsigned int a, unsigned int b) {
                              return g.Depths[a] > g.Depths[b];
                          });
                m_transparentBatch.clear();
                m_transparentBatch.reserve(g.Instances.size());
                for (unsigned int k : m_customOrder) m_transparentBatch.push_back(g.Instances[k]);
                mesh.SetInstances(m_transparentBatch.data(), m_transparentBatch.size());
                mesh.DrawSubmeshInstances(submesh, m_transparentBatch.size());
            } else {
                mesh.SetInstances(g.Instances.data(), g.Instances.size());
                mesh.DrawSubmeshInstances(submesh, g.Instances.size());
            }
            ++m_stats.Batches;
        } else {
            // Хоть у одной сущности группы есть свои значения юниформ — рисуем
            // группу поштучно. Иначе «персональный» параметр применился бы ко
            // всей пачке, то есть ровно к тем объектам, от которых его и
            // отделяли.
            for (size_t k = 0; k < g.Instances.size(); ++k) {
                if (const auto* ov =
                        scene.Registry().try_get<ShaderParamsComponent>(g.Entities[k])) {
                    UploadParams(sh, ov->Params);
                }
                mesh.SetInstances(&g.Instances[k], 1);
                mesh.DrawSubmeshInstances(submesh, 1);
                ++m_stats.Batches;
                if (g.Mat) UploadParams(sh, g.Mat->Params); // вернуть общие значения
            }
        }
    };

    // 1. Flat-инстансный проход.
    Shader& lit = LitShader();
    setupCommon(lit);
    for (auto& kv : m_groups) {
        if (kv.second.Instances.empty()) continue;
        bindLightmap(lit, kv.second.LmPage);
        kv.first.Mesh_->SetInstances(kv.second.Instances.data(), kv.second.Instances.size());
        kv.first.Mesh_->DrawSubmeshInstances(kv.first.Submesh, kv.second.Instances.size());
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
            // Свёрнутое значение, а не it.Mat->Albedo: тон экземпляра обязан
            // работать и на текстурном пути (см. TexturedItem в заголовке).
            tex.SetVec3("uAlbedoFactor", it.Color);
            tex.SetFloat("uMetallic", it.Mat->Metallic);
            tex.SetFloat("uRoughness", it.Mat->Roughness);
            tex.SetInt("uHasAlbedo", it.Mat->AlbedoTex ? 1 : 0);
            tex.SetInt("uHasNormal", it.Mat->NormalTex ? 1 : 0);
            tex.SetInt("uHasMetallic", it.Mat->MetallicTex ? 1 : 0);
            tex.SetInt("uHasRoughness", it.Mat->RoughnessTex ? 1 : 0);
            tex.SetInt("uHasAO", it.Mat->AOTex ? 1 : 0);
            tex.SetVec3("uEmissive", it.Emissive);
            tex.SetInt("uHasEmissive", it.Mat->EmissiveTex ? 1 : 0);
            // Юнит 12, а не 6: на шестом сидят три текстуры GI-объёма
            // (sampler3D), и карта самосвечения занимала его же — один юнит,
            // два разных типа сэмплера, то есть неопределённое поведение по
            // спецификации. Раскладка юнитов целиком — в render/ShadowMap.h.
            tex.SetInt("uEmissiveMap", kEmissiveUnit);
            if (it.Mat->EmissiveTex) it.Mat->EmissiveTex->Bind(kEmissiveUnit);
            if (it.Mat->AlbedoTex) it.Mat->AlbedoTex->Bind(0);
            if (it.Mat->NormalTex) it.Mat->NormalTex->Bind(2);
            if (it.Mat->MetallicTex) it.Mat->MetallicTex->Bind(3);
            if (it.Mat->RoughnessTex) it.Mat->RoughnessTex->Bind(4);
            if (it.Mat->AOTex) it.Mat->AOTex->Bind(5);
            tex.SetFloat("uOpacity", it.Opacity);
            it.Mesh_->DrawSubmesh(it.Submesh);
            ++m_stats.Batches;
        }
    }

    // 3. Материалы с СОБСТВЕННЫМ шейдером. Получают ровно тот же набор
    // стандартных юниформ, что и штатный путь (камера, свет, тени, GI), плюс
    // uTime и свои параметры — контракт описан в docs/custom_shaders.md.
    // Непрозрачные — здесь; полупрозрачные ждут прохода 4 (см. ниже).
    for (auto& kv : m_custom) {
        if (kv.second.Instances.empty() || kv.second.Transparent) continue;
        drawCustom(*kv.first.Program, *kv.first.Mesh_, kv.first.Submesh, kv.second);
    }

    // Есть ли вообще что смешивать в этом кадре — от этого зависит, поднимать ли
    // состояние блендинга.
    bool anyCustomTransparent = false;
    for (const auto& kv : m_custom)
        if (kv.second.Transparent && !kv.second.Instances.empty()) anyCustomTransparent = true;

    // 4. Полупрозрачный проход — ПОСЛЕ всей непрозрачной геометрии.
    //
    // Два правила, без которых прозрачность не работает:
    //   • порядок — от дальних к ближним: смешивание не коммутативно, и стекло,
    //     нарисованное раньше того, что за ним, просто закрасит его собой;
    //   • глубина ЧИТАЕТСЯ, но не пишется: прозрачный объект обязан прятаться за
    //     стеной, но не должен закрывать собой другой прозрачный.
    if (!m_transparent.empty() || anyCustomTransparent) {
        std::sort(m_transparent.begin(), m_transparent.end(),
                  [](const TransparentItem& a, const TransparentItem& b) {
                      return a.Depth > b.Depth; // дальние первыми
                  });

        device.SetBlend(true);
        device.SetDepthWrite(false);
        // Двустороннее прозрачное тело рисуется В ДВА ПРОХОДА: сначала задние
        // грани, потом передние.
        //
        // ПОЧЕМУ НЕ ПРОСТО «выключить отсечение». Сортировка идёт по объектам, а
        // внутри объекта треугольники рисуются в порядке индексов — то есть в
        // произвольном относительно камеры. При выключенном отсечении куб
        // смешивает свои же шесть граней как попало, и получается то, что видно
        // на экране: одна половина выглядит плотнее другой, а грани проступают
        // пятнами. Сортировать треугольники каждый кадр — слишком дорого и всё
        // равно неверно при пересечениях.
        //
        // Два прохода решают именно этот случай, и решают его ТОЧНО: у выпуклого
        // тела (куб, сфера, стакан) задние грани всегда дальше передних, поэтому
        // «сначала все задние, потом все передние» — это и есть правильный
        // порядок от дальнего к ближнему. Для невыпуклых тел остаётся
        // приблизительным, но заведомо лучше произвольного.
        //
        // Замкнутым телам (вода) материал ставит DoubleSided = false: у них
        // задней стенки не видно, и второй проход был бы лишней работой.
        auto passCount = [](const Material* mat) {
            return (!mat || mat->Render.DoubleSided) ? 2 : 1;
        };
        auto setCullForPass = [&device](const Material* mat, int pass) {
            if (mat && !mat->Render.DoubleSided) {
                device.SetCullMode(sage::rhi::CullMode::Back);
                return;
            }
            // Проход 0 — задние грани (отсекаем передние), проход 1 — передние.
            device.SetCullMode(pass == 0 ? sage::rhi::CullMode::Front
                                         : sage::rhi::CullMode::Back);
        };

        // Группы со своим шейдером — первыми: это, как правило, среда (вода,
        // листва), на фоне которой и стоит всё остальное прозрачное. Правильнее
        // было бы слить их в общий порядок, но группа, растянутая на всю сцену,
        // не имеет одной глубины, по которой её можно было бы вставить.
        for (auto& kv : m_custom) {
            CustomGroup& g = kv.second;
            if (!g.Transparent || g.Instances.empty()) continue;
            const int passes = passCount(g.Mat);
            for (int pass = 0; pass < passes; ++pass) {
                setCullForPass(g.Mat, passes == 1 ? 1 : pass);
                drawCustom(*kv.first.Program, *kv.first.Mesh_, kv.first.Submesh, g);
            }
        }

        Shader* texShader = nullptr;
        setupCommon(lit);
        size_t i = 0;
        while (i < m_transparent.size()) {
            const TransparentItem& head = m_transparent[i];
            const int passes = passCount(head.Mat);
            if (head.Textured) {
                if (!texShader) { texShader = &TexShader(); setupCommon(*texShader); }
                Shader& t = *texShader;
                t.SetInt("uAlbedoMap", 0); t.SetInt("uNormalMap", 2);
                t.SetInt("uMetallicMap", 3); t.SetInt("uRoughnessMap", 4); t.SetInt("uAOMap", 5);
                bindLightmap(t, head.LmPage);
                t.SetMat4("uModel", head.Inst.Model);
                // Тон и свечение — из инстанса (свёрнутые), как на всех
                // остальных путях; см. TexturedItem в заголовке.
                t.SetVec3("uAlbedoFactor", head.Inst.Color);
                t.SetFloat("uMetallic", head.Mat->Metallic);
                t.SetFloat("uRoughness", head.Mat->Roughness);
                t.SetFloat("uOpacity", head.Inst.Alpha);
                t.SetInt("uHasAlbedo", head.Mat->AlbedoTex ? 1 : 0);
                t.SetInt("uHasNormal", head.Mat->NormalTex ? 1 : 0);
                t.SetInt("uHasMetallic", head.Mat->MetallicTex ? 1 : 0);
                t.SetInt("uHasRoughness", head.Mat->RoughnessTex ? 1 : 0);
                t.SetInt("uHasAO", head.Mat->AOTex ? 1 : 0);
                t.SetVec3("uEmissive", head.Inst.Emissive);
                t.SetInt("uHasEmissive", head.Mat->EmissiveTex ? 1 : 0);
                t.SetInt("uEmissiveMap", kEmissiveUnit);
                if (head.Mat->EmissiveTex) head.Mat->EmissiveTex->Bind(kEmissiveUnit);
                if (head.Mat->AlbedoTex) head.Mat->AlbedoTex->Bind(0);
                if (head.Mat->NormalTex) head.Mat->NormalTex->Bind(2);
                if (head.Mat->MetallicTex) head.Mat->MetallicTex->Bind(3);
                if (head.Mat->RoughnessTex) head.Mat->RoughnessTex->Bind(4);
                if (head.Mat->AOTex) head.Mat->AOTex->Bind(5);
                for (int pass = 0; pass < passes; ++pass) {
                    setCullForPass(head.Mat, passes == 1 ? 1 : pass);
                    head.Mesh_->DrawSubmesh(head.Submesh);
                    ++m_stats.Batches;
                }
                ++i;
                continue;
            }

            // Подряд идущие плоские объекты С ОДНИМ МЕШЕМ рисуем одной пачкой.
            // Внутри пачки порядок смешивания произволен — но это ровно те
            // объекты, что стоят рядом в отсортированном списке и сделаны из
            // одного материала (вода, листва, стекло одной витрины), где разницы
            // не видно. Зато тысяча плиток воды остаётся одним draw call'ом, а
            // не тысячей: без этого прозрачность была бы неприменима к тому,
            // ради чего она чаще всего и нужна.
            size_t j = i;
            m_transparentBatch.clear();
            while (j < m_transparent.size() && !m_transparent[j].Textured &&
                   m_transparent[j].Mesh_ == head.Mesh_ &&
                   m_transparent[j].Submesh == head.Submesh &&
                   m_transparent[j].Mat == head.Mat) {
                m_transparentBatch.push_back(m_transparent[j].Inst);
                ++j;
            }
            lit.Use();
            bindLightmap(lit, head.LmPage);
            head.Mesh_->SetInstances(m_transparentBatch.data(), m_transparentBatch.size());
            for (int pass = 0; pass < passes; ++pass) {
                setCullForPass(head.Mat, passes == 1 ? 1 : pass);
                head.Mesh_->DrawSubmeshInstances(head.Submesh, m_transparentBatch.size());
                ++m_stats.Batches;
            }
            i = j;
        }

        device.SetCullMode(sage::rhi::CullMode::Back);
        device.SetDepthWrite(true);
        device.SetBlend(false);
    }
    m_stats.Transparent = (int)m_transparent.size();
    for (const auto& kv : m_custom)
        if (kv.second.Transparent) m_stats.Transparent += (int)kv.second.Instances.size();
    return m_stats;
}

// --- Проход скоростей ------------------------------------------------------
//
// Вершина проецируется ДВАЖДЫ: своей матрицей этого кадра и своей матрицей
// прошлого. Разница их экранных позиций и есть вектор скорости пикселя.
//
// Считать её именно в вершинном шейдере, а не восстанавливать из глубины,
// принципиально: восстановление знает только, куда сместилась КАМЕРА, и
// молча считает мир неподвижным.
namespace {

const char* kVelocityVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;

uniform mat4 uViewProj;
uniform mat4 uPrevViewProj;
uniform mat4 uModel;
uniform mat4 uPrevModel;

out vec4 vClip;
out vec4 vPrevClip;

void main() {
    vClip     = uViewProj     * uModel     * vec4(aPos, 1.0);
    vPrevClip = uPrevViewProj * uPrevModel * vec4(aPos, 1.0);
    gl_Position = vClip;
}
)";

const char* kVelocityFrag = R"(#version 330 core
in vec4 vClip;
in vec4 vPrevClip;
out vec4 FragColor;

void main() {
    // Перспективное деление делаем ЗДЕСЬ, а не во вершинном шейдере: между
    // вершинами интерполируется clip-позиция, и делить надо уже
    // проинтерполированное значение, иначе скорость внутри крупного
    // треугольника считается неверно.
    if (abs(vClip.w) < 1e-6 || abs(vPrevClip.w) < 1e-6) { FragColor = vec4(0.0); return; }
    vec2 ndc     = vClip.xy / vClip.w;
    vec2 prevNdc = vPrevClip.xy / vPrevClip.w;
    // В долях экрана (0..1), как их читает проход смаза: NDC вдвое шире.
    FragColor = vec4((ndc - prevNdc) * 0.5, 0.0, 1.0);
}
)";

Shader& VelocityShader() {
    static Shader* s = new Shader(Shader::FromSource(kVelocityVert, kVelocityFrag, "RenderBatchVelocity"));
    return *s;
}

} // namespace

void RenderBatch::ResetVelocityHistory() {
    m_prevWorld.Clear();
    m_hasPrevFrame = false;
}

void RenderBatch::RenderVelocity(const glm::mat4& viewProj, const glm::mat4& prevViewProj) {
    Shader& sh = VelocityShader();
    sh.Use();
    sh.SetMat4("uViewProj", viewProj);
    sh.SetMat4("uPrevViewProj", prevViewProj);

    for (const CullItem& c : m_cull) {
        if (!c.Visible || !c.Mesh_) continue;

        // Матрица прошлого кадра. Если сущности тогда не было — берём текущую:
        // нулевая скорость честнее, чем смаз из случайной точки, где её никогда
        // не было. То же на первом кадре после сброса истории.
        glm::mat4 prev = c.Model;
        if (m_hasPrevFrame) {
            if (const glm::mat4* was = m_prevWorld.Find(c.Entity)) prev = *was;
        }
        sh.SetMat4("uModel", c.Model);
        sh.SetMat4("uPrevModel", prev);
        c.Mesh_->Draw();
    }

}

void RenderBatch::AdvanceVelocityHistory() {
    // Снимок берём с ПОЛНОГО кэша мировых матриц, а не с видимого списка:
    // объект, вылетевший за край кадра и вернувшийся, иначе потерял бы историю
    // и на первом же кадре возврата дал бы нулевую скорость посреди движения.
    //
    // Таблица строится заново, а не дополняется: иначе удалённые сущности
    // копились бы в ней бесконечно. С плотным хранением это обычное
    // копирование массива, а не перестройка хэш-таблицы на каждую сущность.
    m_prevWorld = m_worldCache;
    m_hasPrevFrame = !m_prevWorld.Empty();
}

void RenderBatch::RenderOcclusionProbes(const glm::mat4& viewProj, const glm::vec3& viewPos) {
    SAGE_PROFILE("Проверка перекрытия");
    if (!m_occlusionEnabled) return;
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    if (!device.SupportsOcclusionQueries()) return;

    ++m_frameIndex;
    m_stats.Probes = 0;

    // Таблица ЭТОГО вида: ответы соседних видов к нему отношения не имеют
    // (см. SetOcclusionCulling).
    OcclusionTable& slots = OcclusionSlots();

    // 1) Забираем результаты, выданные в ПРОШЛОМ кадре. Именно прошлом: запрос
    //    этого кадра ещё выполняется, и спрашивать его сейчас значило бы ждать
    //    видеокарту — то есть платить за экономию простоем.
    for (auto& kv : slots) {
        OcclusionSlot& slot = kv.second;
        if (!slot.Pending) continue;
        if (!device.OcclusionResultReady(slot.Query)) continue;
        const bool visible = device.OcclusionVisible(slot.Query);
        slot.Pending = false;
        slot.HiddenFrames = visible ? 0 : slot.HiddenFrames + 1;
        slot.Visible = visible;
    }

    // Коробка-заменитель: единичный куб, растянутый по ограничивающей сфере.
    // Куб, а не сама геометрия объекта: смысл проверки в том, чтобы она стоила
    // заметно дешевле отрисовки, и рисовать ради неё тот же меш было бы
    // самообманом.
    if (!m_probeBox) m_probeBox = std::make_unique<Mesh>(Mesh::CreateCube());

    // 2) Готовим состояние: пишем только глубину, не трогая ни цвет, ни сам
    //    буфер глубины. Коробка не должна ни появиться в кадре, ни заслонить
    //    собой то, что за ней, — иначе следующий проверяемый объект получил бы
    //    ответ «закрыт» от нашей же служебной коробки.
    Shader& depth = DepthUModelShader();
    depth.Use();
    depth.SetMat4("uLightSpace", viewProj); // имя историческое: это просто viewProj прохода
    device.SetColorWrite(false);
    device.SetDepthWrite(false);
    device.SetDepthTest(true);
    // Отсечение выключаем: камера может оказаться ВНУТРИ коробки, и тогда с
    // обычным отсечением задних граней от неё не осталось бы ни одного пикселя,
    // а объект был бы объявлен закрытым — то есть исчез бы ровно тогда, когда
    // находится вплотную к зрителю.
    device.SetCullMode(sage::rhi::CullMode::Off);

    for (const CullItem& c : m_cull) {
        // Проверяем только то, что прошло фрустум и порог размера. Всё
        // остальное уже отсечено дешевле.
        if (c.Radius <= 0.0f) continue;
        if (!c.Visible && !c.Occluded) continue;

        OcclusionSlot& slot = slots[c.Entity];

        // КАМЕРА ВНУТРИ ОБЪЕКТА — проверять нельзя, и это не мелкий случай.
        //
        // Коробка-заменитель, накрывшая камеру, обрезается ближней плоскостью:
        // её передняя часть за спиной, задняя — за уже нарисованной геометрией.
        // Ни одного пикселя не проходит, и объект объявляется закрытым. На
        // стене, которая и служит главным перекрывателем сцены, это означало
        // катастрофу: стена исчезала, буфер глубины оставался пустым, и
        // следующим кадром «видимым» становилось всё подряд — картинка
        // моргала целиком. Ровно это и происходило до правки.
        //
        // Такие объекты считаем видимыми без запроса: они заведомо в кадре.
        if (glm::distance(viewPos, c.Center) < c.Radius * 1.5f) {
            slot.Visible = true;
            slot.Pending = false;
            continue;
        }

        if (slot.Pending) continue; // прошлый запрос ещё не созрел

        // Закрытые объекты перепроверяем РЕЖЕ, но обязательно: иначе объект,
        // однажды признанный закрытым, не вернулся бы никогда, а стена, из-за
        // которой он не виден, может уехать. Раз в четыре кадра — компромисс
        // между ценой запросов и задержкой появления.
        if (!slot.Visible && (m_frameIndex + (unsigned long long)c.Entity) % 4 != 0) continue;

        if (!slot.Query.Valid()) slot.Query = device.CreateOcclusionQuery();
        if (!slot.Query.Valid()) continue;

        // Коробка чуть больше сферы: ошибка в сторону «нарисуем лишнее»
        // безопасна, ошибка в другую сторону — это пропавший объект.
        glm::mat4 box(1.0f);
        box[3] = glm::vec4(c.Center, 1.0f);
        const float side = c.Radius * 2.1f;
        box[0][0] = side; box[1][1] = side; box[2][2] = side;
        depth.SetMat4("uModel", box);

        device.BeginOcclusionQuery(slot.Query);
        m_probeBox->Draw();
        device.EndOcclusionQuery();
        slot.Pending = true;
        ++m_stats.Probes;
    }

    device.SetCullMode(sage::rhi::CullMode::Back);
    device.SetDepthWrite(true);
    device.SetColorWrite(true);

    // 3) Забываем сущности, которых в кадре больше нет: иначе таблица росла бы
    //    вместе с историей сцены, а не с её содержимым.
    if (slots.size() > m_cull.size() * 2 + 64) {
        OcclusionTable alive;
        alive.reserve(m_cull.size());
        for (const CullItem& c : m_cull) {
            auto it = slots.find(c.Entity);
            if (it != slots.end()) alive.emplace(c.Entity, it->second);
        }
        // Запросы выброшенных сущностей освобождаем явно: это ресурс драйвера.
        for (auto& kv : slots) {
            if (alive.find(kv.first) == alive.end() && kv.second.Query.Valid()) {
                device.DestroyOcclusionQuery(kv.second.Query);
            }
        }
        slots.swap(alive);
    }
}

void RenderBatch::RenderDepth(Scene& scene, const glm::mat4& lightMatrix) {
    m_stats = {};
    m_collectingShadows = true;
    CollectVisible(scene, lightMatrix); // отсечение по фрустуму света
    m_collectingShadows = false;

    // Flat — инстансно.
    Shader& di = DepthInstShader();
    di.Use();
    di.SetMat4("uLightSpace", lightMatrix);
    for (auto& kv : m_groups) {
        if (kv.second.Instances.empty()) continue;
        kv.first.Mesh_->SetInstances(kv.second.Instances.data(), kv.second.Instances.size());
        kv.first.Mesh_->DrawSubmeshInstances(kv.first.Submesh, kv.second.Instances.size());
    }
    // Полупрозрачные: тень отбрасывают только ПЛОТНЫЕ из них. Карта теней
    // бинарна — она не умеет «половину тени», — поэтому выбор всегда между
    // полной тенью и никакой. Стекло и вода не должны затемнять дно, а
    // притемнённая до 0.7 доска обязана оставаться доской: порог посередине и
    // есть наименее неправильный ответ.
    if (!m_transparent.empty()) {
        Shader& du = DepthUModelShader();
        du.Use();
        du.SetMat4("uLightSpace", lightMatrix);
        for (const TransparentItem& it : m_transparent) {
            if (it.Inst.Alpha < 0.5f) continue;
            du.SetMat4("uModel", it.Inst.Model);
            it.Mesh_->DrawSubmesh(it.Submesh);
        }
    }

    // Текстурные — индивидуально (uModel).
    if (!m_textured.empty()) {
        Shader& du = DepthUModelShader();
        du.Use();
        du.SetMat4("uLightSpace", lightMatrix);
        for (const TexturedItem& it : m_textured) {
            du.SetMat4("uModel", it.Model);
            it.Mesh_->DrawSubmesh(it.Submesh);
        }
    }
}

} // namespace sage::ecs
