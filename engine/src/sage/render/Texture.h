#pragma once
#include <memory>
#include <string>
#include "sage/rhi/Resources.h"

// Режим фильтрации текстуры:
//  Nearest     — пиксельная чёткость без сглаживания (voxel-атласы, pixel-art)
//  Bilinear    — простое сглаживание, без интерполяции между уровнями мипмапов
//  Trilinear   — сглаживание + плавный переход между уровнями мипмапов
//                (стандартный выбор по умолчанию для большинства текстур)
//  Anisotropic — как Trilinear, плюс анизотропная фильтрация: убирает
//                размытость текстуры на поверхностях, видных под острым
//                углом (типичный случай — земля/пол, если смотреть почти
//                вдоль неё). Требует мипмапов, автоматически включает их.
// Это те же режимы, что у rhi::Filter — тонкая обёртка сохраняет прежнее имя
// для кода игр.
using TextureFilter = sage::rhi::Filter;

// Загружает изображение (PNG/JPG/BMP/TGA — через stb_image) и создаёт из него
// GPU-текстуру через текущий графический бэкенд. Часть ЯДРА движка — не
// зависит от вокселей, подходит для текстурирования любых мешей.
class Texture {
public:
    // generateMipmaps=false ОБЯЗАТЕЛЕН для текстурных атласов (несколько
    // текстур в одном файле, разные UV-прямоугольники) — иначе на дальних
    // объектах мипмапы "усредняют" цвет с соседними тайлами атласа и получается
    // едва заметное протекание чужого цвета по краям (используй Nearest +
    // generateMipmaps=false для атласов). Для обычных отдельных текстур
    // (не атласов) мипмапы включать можно и нужно — Trilinear или Anisotropic.
    explicit Texture(const std::string& path, TextureFilter filter = TextureFilter::Trilinear,
                      bool generateMipmaps = true);

    // Создаёт текстуру из уже декодированных пикселей в памяти (RGBA8) —
    // нужно для встроенных (embedded) текстур GLTF/GLB, у которых нет
    // отдельного файла на диске.
    Texture(const unsigned char* pixelsRGBA, int width, int height,
            TextureFilter filter = TextureFilter::Trilinear, bool generateMipmaps = true);

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&&) noexcept = default;
    Texture& operator=(Texture&&) noexcept = default;

    void Bind(unsigned int unit = 0) const { m_texture->Bind((int)unit); }

    int Width() const { return m_width; }
    int Height() const { return m_height; }

    // Пересоздаёт GPU-хранилище из уже декодированных RGBA8-пикселей. Ключ
    // асинхронной загрузки: воркер декодирует файл в CPU-буфер, а главный поток
    // (единственный с GL-контекстом) вызывает это, ЗАМЕНЯЯ картинку прямо в
    // существующем объекте Texture — все держатели shared_ptr<Texture>
    // (материалы, UI) видят подгруженное изображение без повторного запроса.
    void ReplacePixels(const unsigned char* pixelsRGBA, int width, int height,
                       TextureFilter filter = TextureFilter::Trilinear, bool generateMipmaps = true);

    // Приблизительный размер в VRAM (RGBA8 + ~33% на цепочку мип-уровней) —
    // для бюджета памяти и LRU-вытеснения в ResourceManager.
    static size_t EstimateBytes(int width, int height, bool mipmaps) {
        size_t base = (size_t)width * (size_t)height * 4u;
        return mipmaps ? base + base / 3u : base;
    }
    size_t GpuBytes() const { return EstimateBytes(m_width, m_height, m_hasMipmaps); }

    // Максимальный уровень анизотропии, который поддерживает текущая
    // видеокарта/драйвер (обычно 4, 8 или 16).
    static float MaxSupportedAnisotropy();

private:
    std::unique_ptr<sage::rhi::Texture2D> m_texture;
    int m_width = 0, m_height = 0, m_channels = 0;
    bool m_hasMipmaps = true;
};
