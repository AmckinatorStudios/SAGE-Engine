#include "sage/render/LightCookies.h"

#include <algorithm>
#include <cmath>

#include "sage/assets/AssetDatabase.h"
#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Shader.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Light.h"

namespace sage::render {

glm::vec4 CookieTileRect(int slot, int cols) {
    if (cols <= 0 || slot < 0) return glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
    const float size = 1.0f / (float)cols;
    return glm::vec4((float)(slot % cols) * size, (float)(slot / cols) * size, size, size);
}

LightCookies& LightCookies::Current() {
    static LightCookies cookies;
    return cookies;
}

int LightCookies::SlotOf(const std::string& path) const {
    if (path.empty()) return -1;
    auto it = m_slots.find(path);
    return it == m_slots.end() ? -1 : it->second;
}

int LightCookies::Prepare(const LightingEnvironment& env, int maxTextureSize) {
    std::vector<std::string> paths;
    for (const PointLight& p : env.PointLights)
        if (!p.Cookie.empty()) paths.push_back(p.Cookie);
    for (const SpotLight& s : env.SpotLights)
        if (!s.Cookie.empty()) paths.push_back(s.Cookie);
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    if (paths == m_paths) return (int)m_slots.size();
    m_paths = paths;
    m_slots.clear();

    struct Image { std::string Path; std::vector<unsigned char> Px; int W = 0, H = 0; };
    std::vector<Image> images;
    int largest = 16;
    for (const std::string& path : paths) {
        Image img;
        img.Path = path;
        const std::string file = sage::AssetDatabase::Instance().LocatePath(path);
        if (!ResourceManager::DecodeImageFile(file, img.Px, img.W, img.H) || img.W <= 0 || img.H <= 0) {
            LOG_WARN("Render") << "Текстура света не читается: " << path;
            continue;
        }
        largest = std::max(largest, std::max(img.W, img.H));
        images.push_back(std::move(img));
    }
    if (images.empty()) {
        m_texture.reset();
        m_cols = m_tile = 0;
        return 0;
    }
    const int limit = maxTextureSize > 0 ? maxTextureSize : 16384;
    m_cols = (int)std::ceil(std::sqrt((double)images.size()));
    // Плитка — по самой крупной картинке (до 1024), но атлас обязан влезть.
    m_tile = std::min({largest, 1024, limit / m_cols});
    const int side = m_cols * m_tile;
    std::vector<unsigned char> atlas((size_t)side * side * 4, 0);
    for (size_t i = 0; i < images.size(); ++i) {
        const Image& img = images[i];
        const int ox = (int)(i % (size_t)m_cols) * m_tile, oy = (int)(i / (size_t)m_cols) * m_tile;
        // Билинейная выборка источника в плитку.
        for (int y = 0; y < m_tile; ++y)
            for (int x = 0; x < m_tile; ++x) {
                const float fx = ((float)x + 0.5f) / (float)m_tile * (float)img.W - 0.5f;
                const float fy = ((float)y + 0.5f) / (float)m_tile * (float)img.H - 0.5f;
                const int x0 = std::clamp((int)std::floor(fx), 0, img.W - 1), y0 = std::clamp((int)std::floor(fy), 0, img.H - 1);
                const int x1 = std::min(x0 + 1, img.W - 1), y1 = std::min(y0 + 1, img.H - 1);
                const float tx = std::clamp(fx - (float)x0, 0.0f, 1.0f), ty = std::clamp(fy - (float)y0, 0.0f, 1.0f);
                for (int c = 0; c < 4; ++c) {
                    auto at = [&](int xx, int yy) { return (float)img.Px[((size_t)yy * img.W + xx) * 4 + c]; };
                    const float v = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) +
                                    (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
                    atlas[((size_t)(oy + y) * side + (ox + x)) * 4 + c] = (unsigned char)std::lround(v);
                }
            }
        m_slots[img.Path] = (int)i;
    }
    sage::rhi::Texture2DDesc desc;
    desc.Width = desc.Height = side;
    desc.Channels = 4;
    // Без мипов: у атласа они смешали бы соседние плитки на краю.
    desc.GenerateMipmaps = false;
    desc.FilterMode = sage::rhi::Filter::Bilinear;
    desc.WrapMode = sage::rhi::Wrap::ClampEdge;
    // Маска — цвет картинки: витраж окрашивает свет так, как он нарисован.
    desc.Srgb = true;
    m_texture = sage::rhi::GraphicsDevice::Get().CreateTexture2D(desc, atlas.data());
    return (int)m_slots.size();
}

void LightCookies::Upload(Shader& shader) const {
    shader.SetInt("uCookieAtlas", kLightCookieUnit);
    shader.SetInt("uCookieCols", m_texture ? m_cols : 0);
    shader.SetFloat("uCookieTile", (float)std::max(m_tile, 1));
    if (m_texture) m_texture->Bind(kLightCookieUnit);
}

} // namespace sage::render
