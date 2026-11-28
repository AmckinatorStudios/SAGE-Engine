#include "sage/render/VRAMBudgetManager.h"

#include <algorithm>

namespace sage::render {

std::vector<size_t> VRAMBudgetManager::SelectEvictions(const std::vector<EvictCandidate>& candidates,
                                                        size_t currentBytes, size_t budget) {
    std::vector<size_t> result;
    if (budget == 0 || currentBytes <= budget) return result;
    std::vector<const EvictCandidate*> ordered;
    for (const auto& candidate : candidates)
        if (candidate.Evictable) ordered.push_back(&candidate);
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
        return a->Tick < b->Tick;
    });
    for (const EvictCandidate* candidate : ordered) {
        if (currentBytes <= budget) break;
        result.push_back(candidate->Index);
        currentBytes -= std::min(currentBytes, candidate->Bytes);
    }
    return result;
}

std::vector<size_t> VRAMBudgetManager::SelectDowngrades(
    const std::vector<DowngradeCandidate>& candidates, size_t currentBytes, size_t budget) {
    std::vector<size_t> chosen;
    if (budget == 0 || currentBytes <= budget) return chosen;
    std::vector<const DowngradeCandidate*> ordered;
    for (const auto& candidate : candidates)
        if (candidate.Streamable && candidate.Side > 64) ordered.push_back(&candidate);
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
        return a->Tick < b->Tick;
    });
    for (const DowngradeCandidate* candidate : ordered) {
        if (currentBytes <= budget) break;
        currentBytes -= std::min(currentBytes, candidate->Bytes - candidate->Bytes / 4);
        chosen.push_back(candidate->Index);
    }
    return chosen;
}

void VRAMBudgetManager::DownscaleRGBA(const std::vector<unsigned char>& src, int w, int h,
                                      std::vector<unsigned char>& dst, int& outW, int& outH) {
    outW = std::max(1, w / 2);
    outH = std::max(1, h / 2);
    dst.assign((size_t)outW * outH * 4u, 0);
    if (w <= 0 || h <= 0 || src.size() < (size_t)w * h * 4u) return;
    for (int y = 0; y < outH; ++y) for (int x = 0; x < outW; ++x) {
        const int sx = std::min(x * 2, w - 1), sy = std::min(y * 2, h - 1);
        const int sx1 = std::min(sx + 1, w - 1), sy1 = std::min(sy + 1, h - 1);
        for (int channel = 0; channel < 4; ++channel) {
            const int a = src[((size_t)sy * w + sx) * 4 + channel];
            const int b = src[((size_t)sy * w + sx1) * 4 + channel];
            const int c = src[((size_t)sy1 * w + sx) * 4 + channel];
            const int d = src[((size_t)sy1 * w + sx1) * 4 + channel];
            dst[((size_t)y * outW + x) * 4 + channel] = (unsigned char)((a + b + c + d + 2) / 4);
        }
    }
}

} // namespace sage::render
