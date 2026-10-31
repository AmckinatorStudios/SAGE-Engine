#pragma once
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "sage/render/FrameGraph.h"
#include "sage/rhi/Resources.h"

namespace sage::rhi {
class GraphicsDevice;
}

// ---------------------------------------------------------------------------
// RenderGraph — кадр своего рендера: проходы, ресурсы, которые граф СОЗДАЁТ
// сам, и зависимости между ними.
//
// ЧЕМ ОТЛИЧАЕТСЯ ОТ FrameGraph. FrameGraph (FrameGraph.h) знает о ресурсах
// только имена: цели у потребителя свои, и граф лишь проверяет порядок и
// выбрасывает лишнее. Своему рендеру этого мало — ему нужно сказать «HDR-цель
// размером в экран» и получить её, а не заводить, пересоздавать при смене
// окна и освобождать руками. Здесь у ресурса есть ОПИСАНИЕ, и граф:
//   • создаёт цели и буферы, которые нужны выполняемым проходам, и держит их
//     между кадрами в пуле (кадр не выделяет видеопамять, пока описание то же);
//   • перед графическим проходом привязывает его цель и выполняет LoadOp
//     (очистку), после вычислительного — ставит барьер памяти;
//   • меряет каждый проход (CPU всегда, GPU — где есть метки времени);
//   • даёт вставить проход ДО или ПОСЛЕ чужого по имени — так дополняют
//     встроенный рендер, не переписывая его (RenderPipeline.h).
//
// Проверка порядка и отбрасывание лишнего — те же, что у FrameGraph: граф
// строит FrameGraph под капотом, и правила версий ресурсов у них одни.
//
// ЧЕГО ОН НЕ ДЕЛАЕТ. Не переставляет проходы (порядок задаёт автор кадра, см.
// FrameGraph.h) и не делит память между ресурсами (aliasing). Барьер после
// вычислительного прохода — полный, а не вычисленный по читателям: в OpenGL
// он дешёв, а ошибка в «точном» барьере — это мерцание раз в сотню кадров.
// ---------------------------------------------------------------------------
namespace sage::render {

using RGHandle = int;
constexpr RGHandle kNoHandle = -1;

// Цель, которую создаёт граф. Width/Height == 0 — размер выхода графа,
// умноженный на Scale (половинное разрешение — Scale = 0.5).
struct RGTextureDesc {
    int Width = 0;
    int Height = 0;
    float Scale = 1.0f;
    sage::rhi::RenderTargetKind Kind = sage::rhi::RenderTargetKind::ColorHDRWithDepth;
    int Samples = 1;
};

struct RGBufferDesc {
    size_t Bytes = 0;
    sage::rhi::BufferUsage Usage = sage::rhi::BufferUsage::Storage;
};

enum class RGPassType {
    Graphics, // граф привязывает цель (Target или первую записываемую) и чистит по LoadOp
    Compute,  // без цели; после прохода — барьер памяти
    Custom,   // граф ничего не привязывает: проход сам решает, куда рисовать
};

class RenderGraph;

// То, что видит тело прохода: устройство, настоящие объекты ресурсов, размер.
class RenderGraphContext {
public:
    sage::rhi::GraphicsDevice& Device() const { return *m_device; }
    const std::string& PassName() const { return *m_pass; }
    // Размер выхода графа (экрана или цели вида).
    int Width() const { return m_width; }
    int Height() const { return m_height; }

    sage::rhi::RenderTarget* Target(RGHandle h) const;
    sage::rhi::TextureHandle Color(RGHandle h) const;  // цвет цели или импортированная текстура
    sage::rhi::TextureHandle Depth(RGHandle h) const;
    sage::rhi::GpuBuffer* Buffer(RGHandle h) const;
    // Привязать цель (или экран) как место отрисовки, вместе с viewport.
    void Bind(RGHandle h) const;

private:
    friend class RenderGraph;
    const RenderGraph* m_graph = nullptr;
    sage::rhi::GraphicsDevice* m_device = nullptr;
    const std::string* m_pass = nullptr;
    int m_width = 0, m_height = 0;
};

struct RGPass {
    std::string Name;
    RGPassType Type = RGPassType::Graphics;
    std::vector<RGHandle> Reads;
    std::vector<RGHandle> Writes;
    // Куда рисует графический проход. kNoHandle — первая записываемая цель.
    RGHandle Target = kNoHandle;
    LoadOp ColorLoad = LoadOp::Load;
    LoadOp DepthLoad = LoadOp::Load;
    glm::vec4 ClearColor{0.0f, 0.0f, 0.0f, 1.0f};
    std::function<void(RenderGraphContext&)> Execute;
    bool Enabled = true;
};

struct RGPassTiming {
    std::string Name;
    RGPassType Type = RGPassType::Graphics;
    bool Executed = false;
    double CpuMs = 0.0;
    double GpuMs = -1.0; // < 0 — неизвестно (нет меток времени или ещё не пришли)
};

struct RGStats {
    int Passes = 0;
    int Executed = 0;
    int Culled = 0;
    int Disabled = 0;
    int PooledTargets = 0;   // живых целей в пуле
    int PooledBuffers = 0;
    int CreatedThisFrame = 0; // выделено за кадр (в устоявшемся кадре — 0)
};

class RenderGraph {
public:
    explicit RenderGraph(sage::rhi::GraphicsDevice* device = nullptr);
    ~RenderGraph();
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    // Начало кадра: забыть проходы и ресурсы прошлого кадра (пул остаётся).
    // output == nullptr — выход графа экран, в прямоугольник (x, y, w, h).
    void Reset(int width, int height, sage::rhi::RenderTarget* output = nullptr, int x = 0,
               int y = 0);
    // Выход графа — то, что должен записать последний проход кадра.
    RGHandle Output() const { return m_output; }
    int Width() const { return m_width; }
    int Height() const { return m_height; }

    RGHandle CreateTexture(const std::string& name, const RGTextureDesc& desc);
    RGHandle CreateBuffer(const std::string& name, const RGBufferDesc& desc);
    // Чужие ресурсы: их содержимое существует до кадра (карта, собранная
    // игрой; буфер чанков), граф ими не владеет.
    RGHandle ImportTarget(const std::string& name, sage::rhi::RenderTarget* target);
    RGHandle ImportTexture(const std::string& name, sage::rhi::TextureHandle texture);
    RGHandle ImportBuffer(const std::string& name, sage::rhi::GpuBuffer* buffer);
    // Ресурс без объекта — только имя для зависимостей («Тени», которые
    // проход держит у себя сам).
    RGHandle DeclareVirtual(const std::string& name);
    RGHandle Find(const std::string& name) const;
    const std::string& ResourceName(RGHandle h) const;

    void AddPass(RGPass pass);
    // Вставить проход перед/после прохода с именем anchor. false — такого нет
    // (проход тогда добавляется в конец: потеря места лучше потери прохода).
    bool AddPassBefore(const std::string& anchor, RGPass pass);
    bool AddPassAfter(const std::string& anchor, RGPass pass);
    bool HasPass(const std::string& name) const;
    // Выключить проход по имени (заменить штатный своим — выключить и вставить).
    bool DisablePass(const std::string& name);

    // Проверка порядка, отбрасывание лишнего, выделение ресурсов.
    bool Compile(std::string& error);
    void Execute();

    void SetGpuTiming(bool on) { m_gpuTiming = on; }
    const std::vector<RGPassTiming>& Timings() const { return m_timings; }
    const RGStats& Stats() const { return m_stats; }
    std::string Describe() const;
    const std::vector<RGPass>& Passes() const { return m_passes; }

    // Освободить пул (смена рендерера, выход из игры).
    void ReleasePool();

private:
    friend class RenderGraphContext;
    enum class Kind { Texture, Buffer, Output, ImportedTarget, ImportedTexture, ImportedBuffer, Virtual };
    struct Resource {
        std::string Name;
        Kind ResKind = Kind::Virtual;
        RGTextureDesc Tex;
        RGBufferDesc Buf;
        sage::rhi::RenderTarget* Target = nullptr;
        sage::rhi::TextureHandle Texture;
        sage::rhi::GpuBuffer* Buffer = nullptr;
    };
    struct PooledTarget {
        RGTextureDesc Desc;
        std::unique_ptr<sage::rhi::RenderTarget> Target;
        long long LastFrame = 0;
    };
    struct PooledBuffer {
        RGBufferDesc Desc;
        std::unique_ptr<sage::rhi::GpuBuffer> Buffer;
        long long LastFrame = 0;
    };
    struct GpuQueries {
        std::string Name;
        sage::rhi::QueryHandle Begin, End;
        bool Pending = false;
    };

    RGHandle Add(Resource r);
    sage::rhi::GraphicsDevice& Dev() const;
    void Allocate(const std::vector<int>& order);
    void BindResource(RGHandle h) const;
    void HarvestGpuTimes();

    sage::rhi::GraphicsDevice* m_device = nullptr;
    std::vector<Resource> m_resources;
    std::vector<RGPass> m_passes;
    std::vector<int> m_order;
    std::vector<RGPassTiming> m_timings;
    RGStats m_stats;
    RGHandle m_output = kNoHandle;
    int m_width = 0, m_height = 0, m_outX = 0, m_outY = 0;
    bool m_compiled = false;
    long long m_frame = 0;
    FrameGraph m_check; // проверка порядка — та же модель версий, что у FrameGraph

    std::unordered_map<std::string, PooledTarget> m_targets;
    std::unordered_map<std::string, PooledBuffer> m_buffers;

    bool m_gpuTiming = true;
    static constexpr int kGpuRing = 4;
    std::vector<GpuQueries> m_gpu[kGpuRing];
    std::unordered_map<std::string, double> m_gpuMs;
};

} // namespace sage::render
