#pragma once
#include <memory>
#include <optional>
#include <string>

#include "sage/ecs/RenderBatch.h"
#include "sage/render/PostEffect.h"
#include "sage/render/RenderPipeline.h"

class ShadowMap;
class SkyRenderer;

// ---------------------------------------------------------------------------
// StandardPipeline — встроенный PBR-рендер SAGE как ОДНА из реализаций
// RenderPipeline ("sage.pbr").
//
// Кадр из трёх этапов, каждый — виртуальный метод, объявляющий свои проходы:
//   AddShadowPasses — «Тени»        (каскады солнца, ECS + скелеты)
//   AddScenePasses  — «Небо», «Сцена» (PBR в HDR-цель «HDR»)
//   AddPostPasses   — «Пост»        (тракт PostChain в выход графа)
//
// Свой рендер на его основе — наследник, переопределяющий один этап, или
// расширение (PipelineRenderer::AddExtension), вставляющее проходы по имени:
// «после "Сцена" нарисовать свою воду в "HDR"», «вместо "Пост" — свой
// тон-маппинг». Имена проходов и ресурсов — константы ниже: это контракт.
//
// Что он НЕ повторяет за SagePlayer: отражения, зонды, экранный GI, частицы и
// интерфейс сцены. Плеер собирает кадр сам (runtime/PlayerLayer_Render.cpp);
// здесь — основа, которую удобно дополнять, а не копия всего плеера.
// ---------------------------------------------------------------------------
namespace sage::render {

class PostFX;

class StandardPipeline : public RenderPipeline {
public:
    static constexpr const char* kId = "sage.pbr";
    // Имена проходов и ресурсов — опоры для расширений.
    static constexpr const char* kShadowPass = "Тени";
    static constexpr const char* kSkyPass = "Небо";
    static constexpr const char* kScenePass = "Сцена";
    static constexpr const char* kPostPass = "Пост";
    static constexpr const char* kHdr = "HDR";
    static constexpr const char* kShadows = "Карта теней";

    StandardPipeline();
    ~StandardPipeline() override;

    std::string Id() const override { return kId; }
    bool Initialize(sage::rhi::GraphicsDevice& device, std::string& error) override;
    void Build(RenderGraph& graph, const RenderView& view) override;
    void Shutdown() override;

    // Настройки — меняются между кадрами.
    bool Shadows = true;
    bool Post = true;
    PostChain Chain;
    int ShadowResolution = 2048;
    int ShadowCascades = 3;
    float ShadowDistance = 80.0f;

protected:
    virtual void AddShadowPasses(RenderGraph& graph, const RenderView& view);
    virtual void AddScenePasses(RenderGraph& graph, const RenderView& view);
    virtual void AddPostPasses(RenderGraph& graph, const RenderView& view);

    sage::ecs::RenderBatch m_batch;
    std::unique_ptr<ShadowMap> m_shadowMap;
    std::unique_ptr<SkyRenderer> m_sky;
    std::unique_ptr<PostFX> m_postfx;
    RGHandle m_hdr = kNoHandle;
    RGHandle m_shadowRes = kNoHandle;
};

} // namespace sage::render
