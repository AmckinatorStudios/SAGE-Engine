#include "sage/render/StandardPipeline.h"

#include <cmath>

#include "sage/render/PostFX.h"
#include "sage/render/ScenePasses.h"
#include "sage/render/ShadowMap.h"
#include "sage/render/SkyDraw.h"
#include "sage/render/SkyRenderer.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Light.h"
#include "sage/scene/Scene.h"

namespace sage::render {

StandardPipeline::StandardPipeline() : Chain(PostChain::Default()) {}
StandardPipeline::~StandardPipeline() = default;

bool StandardPipeline::Initialize(sage::rhi::GraphicsDevice& device, std::string& error) {
    (void)device;
    (void)error;
    m_shadowMap = std::make_unique<ShadowMap>(ShadowResolution, ShadowCascades);
    m_sky = std::make_unique<SkyRenderer>();
    m_postfx = std::make_unique<PostFX>();
    return true;
}

void StandardPipeline::Shutdown() {
    m_postfx.reset();
    m_sky.reset();
    m_shadowMap.reset();
}

void StandardPipeline::Build(RenderGraph& graph, const RenderView& view) {
    m_hdr = graph.CreateTexture(kHdr, RGTextureDesc{});
    m_shadowRes = graph.DeclareVirtual(kShadows);
    AddShadowPasses(graph, view);
    AddScenePasses(graph, view);
    AddPostPasses(graph, view);
}

void StandardPipeline::AddShadowPasses(RenderGraph& graph, const RenderView& view) {
    RGPass pass;
    pass.Name = kShadowPass;
    pass.Type = RGPassType::Custom; // карта теней привязывает свои цели сама
    pass.Writes = {m_shadowRes};
    pass.Enabled = Shadows && view.World != nullptr;
    pass.Execute = [this, view](RenderGraphContext& ctx) {
        Scene& scene = *view.World;
        ShadowMap::CameraView v;
        v.Position = view.Position;
        const glm::mat3 basis = glm::mat3(view.View);
        v.Forward = -glm::vec3(basis[0][2], basis[1][2], basis[2][2]);
        v.Up = glm::vec3(basis[0][1], basis[1][1], basis[2][1]);
        v.FovY = 2.0f * std::atan(1.0f / view.Proj[1][1]);
        v.Aspect = (float)view.Width / (float)std::max(view.Height, 1);
        v.Near = view.Proj[3][2] / (view.Proj[2][2] - 1.0f);
        v.ShadowDistance = scene.Lighting.Shadows.Distance > 0.0f ? scene.Lighting.Shadows.Distance
                                                                  : ShadowDistance;
        if (m_shadowMap->CascadeCount() > 1) m_shadowMap->SetCascades(scene.Lighting.Sun.Direction, v);
        else m_shadowMap->FitSingle(scene.Lighting.Sun.Direction, v);
        RenderShadowDepth(*m_shadowMap, scene, m_batch, ctx.Width(), ctx.Height());
    };
    graph.AddPass(std::move(pass));
}

void StandardPipeline::AddScenePasses(RenderGraph& graph, const RenderView& view) {
    {
        RGPass sky;
        sky.Name = kSkyPass;
        sky.Writes = {m_hdr};
        sky.ColorLoad = LoadOp::Clear;
        sky.DepthLoad = LoadOp::Clear;
        if (view.World) {
            const glm::vec3 c = SceneClearColor(view.World->Lighting);
            sky.ClearColor = {c, 1.0f};
        }
        sky.Execute = [this, view](RenderGraphContext&) {
            if (!view.World || !view.World->Lighting.Skybox.Enabled) return;
            DrawSceneSky(*m_sky, view.World->Lighting, view.View, view.Proj, view.Time);
        };
        graph.AddPass(std::move(sky));
    }
    RGPass scene;
    scene.Name = kScenePass;
    scene.Writes = {m_hdr};
    if (Shadows && view.World) scene.Reads = {m_shadowRes};
    scene.Execute = [this, view](RenderGraphContext&) {
        if (!view.World) return;
        SceneColorInput in;
        in.View = view.View;
        in.Proj = view.Proj;
        in.ViewPos = view.Position;
        in.Env = &view.World->Lighting;
        in.Shadows = ShadowBinding(*m_shadowMap, Shadows);
        in.Time = view.Time;
        RenderSceneColor(*view.World, m_batch, in);
    };
    graph.AddPass(std::move(scene));
}

void StandardPipeline::AddPostPasses(RenderGraph& graph, const RenderView& view) {
    RGPass post;
    post.Name = kPostPass;
    post.Type = RGPassType::Custom; // тракт сам выбирает свои промежуточные цели
    post.Reads = {m_hdr};
    post.Writes = {graph.Output()};
    const RGHandle hdr = m_hdr;
    post.Execute = [this, view, hdr](RenderGraphContext& ctx) {
        sage::rhi::RenderTarget* t = ctx.Target(hdr);
        if (!t) return;
        t->Resolve();
        static const PostChain kOnlyTonemap;
        m_postfx->Render(ctx.Color(hdr), ctx.Depth(hdr), t->Width(), t->Height(), view.Proj,
                         view.View, Post ? Chain : kOnlyTonemap, view.Output, view.ViewportX,
                         view.ViewportY, view.Width, view.Height);
    };
    graph.AddPass(std::move(post));
}

} // namespace sage::render
