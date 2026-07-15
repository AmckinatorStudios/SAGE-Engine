#define STB_IMAGE_IMPLEMENTATION
// Делает внутренние флаги stb_image (флип по вертикали, причина ошибки)
// потоко-локальными — чтобы одновременный декод в нескольких воркерах
// JobSystem не гонялся за общий глобал (см. LoadImageFile / асинхронную
// загрузку текстур в ResourceManager).
#define STBI_THREAD_LOCAL
#include <stb_image.h>
#include "Texture.h"
#include "../core/Log.h"
#include <stdexcept>
#include <algorithm>

// Токены расширения GL_EXT_texture_filter_anisotropic. Это старое (но почти
// повсеместно поддерживаемое) расширение стало core-функциональностью только
// в OpenGL 4.6, а наш glad сгенерирован под 3.3 core без этого расширения —
// поэтому определяем токены сами. Функций загружать не нужно: это просто
// два enum-значения для обычных glTexParameterf/glGetFloatv.
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

float Texture::MaxSupportedAnisotropy() {
    static float cached = -1.0f;
    if (cached < 0.0f) {
        // Если расширение не поддерживается, glGetFloatv молча ничего не
        // запишет (останется 0) или вернёт GL_INVALID_ENUM — в обоих случаях
        // безопасно откатываемся к 1.0 (анизотропия выключена).
        GLfloat value = 0.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &value);
        cached = (value >= 1.0f) ? value : 1.0f;
    }
    return cached;
}

void Texture::ApplyFilter(TextureFilter filter, bool generateMipmaps) {
    GLenum magFilter = (filter == TextureFilter::Nearest) ? GL_NEAREST : GL_LINEAR;

    GLenum minFilter;
    if (!generateMipmaps) {
        minFilter = magFilter; // без мипмапов MIPMAP-варианты фильтра недоступны
    } else {
        switch (filter) {
            case TextureFilter::Nearest:  minFilter = GL_NEAREST_MIPMAP_NEAREST; break;
            case TextureFilter::Bilinear: minFilter = GL_LINEAR_MIPMAP_NEAREST; break;
            case TextureFilter::Trilinear:
            case TextureFilter::Anisotropic:
            default: minFilter = GL_LINEAR_MIPMAP_LINEAR; break;
        }
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);

    if (filter == TextureFilter::Anisotropic && generateMipmaps) {
        float level = std::min(4.0f, MaxSupportedAnisotropy()); // 4x — разумный баланс качество/цена
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, level);
    }
}

ImageData LoadImageFile(const std::string& path, std::string* error) {
    stbi_set_flip_vertically_on_load_thread(1); // (0,0) внизу-слева для OpenGL; потоко-локально

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 0);
    if (!pixels) {
        const char* reason = stbi_failure_reason();
        if (error) *error = std::string("не удалось загрузить '") + path + "': " + (reason ? reason : "?");
        return {};
    }

    ImageData data;
    data.Width = w;
    data.Height = h;
    data.Channels = channels;
    data.Pixels.assign(pixels, pixels + (size_t)w * h * channels);
    stbi_image_free(pixels);
    return data;
}

Texture::Texture(const std::string& path, TextureFilter filter, bool generateMipmaps) {
    std::string error;
    ImageData data = LoadImageFile(path, &error);
    if (!data.Ok()) {
        LOG_ERROR("Texture") << "Не удалось загрузить текстуру: " << error;
        throw std::runtime_error("Не удалось загрузить текстуру: " + error);
    }
    UploadFromImage(data, filter, generateMipmaps);
    LOG_INFO("Texture") << "Текстура загружена: " << path << " (" << m_width << "x" << m_height << ")";
}

Texture::Texture(const ImageData& data, TextureFilter filter, bool generateMipmaps) {
    if (!data.Ok()) throw std::runtime_error("Texture: пустой ImageData");
    UploadFromImage(data, filter, generateMipmaps);
}

void Texture::UploadFromImage(const ImageData& data, TextureFilter filter, bool generateMipmaps) {
    m_width = data.Width;
    m_height = data.Height;
    m_channels = data.Channels;

    GLenum format = GL_RGB;
    if (m_channels == 1) format = GL_RED;
    else if (m_channels == 3) format = GL_RGB;
    else if (m_channels == 4) format = GL_RGBA;

    glGenTextures(1, &m_id);
    glBindTexture(GL_TEXTURE_2D, m_id);

    glTexImage2D(GL_TEXTURE_2D, 0, format, m_width, m_height, 0, format, GL_UNSIGNED_BYTE, data.Pixels.data());
    if (generateMipmaps) glGenerateMipmap(GL_TEXTURE_2D);

    ApplyFilter(filter, generateMipmaps);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

Texture::Texture(const unsigned char* pixelsRGBA, int width, int height, TextureFilter filter, bool generateMipmaps) {
    m_width = width;
    m_height = height;
    m_channels = 4;

    glGenTextures(1, &m_id);
    glBindTexture(GL_TEXTURE_2D, m_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixelsRGBA);
    if (generateMipmaps) glGenerateMipmap(GL_TEXTURE_2D);

    ApplyFilter(filter, generateMipmaps);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

Texture::~Texture() {
    if (m_id) glDeleteTextures(1, &m_id);
}

void Texture::Bind(unsigned int unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, m_id);
}
