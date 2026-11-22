#include "sage/render/RenderPipeline.h"

#include <mutex>

#include "sage/core/Log.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/StandardPipeline.h"
#include "sage/rhi/GraphicsDevice.h"

namespace sage::render {
namespace {

PipelineInfo BuiltinPbr() {
    PipelineInfo pbr;
    pbr.Id = StandardPipeline::kId;
    pbr.Label = "SAGE PBR";
    pbr.Description = "Встроенный рендер: тени, PBR-освещение сцены, небо, пост-обработка.";
    pbr.Create = [] { return std::make_unique<StandardPipeline>(); };
    return pbr;
}

// Встроенный PBR кладётся в реестр В КОНСТРУКТОРЕ статика: инициализация
// локального статика в C++11 потокобезопасна и однократна. Прежняя схема
// (флаг «встроенные есть» ставился ДО регистрации, сама регистрация — после
// снятия блокировки) давала два сбоя: второй поток мог увидеть флаг и не
// найти "sage.pbr", а подмена PBR игрой ДО первого Find затиралась встроенным.
struct Registry {
    std::mutex Lock;
    std::vector<PipelineInfo> Items;
    Registry() { Items.push_back(BuiltinPbr()); }
};

Registry& Reg() {
    static Registry r;
    return r;
}

} // namespace

bool RenderPipelineRegistry::Register(PipelineInfo info) {
    if (info.Id.empty() || !info.Create) {
        LOG_ERROR("Render") << "RenderPipelineRegistry: запись отвергнута — "
                            << (info.Id.empty() ? "пустой Id" : "нет фабрики у '" + info.Id + "'");
        return false;
    }
    Registry& r = Reg();
    std::lock_guard<std::mutex> lock(r.Lock);
    for (PipelineInfo& existing : r.Items) {
        if (existing.Id == info.Id) {
            existing = std::move(info);
            return true;
        }
    }
    r.Items.push_back(std::move(info));
    return true;
}

std::optional<PipelineInfo> RenderPipelineRegistry::Find(const std::string& id) {
    Registry& r = Reg();
    std::lock_guard<std::mutex> lock(r.Lock);
    for (const PipelineInfo& i : r.Items)
        if (i.Id == id) return i;
    return std::nullopt;
}

std::vector<PipelineInfo> RenderPipelineRegistry::All() {
    Registry& r = Reg();
    std::lock_guard<std::mutex> lock(r.Lock);
    return r.Items;
}

std::unique_ptr<RenderPipeline> RenderPipelineRegistry::Create(const std::string& id) {
    // Копия фабрики берётся под блокировкой, вызывается — без неё.
    std::function<std::unique_ptr<RenderPipeline>()> factory;
    {
        Registry& r = Reg();
        std::lock_guard<std::mutex> lock(r.Lock);
        for (const PipelineInfo& i : r.Items)
            if (i.Id == id) {
                factory = i.Create;
                break;
            }
    }
    return factory ? factory() : nullptr;
}

void RenderPipelineRegistry::RegisterBuiltins() { (void)Reg(); }

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
