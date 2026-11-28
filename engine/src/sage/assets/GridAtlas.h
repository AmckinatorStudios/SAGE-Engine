#pragma once

#include <cmath>
#include <cstddef>

namespace sage::assets {

inline constexpr size_t kMaxGridAtlasFrames = 4096;
inline constexpr size_t kGridAtlasBudget = 64u << 20;

struct GridAtlasLayout {
    int Columns = 0;
    int Rows = 0;
    int Width = 0;
    int Height = 0;
    size_t Bytes = 0;
};

inline bool PlanGridAtlas(size_t frames, int frameWidth, int frameHeight, int maxDimension,
                              size_t bytesLimit, GridAtlasLayout& layout) {
    if (frames == 0 || frames > kMaxGridAtlasFrames || frameWidth <= 0 || frameHeight <= 0)
        return false;
    const int count = static_cast<int>(frames);
    const int columns = static_cast<int>(std::ceil(std::sqrt(count)));
    const int rows = (count + columns - 1) / columns;
    if (frameWidth > maxDimension / columns || frameHeight > maxDimension / rows) return false;
    const int width = columns * frameWidth;
    const int height = rows * frameHeight;
    if (size_t(width) > bytesLimit / 4 / size_t(height)) return false;
    layout = {columns, rows, width, height, size_t(width) * size_t(height) * 4};
    return true;
}

} // namespace sage::assets
