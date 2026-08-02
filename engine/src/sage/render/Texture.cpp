#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include "Texture.h"
#include "sage/core/Log.h"
#include "sage/rhi/GraphicsDevice.h"
#include <algorithm>
#include <stdexcept>
#include <vector>

#include "sage/assets/format/TextureFormat.h"

using namespace sage::rhi;

float Texture::MaxSupportedAnisotropy() {
    return GraphicsDevice::Get().MaxAnisotropy();
}

Texture::Texture(const std::string& path, TextureFilter filter, bool generateMipmaps) {
    // Свой формат — своя ветка, и она ПЕРВАЯ: .sagetex это не картинка, stb его
    // не откроет, а сообщение «unknown image type» ничего бы не объяснило.
    if (path.size() > 8 && path.compare(path.size() - 8, 8, ".sagetex") == 0) {
        if (LoadFromNative(path, filter)) return;
        throw std::runtime_error("Не удалось загрузить .sagetex: " + path);
    }

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

// Загрузка своего формата. Блочно сжатые уровни ПОКА разжимаются на процессоре
// и заливаются как RGBA: экономия видеопамяти появится вместе с поддержкой
// сжатых форматов в RHI, а выигрыш в размере файла и в отсутствии декодирования
// PNG есть уже сейчас. Это честный промежуточный шаг, а не окончательный.
bool Texture::LoadFromNative(const std::string& path, TextureFilter filter) {
    sage::assets::TextureData tex;
    std::string err;
    if (!sage::assets::ReadTextureFile(path, tex, err)) {
        LOG_ERROR("Texture") << "Не удалось загрузить " << path << ": " << err;
        return false;
    }
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!sage::assets::DecodeToRGBA(tex, 0, rgba, w, h)) {
        LOG_ERROR("Texture") << "Не удалось разжать уровень 0: " << path;
        return false;
    }
    // .sagetex хранит строки сверху вниз (как исходная картинка), а GPU ждёт
    // (0,0) снизу — переворачиваем здесь, в одном месте, как и для stb.
    const size_t rowBytes = (size_t)w * 4;
    for (int y = 0; y < h / 2; ++y) {
        uint8_t* a = rgba.data() + (size_t)y * rowBytes;
        uint8_t* b = rgba.data() + (size_t)(h - 1 - y) * rowBytes;
        std::swap_ranges(a, a + rowBytes, b);
    }

    m_width = w;
    m_height = h;
    m_channels = 4;
    Texture2DDesc desc;
    desc.Width = w;
    desc.Height = h;
    desc.Channels = 4;
    desc.FilterMode = filter;
    desc.GenerateMipmaps = tex.Levels.size() > 1;
    m_hasMipmaps = desc.GenerateMipmaps;
    m_texture = GraphicsDevice::Get().CreateTexture2D(desc, rgba.data());
    LOG_INFO("Texture") << "Текстура загружена: " << path << " (" << w << "x" << h
                        << ", свой формат, уровней " << tex.Levels.size() << ")";
    return true;
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
