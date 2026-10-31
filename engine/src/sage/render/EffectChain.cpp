#include "sage/render/EffectChain.h"

#include "sage/render/ShaderMaterial.h"
#include "sage/rhi/GraphicsDevice.h"

namespace sage::render {
namespace {

const char* kFullscreenVert = R"GLSL(#version 330 core
out vec2 vUV;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

const char* kCopyFrag = R"GLSL(#version 330 core
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSource;
void main() { FragColor = texture(uSource, vUV); }
)GLSL";

// Пустая геометрия: в core-профиле рисовать без привязанного массива вершин
// нельзя, даже если шейдер ничего из него не читает.
//
// Объекты ниже НАМЕРЕННО не удаляются (как CopyShader в PostFX): статический
// unique_ptr разрушался бы после закрытия окна, когда GL-контекста уже нет, и
// игра падала на выходе. Объект уходит вместе с контекстом.
sage::rhi::Geometry& EmptyGeometry() {
    static sage::rhi::Geometry* g = nullptr;
    static sage::rhi::GraphicsDevice* owner = nullptr;
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    if (!g || owner != &device) {
        g = device.CreateGeometry(sage::rhi::VertexLayout{}).release();
        owner = &device;
    }
    return *g;
}

sage::rhi::ShaderProgram* CopyProgram() {
    static sage::rhi::ShaderProgram* p = nullptr;
    static sage::rhi::GraphicsDevice* owner = nullptr;
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    if (!p || owner != &device) {
        sage::rhi::ShaderBuildLog log;
        p = device.CreateProgram({{{sage::rhi::ShaderStage::Vertex, kFullscreenVert},
                                   {sage::rhi::ShaderStage::Fragment, kCopyFrag}},
                                  "sage.copy"},
                                 log)
                .release();
        owner = &device;
    }
    return p;
}

} // namespace

void DrawFullscreenTriangle() { EmptyGeometry().DrawArrays(3); }

const char* FullscreenVertexSource() { return kFullscreenVert; }

int AddEffectChain(RenderGraph& graph, const std::string& prefix, RGHandle input, RGHandle output,
                   const std::vector<EffectStep>& steps) {
    std::vector<const EffectStep*> active;
    for (const EffectStep& s : steps)
        if (s.Enabled && s.Material) active.push_back(&s);

    if (active.empty()) {
        RGPass copy;
        copy.Name = prefix + ": копия";
        copy.Reads = {input};
        copy.Writes = {output};
        copy.Execute = [input](RenderGraphContext& ctx) {
            sage::rhi::ShaderProgram* p = CopyProgram();
            if (!p) return;
            RestoreDefaultState(ctx.Device());
            ctx.Device().SetDepthTest(false);
            p->Use();
            p->SetInt("uSource", 0);
            ctx.Device().BindTexture2D(0, ctx.Color(input));
            DrawFullscreenTriangle();
            RestoreDefaultState(ctx.Device());
        };
        graph.AddPass(std::move(copy));
        return 1;
    }

    RGTextureDesc mid;
    mid.Kind = sage::rhi::RenderTargetKind::ColorHDR;
    const RGHandle ping = active.size() > 1 ? graph.CreateTexture(prefix + ": A", mid) : kNoHandle;
    const RGHandle pong = active.size() > 2 ? graph.CreateTexture(prefix + ": B", mid) : kNoHandle;

    RGHandle source = input;
    for (size_t i = 0; i < active.size(); ++i) {
        // Шаг — копией: список шагов вызывающего к выполнению кадра уже может умереть.
        const EffectStep step = *active[i];
        const bool last = i + 1 == active.size();
        const RGHandle dest = last ? output : ((i % 2 == 0) ? ping : pong);
        RGPass pass;
        pass.Name = prefix + ": " + step.Name;
        pass.Reads = {source};
        pass.Writes = {dest};
        // Глубину эффекту даёт ТОЛЬКО вход цепочки: у промежуточных целей её нет.
        pass.Execute = [step, source, input](RenderGraphContext& ctx) {
            ShaderMaterial& m = *step.Material;
            m.SetTexture("uSource", 0, ctx.Color(source));
            m.SetTexture("uDepth", 1, ctx.Depth(input));
            sage::rhi::RenderTarget* t = ctx.Target(source);
            const int w = t ? t->Width() : ctx.Width(), h = t ? t->Height() : ctx.Height();
            m.Set("uTexel", glm::vec2(1.0f / (float)w, 1.0f / (float)h));
            m.State.DepthTest = false;
            m.State.DepthWrite = false;
            m.State.Cull = sage::rhi::CullMode::Off;
            if (step.Setup) step.Setup(m, ctx);
            if (m.Apply(ctx.Device())) DrawFullscreenTriangle();
            RestoreDefaultState(ctx.Device());
        };
        graph.AddPass(std::move(pass));
        source = dest;
    }
    return (int)active.size();
}

} // namespace sage::render
