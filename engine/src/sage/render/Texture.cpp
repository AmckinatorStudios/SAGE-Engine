#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include "Texture.h"
#include "sage/core/Log.h"
#include "sage/rhi/GraphicsDevice.h"
#include <stdexcept>

using namespace sage::rhi;

float Texture::MaxSupportedAnisotropy() {
    return GraphicsDevice::Get().MaxAnisotropy();
}

Texture::Texture(const std::string& path, TextureFilter filter, bool generateMipmaps) {
    stbi_set_flip_vertically_on_load(true); // GPU ждёт (0,0) внизу-слева, у большинства картинок — вверху-слева

    unsigned char* data = stbi_load(path.c_str(), &m_width, &m_height, &m_channels, 0);
    if (!data) {
        LOG_ERROR("Texture") << "Не удалось загрузить текстуру: " << path << " (" << stbi_failure_reason() << ")";
        throw std::runtime_error("Не удалось загрузить текстуру: " + path + " (" + stbi_failure_reason() + ")");
    }

    Texture2DDesc desc;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.Channels = m_channels;
    desc.FilterMode = filter;
    desc.GenerateMipmaps = generateMipmaps;
    m_hasMipmaps = generateMipmaps;
    m_texture = GraphicsDevice::Get().CreateTexture2D(desc, data);

    stbi_image_free(data);

    LOG_INFO("Texture") << "Текстура загружена: " << path << " (" << m_width << "x" << m_height << ")";
}

Texture::Texture(const unsigned char* pixelsRGBA, int width, int height, TextureFilter filter, bool generateMipmaps) {
    m_width = width;
    m_height = height;
    m_channels = 4;

    Texture2DDesc desc;
    desc.Width = width;
    desc.Height = height;
    desc.Channels = 4;
    desc.FilterMode = filter;
    desc.GenerateMipmaps = generateMipmaps;
    m_hasMipmaps = generateMipmaps;
    m_texture = GraphicsDevice::Get().CreateTexture2D(desc, pixelsRGBA);
}

void Texture::ReplacePixels(const unsigned char* pixelsRGBA, int width, int height,
                            TextureFilter filter, bool generateMipmaps) {
    m_width = width;
    m_height = height;
    m_channels = 4;
    m_hasMipmaps = generateMipmaps;

    Texture2DDesc desc;
    desc.Width = width;
    desc.Height = height;
    desc.Channels = 4;
    desc.FilterMode = filter;
    desc.GenerateMipmaps = generateMipmaps;
    // Пересоздание: старый rhi::Texture2D освобождается (unique_ptr), новый
    // занимает его место — GL-хендл заменяется на главном потоке.
    m_texture = GraphicsDevice::Get().CreateTexture2D(desc, pixelsRGBA);
}
