#include "VoxelPipeline.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/core/Log.h"
#include "sage/render/EffectChain.h"
#include "sage/render/Frustum.h"
#include "sage/rhi/GraphicsDevice.h"

#include "BlockAtlas.h"
#include "ChunkMesher.h"

using namespace sage::render;
namespace rhi = sage::rhi;

namespace voxel {
namespace {

constexpr int kAtlasUnit = 0;
constexpr int kLightmapUnit = 1;

float Curve(float level, float gamma) {
    const float b = std::pow(0.8f, 15.0f - level);
    const float bright = 1.0f - std::pow(1.0f - b, 3.0f);
    return b + (bright - b) * gamma * 0.6f;
}

std::unique_ptr<rhi::Geometry> MakeChunkGeometry() {
    rhi::VertexLayout layout;
    layout.Stride = 8;
    layout.Attributes = {{0, 1, rhi::AttribType::UInt, 0}, {1, 1, rhi::AttribType::UInt, 4}};
    return rhi::GraphicsDevice::Get().CreateGeometry(layout);
}

} // namespace

void ComputeLightmapCpu(const LightmapParams& p, uint8_t out[16 * 16 * 4]) {
    for (int own = 0; own < 16; ++own)
        for (int sky = 0; sky < 16; ++sky) {
            const float s = Curve((float)sky, p.Gamma) * (0.06f + (1.0f - 0.06f) * p.Daylight);
            const float o = Curve((float)own, p.Gamma) * p.Flicker;
            const glm::vec3 c = glm::min(glm::max(s * p.SkyTint + o * glm::vec3(1.0f, 0.78f, 0.52f),
                                                  glm::vec3(0.018f, 0.02f, 0.03f)),
                                         glm::vec3(1.0f));
            uint8_t* px = &out[(own * 16 + sky) * 4];
            for (int i = 0; i < 3; ++i) px[i] = (uint8_t)std::lround(c[i] * 255.0f);
            px[3] = 255;
        }
}

VoxelPipeline::VoxelPipeline(World& world) : m_world(world) {}
VoxelPipeline::~VoxelPipeline() = default;

bool VoxelPipeline::Initialize(rhi::GraphicsDevice& device, std::string& error) {
    const rhi::GraphicsDevice::Caps caps = device.Capabilities();
    m_compute = caps.ComputeShaders && caps.ImageLoadStore;
    LOG_INFO("Voxel") << "Устройство: " << device.BackendName() << ", вычисления "
                      << (m_compute ? "есть" : "нет — карта света на CPU");

    const std::string dir = "assets/shaders/";
    m_shaders.AddIncludeDir(dir);
    m_shaders.Load("chunk", dir + "chunk.vert", dir + "chunk.frag");
    m_shaders.Load("sky", dir + "sky.vert", dir + "sky.frag");
    m_shaders.Load("outline", dir + "outline.vert", dir + "outline.frag");
    const std::string fsVert = FullscreenVertexSource();
    m_shaders.LoadStages("fx.underwater", {{rhi::ShaderStage::Vertex, "", fsVert},
                                           {rhi::ShaderStage::Fragment, dir + "underwater.frag", ""}});
    m_shaders.LoadStages("fx.final", {{rhi::ShaderStage::Vertex, "", fsVert},
                                      {rhi::ShaderStage::Fragment, dir + "final.frag", ""}});
    if (m_compute && !m_shaders.LoadCompute("lightmap", dir + "lightmap.comp")) {
        LOG_WARN("Voxel") << "вычислительный шейдер карты света не собрался — запасной путь на CPU";
        m_compute = false;
    }
    for (const char* need : {"chunk", "sky", "fx.final"}) {
        if (!m_shaders.Get(need)) {
            const ShaderEntry* e = m_shaders.Find(need);
            error = std::string("шейдер «") + need + "» не собрался";
            if (e && !e->Diagnostics.empty()) error += ": " + e->Diagnostics.front().ToString();
            return false;
        }
    }

    m_chunkMat.emplace(m_shaders, "chunk");
    m_waterMat.emplace(m_shaders, "chunk");
    m_waterMat->State.Blend = BlendState::Alpha;
    m_waterMat->State.DepthWrite = false;
    m_waterMat->State.Cull = rhi::CullMode::Off; // поверхность воды видна и снизу
    m_skyMat.emplace(m_shaders, "sky");
    m_skyMat->State.DepthTest = false;
    m_skyMat->State.DepthWrite = false;
    m_skyMat->State.Cull = rhi::CullMode::Off;
    m_outlineMat.emplace(m_shaders, "outline");
    m_outlineMat->State.Blend = BlendState::Alpha;
    m_outlineMat->State.Depth = rhi::DepthFunc::LessEqual;
    m_underwaterFx.emplace(m_shaders, "fx.underwater");
    m_finalFx.emplace(m_shaders, "fx.final");

    int aw = 0, ah = 0;
    const std::vector<uint8_t> atlas = BuildAtlasPixels(aw, ah);
    rhi::Texture2DDesc ad;
    ad.Width = aw;
    ad.Height = ah;
    ad.Channels = 4;
    ad.FilterMode = rhi::Filter::Nearest;
    ad.WrapMode = rhi::Wrap::ClampEdge;
    ad.GenerateMipmaps = false;
    ad.Srgb = true;
    m_atlas = device.CreateTexture2D(ad, atlas.data());

    rhi::Texture2DDesc ld;
    ld.Width = ld.Height = 16;
    ld.Channels = 4;
    ld.FilterMode = rhi::Filter::Bilinear; // дробные уровни у углов — плавно
    ld.WrapMode = rhi::Wrap::ClampEdge;
    ld.GenerateMipmaps = false;
    ld.ShaderWrite = true;
    uint8_t initial[16 * 16 * 4];
    ComputeLightmapCpu(LightmapParams{}, initial);
    m_lightmap = device.CreateTexture2D(ld, initial);

    // Рамка выделенного блока: 12 рёбер куба, чуть больше блока — без мерцания
    // глубины на его гранях.
    rhi::VertexLayout ol;
    ol.Stride = 12;
    ol.Attributes = {{0, 3, rhi::AttribType::Float, 0}};
    m_outline = device.CreateGeometry(ol);
    std::vector<float> lines;
    const float a = -0.003f, b = 1.003f;
    const float c[8][3] = {{a, a, a}, {b, a, a}, {b, a, b}, {a, a, b}, {a, b, a}, {b, b, a}, {b, b, b}, {a, b, b}};
    const int e[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& edge : e)
        for (int k : edge) lines.insert(lines.end(), c[k], c[k] + 3);
    m_outline->SetVertexData(lines.data(), lines.size() * sizeof(float), false);

    m_gpu.resize(m_world.Chunks().size());
    return true;
}

void VoxelPipeline::Shutdown() {
    m_gpu.clear();
    m_outline.reset();
    m_lightmap.reset();
    m_atlas.reset();
}

rhi::TextureHandle VoxelPipeline::AtlasHandle() const { return m_atlas ? m_atlas->Handle() : rhi::TextureHandle{}; }

int VoxelPipeline::PendingMeshes() const {
    int n = 0;
    for (const auto& c : m_world.Chunks())
        if (c->Dirty) ++n;
    return n;
}

void VoxelPipeline::UploadChunk(int index) {
    Chunk& chunk = *m_world.Chunks()[(size_t)index];
    const ChunkMeshes meshes = BuildChunkMesh(m_world, chunk.CX, chunk.CZ);
    GpuChunk& g = m_gpu[(size_t)index];
    auto upload = [](std::unique_ptr<rhi::Geometry>& geo, const ChunkMeshData& data, size_t& count) {
        count = data.Indices.size();
        if (count == 0) return;
        if (!geo) geo = MakeChunkGeometry();
        geo->SetVertexData(data.Vertices.data(), data.Vertices.size() * sizeof(uint32_t), true);
        geo->SetIndexData(data.Indices.data(), data.Indices.size(), true);
    };
    upload(g.Opaque, meshes.Opaque, g.OpaqueIndices);
    upload(g.Translucent, meshes.Translucent, g.TranslucentIndices);
    g.Faces = meshes.Opaque.Faces() + meshes.Translucent.Faces();
    g.Center = glm::vec3(chunk.CX * kChunkSize + 8.0f, kWorldHeight * 0.5f, chunk.CZ * kChunkSize + 8.0f);
    chunk.Dirty = false;
}

void VoxelPipeline::RebuildAllMeshes() {
    for (int i = 0; i < (int)m_gpu.size(); ++i)
        if (m_world.Chunks()[(size_t)i]->Dirty) UploadChunk(i);
}

void VoxelPipeline::SkyColors(glm::vec3& zenith, glm::vec3& horizon, glm::vec3& sunDir,
                              float& daylight) const {
    const float a = Frame.TimeOfDay * 6.2831853f;
    sunDir = glm::normalize(glm::vec3(std::cos(a), std::sin(a), 0.35f));
    daylight = glm::smoothstep(-0.12f, 0.28f, sunDir.y);
    const glm::vec3 dayZ(0.20f, 0.42f, 0.95f), nightZ(0.004f, 0.006f, 0.02f);
    const glm::vec3 dayH(0.62f, 0.78f, 1.0f), nightH(0.02f, 0.03f, 0.06f);
    zenith = glm::mix(nightZ, dayZ, daylight);
    horizon = glm::mix(nightH, dayH, daylight);
    // Закат и рассвет — тёплая полоса у горизонта, пока солнце низко.
    const float dusk = std::exp(-sunDir.y * sunDir.y * 40.0f);
    horizon = glm::mix(horizon, glm::vec3(1.0f, 0.48f, 0.22f), dusk * 0.6f);
}

glm::vec3 VoxelPipeline::HorizonColor() const {
    glm::vec3 z, h, s;
    float d;
    SkyColors(z, h, s, d);
    return h;
}

LightmapParams VoxelPipeline::Lightmap(float time) const {
    glm::vec3 z, h, sun;
    LightmapParams p;
    SkyColors(z, h, sun, p.Daylight);
    const float dusk = std::exp(-sun.y * sun.y * 40.0f);
    p.SkyTint = glm::mix(glm::vec3(0.55f, 0.6f, 0.95f), glm::vec3(1.0f), p.Daylight);
    p.SkyTint = glm::mix(p.SkyTint, glm::vec3(1.0f, 0.75f, 0.55f), dusk * 0.5f);
    // Мерцание — сумма двух несоизмеримых синусов: без видимого периода.
    p.Flicker = 0.97f + 0.03f * std::sin(time * 7.3f) * std::sin(time * 2.9f + 1.0f);
    return p;
}

void VoxelPipeline::Build(RenderGraph& graph, const RenderView& view) {
    const RGHandle meshes = graph.DeclareVirtual("Меши чанков");
    const RGHandle lightmap = graph.ImportTexture("Карта света", m_lightmap->Handle());
    RGTextureDesc sceneDesc;
    sceneDesc.Kind = rhi::RenderTargetKind::ColorHDRWithDepth;
    const RGHandle scene = graph.CreateTexture("Сцена", sceneDesc);
    const glm::mat4 viewProj = view.Proj * view.View;
    const VoxelFrame frame = Frame;

    // --- Геометрия: своя подготовка, в бюджете кадра ---
    {
        RGPass p;
        p.Name = "Меши чанков";
        p.Type = RGPassType::Custom;
        p.Writes = {meshes};
        p.Execute = [this, view](RenderGraphContext&) {
            // Сначала ближние: игрок видит пересборку там, где правит.
            std::vector<std::pair<float, int>> dirty;
            for (int i = 0; i < (int)m_gpu.size(); ++i) {
                const Chunk& c = *m_world.Chunks()[(size_t)i];
                if (!c.Dirty) continue;
                const glm::vec2 d = glm::vec2(c.CX * 16 + 8, c.CZ * 16 + 8) - glm::vec2(view.Position.x, view.Position.z);
                dirty.emplace_back(glm::dot(d, d), i);
            }
            std::sort(dirty.begin(), dirty.end());
            const int budget = view.Frame <= 1 ? (int)dirty.size() : MeshBudgetPerFrame;
            for (int k = 0; k < (int)dirty.size() && k < budget; ++k) UploadChunk(dirty[(size_t)k].second);
        };
        graph.AddPass(std::move(p));
    }

    // --- Свет: карта «уровень -> цвет» ---
    const LightmapParams lp = Lightmap(view.Time);
    {
        RGPass p;
        p.Name = "Карта света";
        p.Type = RGPassType::Compute;
        p.Writes = {lightmap};
        p.Enabled = UsesComputeLightmap();
        p.Execute = [this, lp, lightmap](RenderGraphContext& ctx) {
            rhi::ShaderProgram* prog = m_shaders.Get("lightmap");
            if (!prog) return;
            prog->Use();
            prog->SetFloat("uDaylight", lp.Daylight);
            prog->SetVec3("uSkyTint", lp.SkyTint);
            prog->SetFloat("uFlicker", lp.Flicker);
            prog->SetFloat("uGamma", lp.Gamma);
            ctx.Device().BindImage(0, ctx.Color(lightmap), rhi::ImageAccess::Write, rhi::ImageFormat::RGBA8);
            ctx.Device().Dispatch(1, 1, 1);
        };
        graph.AddPass(std::move(p));

        RGPass cpu;
        cpu.Name = "Карта света (CPU)";
        cpu.Type = RGPassType::Custom;
        cpu.Writes = {lightmap};
        cpu.Enabled = !UsesComputeLightmap();
        cpu.Execute = [this, lp](RenderGraphContext&) {
            uint8_t px[16 * 16 * 4];
            ComputeLightmapCpu(lp, px);
            m_lightmap->Update(px);
        };
        graph.AddPass(std::move(cpu));
    }

    glm::vec3 zenith, horizon, sunDir;
    float daylight = 1.0f;
    SkyColors(zenith, horizon, sunDir, daylight);
    const float far = (float)(RenderDistance * kChunkSize);
    const glm::vec2 fog(far * 0.6f, far * 0.95f);

    // --- Небо ---
    {
        RGPass p;
        p.Name = "Небо";
        p.Writes = {scene};
        p.ColorLoad = LoadOp::Clear;
        p.DepthLoad = LoadOp::Clear;
        p.ClearColor = {horizon, 1.0f};
        p.Execute = [this, view, viewProj, zenith, horizon, sunDir, daylight](RenderGraphContext& ctx) {
            ShaderMaterial& m = *m_skyMat;
            m.Set("uInvViewProj", glm::inverse(viewProj));
            m.Set("uCameraPos", view.Position);
            m.Set("uZenith", zenith);
            m.Set("uHorizon", horizon);
            m.Set("uSunDir", sunDir);
            m.Set("uDaylight", daylight);
            m.Set("uTime", view.Time);
            if (m.Apply(ctx.Device())) DrawFullscreenTriangle();
            RestoreDefaultState(ctx.Device());
        };
        graph.AddPass(std::move(p));
    }

    // Видимые чанки — один раз на кадр, для обоих проходов геометрии.
    const Frustum frustum = Frustum::FromViewProj(viewProj);
    auto visible = std::make_shared<std::vector<int>>();
    for (int i = 0; i < (int)m_gpu.size(); ++i) {
        const Chunk& c = *m_world.Chunks()[(size_t)i];
        const glm::vec2 d = glm::vec2(c.CX * 16 + 8, c.CZ * 16 + 8) - glm::vec2(view.Position.x, view.Position.z);
        if (glm::length(d) > far + 12.0f) continue;
        const glm::vec3 center(c.CX * 16 + 8.0f, kWorldHeight * 0.5f, c.CZ * 16 + 8.0f);
        if (!frustum.IntersectsSphere(center, std::sqrt(8.0f * 8.0f * 2.0f + 48.0f * 48.0f))) continue;
        visible->push_back(i);
    }

    auto setCommon = [this, view, viewProj, horizon, fog, frame](ShaderMaterial& m, int translucent) {
        m.Set("uViewProj", viewProj);
        m.Set("uCameraPos", view.Position);
        m.Set("uTime", view.Time);
        m.Set("uFogColor", horizon);
        m.Set("uFog", fog);
        m.Set("uTranslucent", translucent);
        m.Set("uUnderwater", frame.Underwater ? 1 : 0);
        m.SetTexture("uAtlas", kAtlasUnit, m_atlas->Handle());
        m.SetTexture("uLightmap", kLightmapUnit, m_lightmap->Handle());
    };

    // --- Глухие блоки ---
    {
        RGPass p;
        p.Name = "Чанки";
        p.Reads = {meshes, lightmap};
        p.Writes = {scene};
        p.Execute = [this, visible, setCommon](RenderGraphContext& ctx) {
            ShaderMaterial& m = *m_chunkMat;
            setCommon(m, 0);
            if (!m.Apply(ctx.Device())) return;
            rhi::ShaderProgram* prog = m.Program();
            m_chunksDrawn = 0;
            m_facesDrawn = 0;
            for (int i : *visible) {
                const GpuChunk& g = m_gpu[(size_t)i];
                if (!g.Opaque || g.OpaqueIndices == 0) continue;
                const Chunk& c = *m_world.Chunks()[(size_t)i];
                prog->SetVec3("uChunkOrigin", glm::vec3(c.CX * kChunkSize, 0.0f, c.CZ * kChunkSize));
                g.Opaque->DrawIndexed(g.OpaqueIndices);
                ++m_chunksDrawn;
                m_facesDrawn += (int)(g.OpaqueIndices / 6);
            }
            RestoreDefaultState(ctx.Device());
        };
        graph.AddPass(std::move(p));
    }

    // --- Вода и стекло: от дальних к ближним ---
    {
        RGPass p;
        p.Name = "Вода и стекло";
        p.Reads = {meshes, lightmap};
        p.Writes = {scene};
        p.Execute = [this, visible, setCommon, view](RenderGraphContext& ctx) {
            std::vector<std::pair<float, int>> order;
            for (int i : *visible) {
                const GpuChunk& g = m_gpu[(size_t)i];
                if (!g.Translucent || g.TranslucentIndices == 0) continue;
                order.emplace_back(-glm::length(g.Center - view.Position), i);
            }
            std::sort(order.begin(), order.end());
            ShaderMaterial& m = *m_waterMat;
            setCommon(m, 1);
            if (!m.Apply(ctx.Device())) return;
            rhi::ShaderProgram* prog = m.Program();
            for (const auto& [dist, i] : order) {
                const Chunk& c = *m_world.Chunks()[(size_t)i];
                prog->SetVec3("uChunkOrigin", glm::vec3(c.CX * kChunkSize, 0.0f, c.CZ * kChunkSize));
                m_gpu[(size_t)i].Translucent->DrawIndexed(m_gpu[(size_t)i].TranslucentIndices);
                m_facesDrawn += (int)(m_gpu[(size_t)i].TranslucentIndices / 6);
            }
            RestoreDefaultState(ctx.Device());
        };
        graph.AddPass(std::move(p));
    }

    // --- Рамка блока под прицелом ---
    {
        RGPass p;
        p.Name = "Рамка блока";
        p.Writes = {scene};
        p.Enabled = frame.HasSelection;
        p.Execute = [this, viewProj, frame](RenderGraphContext& ctx) {
            ShaderMaterial& m = *m_outlineMat;
            m.Set("uViewProj", viewProj * glm::translate(glm::mat4(1.0f), glm::vec3(frame.Selection)));
            m.Set("uColor", glm::vec4(0.02f, 0.02f, 0.02f, 0.75f));
            if (m.Apply(ctx.Device())) m_outline->DrawLines(24);
            RestoreDefaultState(ctx.Device());
        };
        graph.AddPass(std::move(p));
    }

    // --- Своя пост-обработка: цепочка эффектов ---
    std::vector<EffectStep> fx;
    fx.push_back({"подводный", &*m_underwaterFx, frame.Underwater,
                  [view](ShaderMaterial& m, RenderGraphContext&) { m.Set("uTime", view.Time); }});
    fx.push_back({"тон и виньетка", &*m_finalFx, true, [](ShaderMaterial& m, RenderGraphContext&) {
                      m.Set("uExposure", 1.15f);
                      m.Set("uVignette", 0.55f);
                  }});
    AddEffectChain(graph, "Пост", scene, graph.Output(), fx);
}

} // namespace voxel
