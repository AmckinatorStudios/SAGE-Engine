#include "sage/render/RenderPipeline.h"

#include <mutex>

#include "sage/core/Log.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/StandardPipeline.h"
#include "sage/rhi/GraphicsDevice.h"

namespace sage::render {
namespace {

struct Registry {
    std::mutex Lock;
    std::vector<PipelineInfo> Items;
    bool Builtins = false;
};

Registry& Reg() {
    static Registry r;
    return r;
}

void EnsureBuiltins() {
    bool need = false;
    {
        std::lock_guard<std::mutex> lock(Reg().Lock);
        need = !Reg().Builtins;
        Reg().Builtins = true;
    }
    if (need) RenderPipelineRegistry::RegisterBuiltins();
}

} // namespace

void RenderPipelineRegistry::Register(PipelineInfo info) {
    Registry& r = Reg();
    std::lock_guard<std::mutex> lock(r.Lock);
    for (PipelineInfo& existing : r.Items) {
        if (existing.Id == info.Id) {
            existing = std::move(info);
            return;
        }
    }
    r.Items.push_back(std::move(info));
}

const PipelineInfo* RenderPipelineRegistry::Find(const std::string& id) {
    EnsureBuiltins();
    Registry& r = Reg();
    std::lock_guard<std::mutex> lock(r.Lock);
    for (const PipelineInfo& i : r.Items)
        if (i.Id == id) return &i;
    return nullptr;
}

std::vector<PipelineInfo> RenderPipelineRegistry::All() {
    EnsureBuiltins();
    Registry& r = Reg();
    std::lock_guard<std::mutex> lock(r.Lock);
    return r.Items;
}

std::unique_ptr<RenderPipeline> RenderPipelineRegistry::Create(const std::string& id) {
    const PipelineInfo* info = Find(id);
    if (!info || !info->Create) return nullptr;
    return info->Create();
}

void RenderPipelineRegistry::RegisterBuiltins() {
    {
        std::lock_guard<std::mutex> lock(Reg().Lock);
        Reg().Builtins = true;
    }
    PipelineInfo pbr;
    pbr.Id = StandardPipeline::kId;
    pbr.Label = "SAGE PBR";
    pbr.Description = "Встроенный рендер: тени, PBR-освещение сцены, небо, пост-обработка.";
    pbr.Create = [] { return std::make_unique<StandardPipeline>(); };
    Register(std::move(pbr));
}

// ============================================================================
//  PipelineRenderer
// ============================================================================

PipelineRenderer::PipelineRenderer(std::unique_ptr<RenderPipeline> pipeline,
                                   sage::rhi::GraphicsDevice* device)
    : m_device(device), m_graph(device) {
    SetPipeline(std::move(pipeline));
}

PipelineRenderer::~PipelineRenderer() {
    if (m_pipeline) m_pipeline->Shutdown();
}

sage::rhi::GraphicsDevice& PipelineRenderer::Dev() const {
    return m_device ? *m_device : sage::rhi::GraphicsDevice::Get();
}

bool PipelineRenderer::SetPipeline(std::unique_ptr<RenderPipeline> pipeline) {
    if (m_pipeline) m_pipeline->Shutdown();
    m_graph.ReleasePool();
    m_pipeline = std::move(pipeline);
    m_ready = false;
    m_error.clear();
    m_reportedError.clear();
    if (!m_pipeline) {
        m_error = "рендерер не задан";
        return false;
    }
    std::string err;
    m_ready = m_pipeline->Initialize(Dev(), err);
    if (!m_ready) {
        m_error = m_pipeline->Id() + ": " + (err.empty() ? "не инициализировался" : err);
        LOG_ERROR("Render") << m_error;
    } else {
        LOG_INFO("Render") << "Рендерер: " << m_pipeline->Id();
    }
    return m_ready;
}

void PipelineRenderer::AddExtension(const std::string& name, GraphExtension extension) {
    RemoveExtension(name);
    m_extensions.emplace_back(name, std::move(extension));
}

void PipelineRenderer::RemoveExtension(const std::string& name) {
    for (auto it = m_extensions.begin(); it != m_extensions.end(); ++it) {
        if (it->first == name) {
            m_extensions.erase(it);
            return;
        }
    }
}

bool PipelineRenderer::Render(const RenderView& view) {
    if (!m_ready) return false;
    sage::rhi::RenderTarget* out = view.Output ? &view.Output->Target() : nullptr;
    m_graph.Reset(view.Width, view.Height, out, view.ViewportX, view.ViewportY);
    m_pipeline->Build(m_graph, view);
    for (auto& [name, ext] : m_extensions) ext(m_graph, view);

    std::string err;
    if (!m_graph.Compile(err)) {
        m_error = err;
        // Ошибка описания кадра повторяется каждый кадр — в лог один раз.
        if (m_reportedError != err) {
            m_reportedError = err;
            LOG_ERROR("Render") << m_pipeline->Id() << ": кадр не собран: " << err;
        }
        return false;
    }
    m_error.clear();
    m_reportedError.clear();
    m_graph.Execute();
    return true;
}

} // namespace sage::render
