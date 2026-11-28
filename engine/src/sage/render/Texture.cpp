#include "sage/core/ImageDecodeBudget.h"
#define STBI_MALLOC(sz) sage::image_memory::Allocate(sz)
#define STBI_REALLOC(p, sz) sage::image_memory::Reallocate(p, sz)
#define STBI_FREE(p) sage::image_memory::Free(p)
#define STB_IMAGE_IMPLEMENTATION
#include "sage/assets/Pack.h"
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

namespace {
// Какую фильтрацию МОЖНО получить на этой машине.
//
// Анизотропия — расширение: на карте без него запрос молча превращается в
// трилинейную, и лучше решить это ОДИН раз здесь, чем в каждом бэкенде и в
// каждом месте, которое потом спросит «а какая фильтрация вышла». Заодно
// пропадает ложь в ответе Filter(): он говорит про то, что есть, а не про то,
// что просили.
//
// Анизотропия без мипмапов бессмысленна (ей нечего выбирать), поэтому такой
// запрос тоже опускается.
TextureFilter EffectiveFilter(TextureFilter want, bool mipmaps) {
    if (want != TextureFilter::Anisotropic) return want;
    if (!mipmaps) return TextureFilter::Trilinear;
    return Texture::MaxSupportedAnisotropy() > 1.0f ? want : TextureFilter::Trilinear;
}
} // namespace

Texture::Texture(const std::string& path, TextureFilter filter, bool generateMipmaps, bool srgb)
    : m_srgb(srgb) {
    // Свой формат — своя ветка, и она ПЕРВАЯ: .sagetex это не картинка, stb его
    // не откроет, а сообщение «unknown image type» ничего бы не объяснило.
    if (path.size() > 8 && path.compare(path.size() - 8, 8, ".sagetex") == 0) {
        if (LoadFromNative(path, filter)) return;
        throw std::runtime_error("Не удалось загрузить .sagetex: " + path);
    }

    stbi_set_flip_vertically_on_load_thread(true); // GPU ждёт (0,0) внизу-слева, у большинства картинок — вверху-слева

    // Через vfs: stbi_load(путь) открывает файл сам и о пакете не знает — в
    // собранной игре не грузилась ни одна текстура.
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> data(
        sage::assets::vfs::LoadImage(path, &m_width, &m_height, &m_channels, 0),
        stbi_image_free);
    if (!data) {
        // Причины может не быть: файла нет — stb его и не открывал. nullptr в
        // поток и в строку — падение процесса вместо понятной ошибки.
        const char* why = stbi_failure_reason();
        const std::string reason = why ? why : "файл не найден или не читается";
        LOG_ERROR("Texture") << "Не удалось загрузить текстуру: " << path << " (" << reason << ")";
        throw std::runtime_error("Не удалось загрузить текстуру: " + path + " (" + reason + ")");
    }

    const unsigned char* pixels = data.get();
    std::vector<unsigned char> rgba;
    if (m_channels == 2) {
        // STB допускает grayscale + alpha, а RHI принимает 1, 3 или 4 канала.
        const size_t count = static_cast<size_t>(m_width) * m_height;
        rgba.resize(count * 4);
        for (size_t i = 0; i < count; ++i) {
            std::fill_n(&rgba[i * 4], 3, pixels[i * 2]);
            rgba[i * 4 + 3] = pixels[i * 2 + 1];
        }
        m_channels = 4;
        pixels = rgba.data();
    }

    Texture2DDesc desc;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.Channels = m_channels;
    desc.Srgb = m_srgb;
    filter = EffectiveFilter(filter, generateMipmaps);
    desc.FilterMode = filter;
    m_filter = filter;
    desc.GenerateMipmaps = generateMipmaps;
    m_hasMipmaps = generateMipmaps;
    m_texture = GraphicsDevice::Get().CreateTexture2D(desc, pixels);
    if (!m_texture) throw std::runtime_error("GPU texture upload failed: " + path);

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
    desc.Srgb = m_srgb;
    desc.GenerateMipmaps = tex.Levels.size() > 1;
    m_hasMipmaps = desc.GenerateMipmaps;
    filter = EffectiveFilter(filter, desc.GenerateMipmaps);
    desc.FilterMode = filter;
    m_filter = filter;
    m_texture = GraphicsDevice::Get().CreateTexture2D(desc, rgba.data());
    if (!m_texture) return false;
    LOG_INFO("Texture") << "Текстура загружена: " << path << " (" << w << "x" << h
                        << ", свой формат, уровней " << tex.Levels.size() << ")";
    return true;
}

namespace {
// Невладеющая обёртка над чужим GL-именем: Bind работает, деструктор ничего не
// удаляет. Хранилищем распоряжается рендер-таргет.
class BorrowedTexture2D : public sage::rhi::Texture2D {
public:
    BorrowedTexture2D(sage::rhi::TextureHandle h) : m_handle(h) {}
    void Bind(int unit) const override { sage::rhi::GraphicsDevice::Get().BindTexture2D(unit, m_handle); }
    sage::rhi::TextureHandle Handle() const override { return m_handle; }
    uint64_t NativeHandle() const override { return m_handle.Value; }
private:
    sage::rhi::TextureHandle m_handle;
};
} // namespace

std::shared_ptr<Texture> Texture::Wrap(sage::rhi::TextureHandle handle, int width, int height, bool linearColor) {
    std::shared_ptr<Texture> t(new Texture());
    t->m_texture = std::make_unique<BorrowedTexture2D>(handle);
    t->m_width = width;
    t->m_height = height;
    t->m_channels = 4;
    t->m_hasMipmaps = false;
    t->m_srgb = linearColor;
    return t;
}

Texture::Texture(const unsigned char* pixelsRGBA, int width, int height, TextureFilter filter,
                 bool generateMipmaps, bool srgb)
    : m_srgb(srgb) {
    m_width = width;
    m_height = height;
    m_channels = 4;

    Texture2DDesc desc;
    desc.Width = width;
    desc.Height = height;
    desc.Channels = 4;
    desc.Srgb = m_srgb;
    filter = EffectiveFilter(filter, generateMipmaps);
    desc.FilterMode = filter;
    m_filter = filter;
    desc.GenerateMipmaps = generateMipmaps;
    m_hasMipmaps = generateMipmaps;
    m_texture = GraphicsDevice::Get().CreateTexture2D(desc, pixelsRGBA);
    if (!m_texture) throw std::runtime_error("GPU texture upload failed");
}

std::shared_ptr<Texture> Texture::CreateFloat32Data(const float* rgba, int width, int height) {
    auto texture = std::shared_ptr<Texture>(new Texture());
    texture->m_width = width;
    texture->m_height = height;
    texture->m_channels = 4;
    texture->m_hasMipmaps = false;
    texture->m_float32 = true;
    texture->m_filter = TextureFilter::Nearest;
    Texture2DDesc desc;
    desc.Width = width;
    desc.Height = height;
    desc.Float32 = true;
    desc.GenerateMipmaps = false;
    desc.FilterMode = TextureFilter::Nearest;
    texture->m_texture = GraphicsDevice::Get().CreateTexture2D(desc, rgba);
    if (!texture->m_texture) throw std::runtime_error("GPU data texture upload failed");
    return texture;
}

void Texture::ReplacePixels(const unsigned char* pixelsRGBA, int width, int height,
                            TextureFilter filter, bool generateMipmaps) {

    Texture2DDesc desc;
    desc.Width = width;
    desc.Height = height;
    desc.Channels = 4;
    desc.Srgb = m_srgb;   // цветовое пространство — свойство картинки, не заливки
    filter = EffectiveFilter(filter, generateMipmaps);
    desc.FilterMode = filter;
    desc.GenerateMipmaps = generateMipmaps;
    // Пересоздание: старый rhi::Texture2D освобождается (unique_ptr), новый
    // занимает его место — GL-хендл заменяется на главном потоке.
    auto replacement = GraphicsDevice::Get().CreateTexture2D(desc, pixelsRGBA);
    if (!replacement) throw std::runtime_error("Texture::ReplacePixels: GPU allocation/upload failed");
    m_texture = std::move(replacement);
    m_float32 = false;
    m_width = width;
    m_height = height;
    m_channels = 4;
    m_hasMipmaps = generateMipmaps;
    m_filter = filter;
}
