#include "sage/render/RenderGraph.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "sage/core/Log.h"
#include "sage/rhi/GraphicsDevice.h"

namespace sage::render {
namespace {

const std::string kNoName = "<нет ресурса>";

// Сколько кадров цель пула может простаивать, прежде чем её освободят. Не
// ноль: проход, выключенный на кадр (эффект мигнул галочкой), не должен
// стоить пересоздания цели при включении обратно.
constexpr long long kPoolIdleFrames = 3;

const char* TypeName(RGPassType t) {
    switch (t) {
        case RGPassType::Graphics: return "графика";
        case RGPassType::Compute: return "вычисления";
        case RGPassType::Custom: return "свой";
    }
    return "?";
}

bool SameTarget(const RGTextureDesc& a, const RGTextureDesc& b) {
    return a.Kind == b.Kind && a.Samples == b.Samples;
}

} // namespace

// ============================================================================
//  RenderGraphContext
// ============================================================================

sage::rhi::RenderTarget* RenderGraphContext::Target(RGHandle h) const {
    if (h < 0 || h >= (RGHandle)m_graph->m_resources.size()) return nullptr;
    return m_graph->m_resources[(size_t)h].Target;
}

sage::rhi::TextureHandle RenderGraphContext::Color(RGHandle h) const {
    if (h < 0 || h >= (RGHandle)m_graph->m_resources.size()) return {};
    const auto& r = m_graph->m_resources[(size_t)h];
    if (r.Target) return r.Target->ColorTextureHandle();
    return r.Texture;
}

sage::rhi::TextureHandle RenderGraphContext::Depth(RGHandle h) const {
    sage::rhi::RenderTarget* t = Target(h);
    return t ? t->DepthTextureHandle() : sage::rhi::TextureHandle{};
}

sage::rhi::GpuBuffer* RenderGraphContext::Buffer(RGHandle h) const {
    if (h < 0 || h >= (RGHandle)m_graph->m_resources.size()) return nullptr;
    return m_graph->m_resources[(size_t)h].Buffer;
}

void RenderGraphContext::Bind(RGHandle h) const { m_graph->BindResource(h); }

// ============================================================================
//  RenderGraph
// ============================================================================

RenderGraph::RenderGraph(sage::rhi::GraphicsDevice* device) : m_device(device) {}

RenderGraph::~RenderGraph() {
    // Запросы есть, только если граф выполнялся, — а значит, устройство живо.
    for (auto& ring : m_gpu) {
        for (GpuQueries& q : ring) {
            Dev().DestroyTimestampQuery(q.Begin);
            Dev().DestroyTimestampQuery(q.End);
        }
    }
}

sage::rhi::GraphicsDevice& RenderGraph::Dev() const {
    return m_device ? *m_device : sage::rhi::GraphicsDevice::Get();
}

void RenderGraph::Reset(int width, int height, sage::rhi::RenderTarget* output, int x, int y) {
    m_resources.clear();
    m_passes.clear();
    m_order.clear();
    m_timings.clear();
    m_compiled = false;
    m_width = std::max(width, 1);
    m_height = std::max(height, 1);
    m_outX = x;
    m_outY = y;
    ++m_frame;

    Resource out;
    out.Name = "Выход";
    out.ResKind = Kind::Output;
    out.Target = output;
    m_output = Add(std::move(out));

    // Пул: то, чем давно не пользовались, — освободить.
    for (auto it = m_targets.begin(); it != m_targets.end();) {
        if (m_frame - it->second.LastFrame > kPoolIdleFrames) it = m_targets.erase(it);
        else ++it;
    }
    for (auto it = m_buffers.begin(); it != m_buffers.end();) {
        if (m_frame - it->second.LastFrame > kPoolIdleFrames) it = m_buffers.erase(it);
        else ++it;
    }
}

RGHandle RenderGraph::Add(Resource r) {
    m_resources.push_back(std::move(r));
    m_compiled = false;
    return (RGHandle)m_resources.size() - 1;
}

RGHandle RenderGraph::CreateTexture(const std::string& name, const RGTextureDesc& desc) {
    Resource r;
    r.Name = name;
    r.ResKind = Kind::Texture;
    r.Tex = desc;
    return Add(std::move(r));
}

RGHandle RenderGraph::CreateBuffer(const std::string& name, const RGBufferDesc& desc) {
    Resource r;
    r.Name = name;
    r.ResKind = Kind::Buffer;
    r.Buf = desc;
    return Add(std::move(r));
}

RGHandle RenderGraph::ImportTarget(const std::string& name, sage::rhi::RenderTarget* target) {
    Resource r;
    r.Name = name;
    r.ResKind = Kind::ImportedTarget;
    r.Target = target;
    return Add(std::move(r));
}

RGHandle RenderGraph::ImportTexture(const std::string& name, sage::rhi::TextureHandle texture) {
    Resource r;
    r.Name = name;
    r.ResKind = Kind::ImportedTexture;
    r.Texture = texture;
    return Add(std::move(r));
}

RGHandle RenderGraph::ImportBuffer(const std::string& name, sage::rhi::GpuBuffer* buffer) {
    Resource r;
    r.Name = name;
    r.ResKind = Kind::ImportedBuffer;
    r.Buffer = buffer;
    return Add(std::move(r));
}

RGHandle RenderGraph::DeclareVirtual(const std::string& name) {
    Resource r;
    r.Name = name;
    r.ResKind = Kind::Virtual;
    return Add(std::move(r));
}

RGHandle RenderGraph::Find(const std::string& name) const {
    for (size_t i = 0; i < m_resources.size(); ++i)
        if (m_resources[i].Name == name) return (RGHandle)i;
    return kNoHandle;
}

const std::string& RenderGraph::ResourceName(RGHandle h) const {
    if (h < 0 || h >= (RGHandle)m_resources.size()) return kNoName;
    return m_resources[(size_t)h].Name;
}

void RenderGraph::AddPass(RGPass pass) {
    m_passes.push_back(std::move(pass));
    m_compiled = false;
}

bool RenderGraph::AddPassBefore(const std::string& anchor, RGPass pass) {
    for (size_t i = 0; i < m_passes.size(); ++i) {
        if (m_passes[i].Name == anchor) {
            m_passes.insert(m_passes.begin() + (long)i, std::move(pass));
            m_compiled = false;
            return true;
        }
    }
    LOG_WARN("RenderGraph") << "нет прохода «" << anchor << "» — «" << pass.Name
                            << "» добавлен в конец кадра";
    AddPass(std::move(pass));
    return false;
}

bool RenderGraph::AddPassAfter(const std::string& anchor, RGPass pass) {
    for (size_t i = 0; i < m_passes.size(); ++i) {
        if (m_passes[i].Name == anchor) {
            m_passes.insert(m_passes.begin() + (long)i + 1, std::move(pass));
            m_compiled = false;
            return true;
        }
    }
    LOG_WARN("RenderGraph") << "нет прохода «" << anchor << "» — «" << pass.Name
                            << "» добавлен в конец кадра";
    AddPass(std::move(pass));
    return false;
}

bool RenderGraph::HasPass(const std::string& name) const {
    for (const RGPass& p : m_passes)
        if (p.Name == name) return true;
    return false;
}

bool RenderGraph::DisablePass(const std::string& name) {
    for (RGPass& p : m_passes) {
        if (p.Name == name) {
            p.Enabled = false;
            m_compiled = false;
            return true;
        }
    }
    return false;
}

bool RenderGraph::Compile(std::string& error) {
    m_compiled = false;
    m_order.clear();
    m_stats = RGStats{};
    m_stats.Passes = (int)m_passes.size();

    // Модель версий и отбрасывание лишнего — FrameGraph. Номера ресурсов
    // совпадают: объявляем их в том же порядке.
    m_check.Reset();
    for (const Resource& r : m_resources) m_check.DeclareResource(r.Name);

    // Импортированное существует ДО кадра — его «записал» мир снаружи.
    // Проход-заглушка на каждый такой ресурс делает это видимым модели версий.
    int imports = 0;
    for (size_t i = 0; i < m_resources.size(); ++i) {
        const Kind k = m_resources[i].ResKind;
        if (k == Kind::ImportedTarget || k == Kind::ImportedTexture || k == Kind::ImportedBuffer) {
            RenderPassDesc imp{"Импорт: " + m_resources[i].Name};
            imp.Writes = {(ResourceId)i};
            m_check.AddPass(std::move(imp));
            ++imports;
        }
    }

    std::vector<bool> written(m_resources.size(), false);
    for (size_t i = 0; i < m_resources.size(); ++i) {
        const Kind k = m_resources[i].ResKind;
        written[i] = k == Kind::ImportedTarget || k == Kind::ImportedTexture || k == Kind::ImportedBuffer;
    }
    for (const RGPass& p : m_passes) {
        RenderPassDesc d{p.Name};
        d.Reads = p.Reads;
        d.Writes = p.Writes;
        d.Enabled = p.Enabled;
        // Дорисовка поверх (LoadOp::Load) — это ЧТЕНИЕ прежнего содержимого.
        // Без него проход, очистивший цель раньше, выглядел бы ненужным
        // выходу и выбрасывался: небо пропало бы из кадра, как только поверх
        // него рисует кто-то ещё.
        if (p.Type == RGPassType::Graphics && p.Enabled) {
            RGHandle target = p.Target;
            if (target == kNoHandle && !p.Writes.empty()) target = p.Writes.front();
            if (target >= 0 && target < (RGHandle)written.size() && written[(size_t)target] &&
                p.ColorLoad == LoadOp::Load &&
                std::find(d.Reads.begin(), d.Reads.end(), target) == d.Reads.end())
                d.Reads.push_back(target);
        }
        if (p.Enabled)
            for (RGHandle w : p.Writes)
                if (w >= 0 && w < (RGHandle)written.size()) written[(size_t)w] = true;
        m_check.AddPass(std::move(d));
    }

    if (!m_check.Compile(m_output, error)) return false;
    for (int idx : m_check.Order())
        if (idx >= imports) m_order.push_back(idx - imports);

    m_stats.Executed = (int)m_order.size();
    for (const RGPass& p : m_passes)
        if (!p.Enabled) ++m_stats.Disabled;
    m_stats.Culled = m_stats.Passes - m_stats.Executed - m_stats.Disabled;

    Allocate(m_order);
    m_compiled = true;
    return true;
}

void RenderGraph::Allocate(const std::vector<int>& order) {
    std::vector<bool> used(m_resources.size(), false);
    for (int i : order) {
        const RGPass& p = m_passes[(size_t)i];
        for (RGHandle h : p.Reads) if (h >= 0) used[(size_t)h] = true;
        for (RGHandle h : p.Writes) if (h >= 0) used[(size_t)h] = true;
        if (p.Target >= 0) used[(size_t)p.Target] = true;
    }
    for (size_t i = 0; i < m_resources.size(); ++i) {
        Resource& r = m_resources[i];
        if (!used[i]) continue;
        if (r.ResKind == Kind::Texture) {
            const int w = r.Tex.Width > 0 ? r.Tex.Width
                                          : std::max(1, (int)std::lround(m_width * r.Tex.Scale));
            const int h = r.Tex.Height > 0 ? r.Tex.Height
                                           : std::max(1, (int)std::lround(m_height * r.Tex.Scale));
            PooledTarget& slot = m_targets[r.Name];
            if (!slot.Target || !SameTarget(slot.Desc, r.Tex)) {
                sage::rhi::RenderTargetDesc d;
                d.Width = w;
                d.Height = h;
                d.Kind = r.Tex.Kind;
                d.Samples = r.Tex.Samples;
                slot.Target = Dev().CreateRenderTarget(d);
                slot.Desc = r.Tex;
                ++m_stats.CreatedThisFrame;
            } else if (slot.Target->Width() != w || slot.Target->Height() != h) {
                slot.Target->Resize(w, h);
            }
            slot.LastFrame = m_frame;
            r.Target = slot.Target.get();
        } else if (r.ResKind == Kind::Buffer) {
            PooledBuffer& slot = m_buffers[r.Name];
            if (!slot.Buffer || slot.Desc.Bytes != r.Buf.Bytes || slot.Desc.Usage != r.Buf.Usage) {
                sage::rhi::BufferDesc d;
                d.Bytes = r.Buf.Bytes;
                d.Usage = r.Buf.Usage;
                slot.Buffer = Dev().CreateBuffer(d, nullptr);
                slot.Desc = r.Buf;
                ++m_stats.CreatedThisFrame;
                if (!slot.Buffer)
                    LOG_ERROR("RenderGraph") << "буфер «" << r.Name << "» не создан: устройство "
                                             << Dev().BackendName() << " не умеет буферы шейдеров";
            }
            slot.LastFrame = m_frame;
            r.Buffer = slot.Buffer.get();
        }
    }
    m_stats.PooledTargets = (int)m_targets.size();
    m_stats.PooledBuffers = (int)m_buffers.size();
}

void RenderGraph::BindResource(RGHandle h) const {
    sage::rhi::GraphicsDevice& device = Dev();
    if (h < 0 || h >= (RGHandle)m_resources.size()) return;
    const Resource& r = m_resources[(size_t)h];
    if (r.Target) {
        r.Target->Bind();
        device.SetViewport(0, 0, r.Target->Width(), r.Target->Height());
    } else if (r.ResKind == Kind::Output) {
        device.BindDefaultFramebuffer();
        device.SetViewport(m_outX, m_outY, m_width, m_height);
    }
}

void RenderGraph::HarvestGpuTimes() {
    sage::rhi::GraphicsDevice& device = Dev();
    std::vector<GpuQueries>& ring = m_gpu[m_frame % kGpuRing];
    for (GpuQueries& q : ring) {
        if (!q.Pending) continue;
        if (device.TimestampReady(q.End) && device.TimestampReady(q.Begin)) {
            const unsigned long long a = device.TimestampNs(q.Begin);
            const unsigned long long b = device.TimestampNs(q.End);
            m_gpuMs[q.Name] = b > a ? (double)(b - a) / 1.0e6 : 0.0;
        }
        q.Pending = false;
    }
}

void RenderGraph::Execute() {
    if (!m_compiled) {
        LOG_ERROR("RenderGraph") << "Execute без успешного Compile — кадр пропущен";
        return;
    }
    sage::rhi::GraphicsDevice& device = Dev();
    const bool gpu = m_gpuTiming && device.SupportsGpuTimers();
    if (gpu) HarvestGpuTimes();
    std::vector<GpuQueries>& ring = m_gpu[m_frame % kGpuRing];
    size_t queryIndex = 0;

    m_timings.clear();
    std::vector<bool> runs(m_passes.size(), false);
    for (int i : m_order) runs[(size_t)i] = true;

    RenderGraphContext ctx;
    ctx.m_graph = this;
    ctx.m_device = &device;
    ctx.m_width = m_width;
    ctx.m_height = m_height;

    for (size_t i = 0; i < m_passes.size(); ++i) {
        RGPass& p = m_passes[i];
        RGPassTiming t;
        t.Name = p.Name;
        t.Type = p.Type;
        t.Executed = runs[i];
        if (!runs[i]) {
            m_timings.push_back(std::move(t));
            continue;
        }
        if (gpu) {
            if (queryIndex >= ring.size()) {
                GpuQueries q;
                q.Begin = device.CreateTimestampQuery();
                q.End = device.CreateTimestampQuery();
                ring.push_back(q);
            }
            ring[queryIndex].Name = p.Name;
            device.WriteTimestamp(ring[queryIndex].Begin);
        }
        const auto start = std::chrono::steady_clock::now();

        if (p.Type == RGPassType::Graphics) {
            RGHandle target = p.Target;
            if (target == kNoHandle && !p.Writes.empty()) target = p.Writes.front();
            BindResource(target);
            const bool clearColor = p.ColorLoad == LoadOp::Clear;
            const bool clearDepth = p.DepthLoad == LoadOp::Clear;
            if (clearColor || clearDepth) {
                if (clearColor)
                    device.SetClearColor(p.ClearColor.r, p.ClearColor.g, p.ClearColor.b,
                                         p.ClearColor.a);
                // Очистка глубины требует разрешённой записи глубины — иначе
                // драйвер молча оставляет прежнюю.
                device.SetDepthWrite(true);
                device.Clear(clearColor, clearDepth);
            }
        }
        ctx.m_pass = &p.Name;
        if (p.Execute) p.Execute(ctx);
        if (p.Type == RGPassType::Compute) device.ShaderBarrier(sage::rhi::BarrierBits::All);

        t.CpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                      .count();
        if (gpu) {
            device.WriteTimestamp(ring[queryIndex].End);
            ring[queryIndex].Pending = true;
            ++queryIndex;
        }
        auto it = m_gpuMs.find(p.Name);
        if (it != m_gpuMs.end()) t.GpuMs = it->second;
        m_timings.push_back(std::move(t));
    }
}

std::string RenderGraph::Describe() const {
    std::string out;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "Кадр %dx%d: проходов %d, выполнено %d, отброшено %d, выключено %d\n",
                  m_width, m_height, m_stats.Passes, m_stats.Executed, m_stats.Culled,
                  m_stats.Disabled);
    out += buf;
    std::vector<bool> runs(m_passes.size(), false);
    for (int i : m_order) runs[(size_t)i] = true;
    for (size_t i = 0; i < m_passes.size(); ++i) {
        const RGPass& p = m_passes[i];
        const char* state = !p.Enabled ? "выключен" : (runs[i] ? "выполнен" : "отброшен");
        out += "  [" + std::string(state) + "] " + p.Name + " (" + TypeName(p.Type) + ")";
        if (!p.Reads.empty()) {
            out += " читает:";
            for (RGHandle h : p.Reads) out += " " + ResourceName(h);
        }
        if (!p.Writes.empty()) {
            out += " пишет:";
            for (RGHandle h : p.Writes) out += " " + ResourceName(h);
        }
        for (const RGPassTiming& t : m_timings) {
            if (t.Name != p.Name || !t.Executed) continue;
            std::snprintf(buf, sizeof(buf), " — CPU %.3f мс", t.CpuMs);
            out += buf;
            if (t.GpuMs >= 0.0) {
                std::snprintf(buf, sizeof(buf), ", GPU %.3f мс", t.GpuMs);
                out += buf;
            }
            break;
        }
        out += "\n";
    }
    return out;
}

void RenderGraph::ReleasePool() {
    m_targets.clear();
    m_buffers.clear();
    for (Resource& r : m_resources) {
        if (r.ResKind == Kind::Texture) r.Target = nullptr;
        if (r.ResKind == Kind::Buffer) r.Buffer = nullptr;
    }
    m_compiled = false;
}

} // namespace sage::render
