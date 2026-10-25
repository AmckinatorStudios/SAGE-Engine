#include "sage/ecs/RenderBatch.h"

#include "sage/anim/AnimationComponents.h"
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

#include "sage/ecs/RenderBatchDetail.h"

namespace sage::ecs {

namespace batch_detail {

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
layout(location = 0) out vec4 FragColor;
// Тонкий G-буфер (см. rhi::RenderTargetDesc::GBuffer) — экранному GI.
layout(location = 1) out vec4 oGAlbedo;
layout(location = 2) out vec4 oGNormal;

// Лайтмапа GI текущей группы (uLightmapEnabled — группа запечена).
uniform bool uLightmapEnabled;
uniform sampler2D uLightmap;
)") + kPbrSharedGlsl + R"(
void main() {
    vec3 N = normalize(Normal);
    oGAlbedo = vec4(clamp(vColor, 0.0, 1.0), 1.0);
    oGNormal = vec4(N * 0.5 + 0.5, 1.0);
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
const std::string kTexVert = std::string(R"(#version 330 core
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
)") + sage::render::kUVTilingGlsl + R"(
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
    TexCoords = TiledUV(aUV, aNormal, uModel);
    vUV2 = aUV2;
    gl_Position = uProjection * uView * world;
}
)";

// Фрагментная стадия текстурного PBR живёт в PbrShader.h: тот же самый код
// собирает себе скелетная модель (см. render/SkinnedModel.cpp). Держать её
// здесь значило бы иметь две редакции одного шейдера, которые расходятся при
// первой же правке — ровно так скин и остался без половины карт материала.
std::string TexFragSource() { return sage::render::TexturedPbrFragSource(); }

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

// Глубина С ВЫРЕЗОМ ПО АЛЬФЕ — для материалов с порогом отсечения (листва,
// трава, решётки, волосы). Карта теней знает только «есть поверхность или
// нет», и без этого прохода карточка листа отбрасывала тень целиком — сплошным
// квадратом, хотя сам лист на экране вырезан по контуру. Порог, повтор и
// сдвиг развёртки — те же, что у цветного прохода: тень обязана совпадать с
// тем, что видно, до пикселя.
const std::string kDepthAlphaVert = std::string(R"(#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
uniform mat4 uLightSpace;
uniform mat4 uModel;
out vec2 TexCoords;
)") + sage::render::kUVTilingGlsl + R"(
void main() {
    TexCoords = TiledUV(aUV, aNormal, uModel);
    gl_Position = uLightSpace * uModel * vec4(aPos, 1.0);
}
)";
const char* kDepthAlphaFrag = R"(#version 330 core
in vec2 TexCoords;
uniform sampler2D uAlbedoMap;
uniform vec2 uUVOffset;
uniform float uAlphaCutoff;
void main() {
    if (texture(uAlbedoMap, TexCoords + uUVOffset).a < uAlphaCutoff) discard;
}
)";

// Повтор развёртки — ОДНОЙ функцией на все проходы: режимов у него три, и
// разбирать их у каждого значило бы показывать одну и ту же стену с разным
// размером плитки в зависимости от того, каким путём её нарисовали. Размер
// грани в мире шейдер берёт сам из uModel (kUVTilingGlsl).
void UploadTiling(Shader& sh, const MaterialRender& render) {
    sh.SetVec2("uUVScale", BaseTiling(render));
    sh.SetInt("uUVWorld", render.Tiling == MaterialRender::TilingMode::WorldSize ? 1 : 0);
}

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
Shader& DepthAlphaShader() { static Shader* s = new Shader(Shader::FromSource(kDepthAlphaVert, kDepthAlphaFrag, "RenderBatchDepthAlpha")); return *s; }

// Режет ли материал поверхность по альфе альбедо. Нужна и карта, и порог:
// порог без карты резать нечем, карта без порога — обычный цвет, и начинать
// по её альфе дырявить тень значило бы продырявить готовые сцены.
bool CutsByAlpha(const Material* mat) {
    return mat && mat->AlbedoTex && mat->Render.AlphaCutoff > 0.0f;
}

} // namespace batch_detail

using namespace batch_detail;


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
        // СКЕЛЕТНЫЕ МОДЕЛИ РИСУЕТ НЕ ЭТОТ ПРОХОД.
        //
        // С тех пор как модель принадлежит Mesh, а не отдельному компоненту
        // «Animated Model», у анимированного персонажа есть и то и другое: Mesh
        // с путём к .glb и Animation, которая надевает на него скелет. Файл при
        // этом читается ДВАЖДЫ — статическим загрузчиком (в MeshPtr) и
        // скелетным, — и без этой проверки персонаж рисовался бы дважды: раз в
        // позе покоя статикой и раз анимированным поверх. Видно это как
        // «модель двоится» и как проваленный вдвое счёт треугольников.
        if (const AnimationComponent* anim = scene.Registry().try_get<AnimationComponent>(e)) {
            if (anim->Model) return;   // рисует sage::anim::DrawAnimatedModels
        }
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
        // Зонд — по центру объекта, один на все его части: одна модель не
        // должна отражать две разные комнаты.
        const int probeOfItem = m_probes ? m_probes->Pick(c.Center) : -1;
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
            // Без освещения — тоже текстурный путь: у инстансного
            // нетекстурного шейдера нет ветки «цвет как есть», а материал
            // обводки мультяшной модели — именно такой, одним цветом.
            const bool textured = mat && (mat->HasMaps() || mat->Render.Unlit);
            Shader* custom = (mat && mat->ShaderPtr) ? mat->ShaderPtr.get() : nullptr;

            m_stats.Triangles += (long long)(subs[si].IndexCount / 3);
            const int probe = probeOfItem;

            MeshInstance inst;
            inst.Model = c.Model;
            // В шейдер — ЛИНЕЙНЫЙ цвет (см. ShadingAlbedo): выбранный цвет
            // хранится в sRGB, как его показывает инспектор.
            inst.Color = ShadingAlbedo(mr, mat);
            inst.Alpha = opacity;
            inst.Emissive = ShadingEmissive(mr, mat);
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
                CustomGroup& g = m_custom[CustomKey{c.Mesh_, si, custom, probe}];
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
                                         textured, probe});
            } else if (textured) {
                // Есть текстурные карты — индивидуальный текстурный PBR-путь.
                m_textured.push_back({c.Mesh_, si, c.Model, mat, c.LmPage, opacity,
                                      inst.Color, inst.Emissive, probe});
            } else {
                // Плоский цвет — быстрый инстансный путь. Metallic/roughness из
                // материала (если назначен), иначе дефолты MeshInstance.
                const int cull = mat ? (int)mat->Render.Cull : (int)CullFaces::Back;
                Group& g = m_groups[MeshSlotKey{c.Mesh_, si, cull, probe}];
                g.Instances.push_back(inst);
                g.LmPage = c.LmPage; // у запечённой статики меш уникален — страница одна
            }
        }
    }
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
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();

    // ДВУСТОРОННЕЕ ОТБРАСЫВАЕТ ТЕНЬ ОБЕИМИ СТОРОНАМИ. Проход теней отсекает
    // задние грани, и одиночная плоскость (лист, трава, ткань, забор), которая
    // смотрит от света, в карту теней не попадала вовсе: у дерева тень
    // отбрасывала только половина листвы, и какая — зависело от угла солнца.
    // Материал, видимый с обеих сторон, и тень обязан давать с обеих.
    auto depthCull = [&device](const Material* mat) {
        device.SetCullMode(mat && mat->Render.Cull == CullFaces::None ? sage::rhi::CullMode::Off
                                                                       : sage::rhi::CullMode::Back);
    };

    // Flat — инстансно.
    Shader& di = DepthInstShader();
    di.Use();
    di.SetMat4("uLightSpace", lightMatrix);
    for (auto& kv : m_groups) {
        if (kv.second.Instances.empty()) continue;
        device.SetCullMode(kv.first.Cull == (int)CullFaces::None ? sage::rhi::CullMode::Off
                                                                  : sage::rhi::CullMode::Back);
        kv.first.Mesh_->SetInstances(kv.second.Instances.data(), kv.second.Instances.size());
        kv.first.Mesh_->DrawSubmeshInstances(kv.first.Submesh, kv.second.Instances.size());
    }

    // Сплошные — одной программой, вырезаемые по альфе — другой; сперва
    // раскладываем, чтобы не переключать программу на каждом объекте.
    struct DepthItem { Mesh* Mesh_; unsigned int Submesh; glm::mat4 Model; const Material* Mat; };
    std::vector<DepthItem> solid, cutout;
    // Полупрозрачные: тень отбрасывают только ПЛОТНЫЕ из них. Карта теней
    // бинарна — она не умеет «половину тени», — поэтому выбор всегда между
    // полной тенью и никакой. Стекло и вода не должны затемнять дно, а
    // притемнённая до 0.7 доска обязана оставаться доской: порог посередине и
    // есть наименее неправильный ответ.
    for (const TransparentItem& it : m_transparent) {
        if (it.Inst.Alpha < 0.5f) continue;
        (CutsByAlpha(it.Mat) ? cutout : solid).push_back({it.Mesh_, it.Submesh, it.Inst.Model, it.Mat});
    }
    // Текстурные — индивидуально (uModel).
    for (const TexturedItem& it : m_textured)
        (CutsByAlpha(it.Mat) ? cutout : solid).push_back({it.Mesh_, it.Submesh, it.Model, it.Mat});

    if (!solid.empty()) {
        Shader& du = DepthUModelShader();
        du.Use();
        du.SetMat4("uLightSpace", lightMatrix);
        for (const DepthItem& it : solid) {
            depthCull(it.Mat);
            du.SetMat4("uModel", it.Model);
            it.Mesh_->DrawSubmesh(it.Submesh);
        }
    }
    if (!cutout.empty()) {
        Shader& da = DepthAlphaShader();
        da.Use();
        da.SetMat4("uLightSpace", lightMatrix);
        da.SetInt("uAlbedoMap", 0);
        for (const DepthItem& it : cutout) {
            depthCull(it.Mat);
            da.SetMat4("uModel", it.Model);
            // Повтор и сдвиг — той же общей функцией, что и в цветном проходе
            // (Material.h): иначе вырез тени съехал бы относительно листа.
            UploadTiling(da, it.Mat->Render);
            da.SetVec2("uUVOffset", glm::vec2(it.Mat->Render.UVOffsetX, it.Mat->Render.UVOffsetY));
            da.SetFloat("uAlphaCutoff", it.Mat->Render.AlphaCutoff);
            it.Mat->AlbedoTex->Bind(0);
            it.Mesh_->DrawSubmesh(it.Submesh);
        }
    }
    // Отсечение вернулось к обычному: следующий проход (скелетные модели,
    // следующий каскад) не должен унаследовать чужое.
    device.SetCullMode(sage::rhi::CullMode::Back);
}

} // namespace sage::ecs
