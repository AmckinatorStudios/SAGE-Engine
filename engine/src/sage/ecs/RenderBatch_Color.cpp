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

// Цветовой проход батча: материалы, свет, тени, прозрачность (см. RenderBatch.h).

namespace sage::ecs {

using namespace batch_detail;

RenderStats RenderBatch::RenderColor(Scene& scene, const glm::mat4& view, const glm::mat4& proj,
                                     const glm::vec3& viewPos, const LightingEnvironment& env,
                                     const ShadowBinding& shadows, int shadingMode,
                                     const sage::render::ReflectionBinding* reflections) {
    m_stats = {};
    m_viewPos = viewPos; // нужна сборке: по ней считается глубина прозрачных
    m_skipPlanarReflectors = reflections && reflections->CapturingPlanar;
    m_probes = (reflections && reflections->Probes && !reflections->Probes->Empty())
                   ? reflections->Probes : nullptr;
    CollectVisible(scene, proj * view);
    m_probes = nullptr;
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();

    sage::gi::GIState* gi = scene.GI && scene.GI->Baked ? scene.GI.get() : nullptr;
    sage::gi::GIState* giVolume = scene.GI ? scene.GI.get() : nullptr; // объём проб живёт и без лайтмап

    // Источники кадра — по ячейкам ЭТОГО вида (render/LightClusters.h).
    // Строится на каждый цветной проход: у куба отражений, зеркала и панели
    // Game своя пирамида взгляда.
    m_clusters.Build(env, view, proj);

    auto setupCommon = [&](Shader& sh) {
        sh.Use();
        sh.SetMat4("uView", view);
        sh.SetMat4("uProjection", proj);
        sh.SetVec3("uViewPos", viewPos);
        sh.SetInt("uShadingMode", shadingMode);
        UploadLighting(sh, env);
        m_clusters.Upload(sh);
        // GI-объём проб (или выключение) + юниты сэмплеров GI (обязательны
        // всегда — см. SetGISamplerUnits).
        sage::gi::UploadGIVolume(sh, giVolume);
        BindAndUploadShadows(sh, shadows);
        // Отражения: пустая привязка тоже заливается — иначе включённый на
        // прошлом объекте куб остался бы висеть в программе шейдера.
        sage::render::UploadReflection(sh, reflections ? *reflections
                                                       : sage::render::ReflectionBinding{});
    };

    // Зонд отражений объекта. Заливается ТОЛЬКО когда зонды есть: без них
    // привязка одна на весь проход и уже залита в setupCommon.
    const bool anyProbes = reflections && reflections->Probes && !reflections->Probes->Empty();
    auto bindProbe = [&](Shader& sh, int probe) {
        if (anyProbes) sage::render::UploadReflection(sh, reflections->ForProbe(probe));
    };

    // Отсечение граней по материалу. Состояние конвейера, поэтому ставится
    // ПЕРЕД вызовом отрисовки и снимается в конец прохода: оставленное
    // включённым, оно утекло бы в следующий проход и в следующий кадр.
    auto setCull = [&device](int cull) {
        switch ((CullFaces)cull) {
            case CullFaces::Front: device.SetCullMode(sage::rhi::CullMode::Front); break;
            case CullFaces::None:  device.SetCullMode(sage::rhi::CullMode::Off); break;
            default:               device.SetCullMode(sage::rhi::CullMode::Back); break;
        }
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
    auto drawCustom = [&](Shader& sh, Mesh& mesh, unsigned int submesh, int probe,
                          CustomGroup& g) {
        setupCommon(sh);
        bindProbe(sh, probe);
        if (!g.Transparent) setCull(g.Mat ? (int)g.Mat->Render.Cull : (int)CullFaces::Back);
        sh.SetFloat("uTime", m_time);
        bindLightmap(sh, g.LmPage);
        if (g.Mat) {
            // Линейный, как у штатного пути (ShadingAlbedo): свой шейдер
            // получает тот же контракт, что и встроенный.
            sh.SetVec3("uAlbedoFactor", SrgbToLinear(g.Mat->Albedo));
            sh.SetFloat("uMetallic", g.Mat->Metallic);
            sh.SetFloat("uRoughness", g.Mat->Roughness);
            sh.SetFloat("uOpacity", g.Mat->Opacity);
            // ПОРОГ СТАВИТСЯ ВСЕГДА, в том числе нулевой. Шейдер общий на все
            // пачки кадра: не обнулив порог у следующего материала, мы отсекали
            // бы по альфе там, где её никто не задавал, — и обычная текстура с
            // альфой начала бы дырявить модель.
            sh.SetFloat("uAlphaCutoff", g.Mat->Render.AlphaCutoff);
            sh.SetFloat("uTranslucency", g.Mat->Render.Translucency);
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

    // Непрозрачная геометрия ЗАМЕНЯЕТ содержимое буфера, а не подмешивается к
    // нему, — значит, смешивание выключает сам проход, а не тот, кто его
    // позвал. Раньше состояние наследовалось: оставленное включённым чужим
    // проходом (частицы, интерфейс), оно превращало непрозрачные объекты в
    // полупрозрачные и портило небо под ними. Полупрозрачный проход ниже
    // поднимает смешивание сам и сам же его снимает.
    device.SetBlend(false);

    // Проходы 1–2 пишут и тонкий G-буфер (альбедо, нормаль) — если он есть у
    // буфера кадра. Только они: свой шейдер материала (3) и полупрозрачные (4)
    // G-буфер не пишут, и включённая запись оставила бы в нём мусор.
    device.SetGBufferWrites(true);

    // 1. Flat-инстансный проход.
    Shader& lit = LitShader();
    setupCommon(lit);
    for (auto& kv : m_groups) {
        if (kv.second.Instances.empty()) continue;
        bindLightmap(lit, kv.second.LmPage);
        bindProbe(lit, kv.first.Probe);
        setCull(kv.first.Cull);
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
        // У материала движка каждая карта своя, и значение лежит в R (см.
        // ModelLoader::ExtractMaterials, который так их и раскладывает).
        // Маски всё равно выставляем ЯВНО: неустановленный uniform — это ноль,
        // то есть «канала нет», и карта молча перестала бы влиять.
        tex.SetVec4("uMetallicMask", glm::vec4(1, 0, 0, 0));
        tex.SetVec4("uRoughnessMask", glm::vec4(1, 0, 0, 0));
        tex.SetVec4("uAOMask", glm::vec4(1, 0, 0, 0));
        for (const TexturedItem& it : m_textured) {
            bindLightmap(tex, it.LmPage);
            bindProbe(tex, it.Probe);
            setCull(it.Mat ? (int)it.Mat->Render.Cull : (int)CullFaces::Back);
            tex.SetMat4("uModel", it.Model);
            // Свёрнутое значение, а не it.Mat->Albedo: тон экземпляра обязан
            // работать и на текстурном пути (см. TexturedItem в заголовке).
            tex.SetVec3("uAlbedoFactor", it.Color);
            UploadTiling(tex, it.Mat->Render);
            tex.SetVec2("uUVOffset",
                        glm::vec2(it.Mat->Render.UVOffsetX, it.Mat->Render.UVOffsetY));
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
            tex.SetFloat("uAlphaCutoff", it.Mat->Render.AlphaCutoff);
            tex.SetFloat("uTranslucency", it.Mat->Render.Translucency);
            tex.SetInt("uUnlit", it.Mat->Render.Unlit ? 1 : 0);
            it.Mesh_->DrawSubmesh(it.Submesh);
            ++m_stats.Batches;
        }
    }

    // 3. Материалы с СОБСТВЕННЫМ шейдером. Получают ровно тот же набор
    // стандартных юниформ, что и штатный путь (камера, свет, тени, GI), плюс
    // uTime и свои параметры — контракт описан в docs/custom_shaders.md.
    // Непрозрачные — здесь; полупрозрачные ждут прохода 4 (см. ниже).
    device.SetGBufferWrites(false);
    for (auto& kv : m_custom) {
        if (kv.second.Instances.empty() || kv.second.Transparent) continue;
        drawCustom(*kv.first.Program, *kv.first.Mesh_, kv.first.Submesh, kv.first.Probe, kv.second);
    }

    // Есть ли вообще что смешивать в этом кадре — от этого зависит, поднимать ли
    // состояние блендинга.
    bool anyCustomTransparent = false;
    for (const auto& kv : m_custom)
        if (kv.second.Transparent && !kv.second.Instances.empty()) anyCustomTransparent = true;

    // Отсечение вернулось к обычному: его ставили по материалу, и оставленное
    // чужое значение перевернуло бы следующий проход кадра наизнанку.
    device.SetCullMode(sage::rhi::CullMode::Back);

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
            return (!mat || mat->Render.Cull == CullFaces::None) ? 2 : 1;
        };
        auto setCullForPass = [&](const Material* mat, int pass) {
            if (mat && mat->Render.Cull != CullFaces::None) {
                setCull((int)mat->Render.Cull);
                return;
            }
            // Двусторонний: проход 0 — задние грани (отсекаем передние),
            // проход 1 — передние.
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
                drawCustom(*kv.first.Program, *kv.first.Mesh_, kv.first.Submesh, kv.first.Probe, g);
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
                t.SetVec4("uMetallicMask", glm::vec4(1, 0, 0, 0));
                t.SetVec4("uRoughnessMask", glm::vec4(1, 0, 0, 0));
                t.SetVec4("uAOMask", glm::vec4(1, 0, 0, 0));
                bindLightmap(t, head.LmPage);
                bindProbe(t, head.Probe);
                t.SetMat4("uModel", head.Inst.Model);
                // Тон и свечение — из инстанса (свёрнутые), как на всех
                // остальных путях; см. TexturedItem в заголовке.
                t.SetVec3("uAlbedoFactor", head.Inst.Color);
                UploadTiling(t, head.Mat->Render);
                t.SetVec2("uUVOffset",
                          glm::vec2(head.Mat->Render.UVOffsetX, head.Mat->Render.UVOffsetY));
                t.SetFloat("uMetallic", head.Mat->Metallic);
                t.SetFloat("uRoughness", head.Mat->Roughness);
                t.SetFloat("uOpacity", head.Inst.Alpha);
                t.SetFloat("uAlphaCutoff", head.Mat->Render.AlphaCutoff);
                t.SetFloat("uTranslucency", head.Mat->Render.Translucency);
                t.SetInt("uUnlit", head.Mat->Render.Unlit ? 1 : 0);
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
                   m_transparent[j].Mat == head.Mat && m_transparent[j].Probe == head.Probe) {
                m_transparentBatch.push_back(m_transparent[j].Inst);
                ++j;
            }
            lit.Use();
            bindLightmap(lit, head.LmPage);
            bindProbe(lit, head.Probe);
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

} // namespace sage::ecs
