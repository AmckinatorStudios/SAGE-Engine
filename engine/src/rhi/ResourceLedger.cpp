#include "sage/rhi/ResourceLedger.h"

#include <array>
#include <atomic>

namespace sage::rhi {
namespace {

// Атомарные счётчики, а не мьютекс: считать надо на каждом создании ресурса, в
// том числе в потоках фоновой загрузки, и цена учёта обязана быть неотличимой
// от нуля — иначе учёт начнёт менять то, что измеряет.
std::array<std::atomic<long long>, (size_t)ResourceKind::Count> g_live{};
std::array<std::atomic<long long>, (size_t)ResourceKind::Count> g_created{};

const char* KindName(ResourceKind kind) {
    switch (kind) {
        case ResourceKind::Shader:       return "шейдеры";
        case ResourceKind::Geometry:     return "геометрия";
        case ResourceKind::Texture:      return "текстуры";
        case ResourceKind::RenderTarget: return "буферы";
        case ResourceKind::Query:        return "запросы";
        default:                         return "?";
    }
}

} // namespace

void ResourceLedger::Acquired(ResourceKind kind) {
    const size_t i = (size_t)kind;
    if (i >= g_live.size()) return;
    g_live[i].fetch_add(1, std::memory_order_relaxed);
    g_created[i].fetch_add(1, std::memory_order_relaxed);
}

void ResourceLedger::Released(ResourceKind kind) {
    const size_t i = (size_t)kind;
    if (i >= g_live.size()) return;
    g_live[i].fetch_sub(1, std::memory_order_relaxed);
}

ResourceCounts ResourceLedger::Snapshot() {
    ResourceCounts c;
    for (size_t i = 0; i < g_live.size(); ++i) {
        c.Live[i] = g_live[i].load(std::memory_order_relaxed);
        c.Created[i] = g_created[i].load(std::memory_order_relaxed);
    }
    return c;
}

long long ResourceLedger::Live(ResourceKind kind) {
    const size_t i = (size_t)kind;
    return i < g_live.size() ? g_live[i].load(std::memory_order_relaxed) : 0;
}

long long ResourceCounts::LiveTotal() const {
    long long sum = 0;
    for (long long v : Live) sum += v;
    return sum;
}

long long ResourceCounts::CreatedTotal() const {
    long long sum = 0;
    for (long long v : Created) sum += v;
    return sum;
}

std::string ResourceCounts::ToString() const {
    std::string out;
    for (int i = 0; i < (int)ResourceKind::Count; ++i) {
        if (!out.empty()) out += ", ";
        out += KindName((ResourceKind)i);
        out += ' ';
        out += std::to_string(Live[i]);
    }
    return out;
}

std::string ResourceCounts::DiffFrom(const ResourceCounts& baseline) const {
    std::string out;
    for (int i = 0; i < (int)ResourceKind::Count; ++i) {
        const long long delta = Live[i] - baseline.Live[i];
        if (delta == 0) continue;
        if (!out.empty()) out += ", ";
        out += KindName((ResourceKind)i);
        out += ' ';
        if (delta > 0) out += '+';
        out += std::to_string(delta);
    }
    return out;
}

} // namespace sage::rhi
