#pragma once
#include <glad/glad.h>
#include <string>

// Режим фильтрации текстуры:
//  Nearest     — пиксельная чёткость без сглаживания (voxel-атласы, pixel-art)
//  Bilinear    — простое сглаживание, без интерполяции между уровнями мипмапов
//  Trilinear   — сглаживание + плавный переход между уровнями мипмапов
//                (стандартный выбор по умолчанию для большинства текстур)
//  Anisotropic — как Trilinear, плюс анизотропная фильтрация: убирает
//                размытость текстуры на поверхностях, видных под острым
//                углом (типичный случай — земля/пол, если смотреть почти
//                вдоль неё). Требует мипмапов, автоматически включает их.
enum class TextureFilter { Nearest, Bilinear, Trilinear, Anisotropic };

// Загружает изображение (PNG/JPG/BMP/TGA — через stb_image) и создаёт
// из него GPU-текстуру. Часть ЯДРА движка — не зависит от вокселей,
// подходит для текстурирования любых мешей (моделей, кубов, чего угодно).
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

    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    void Bind(unsigned int unit = 0) const;

    int Width() const { return m_width; }
    int Height() const { return m_height; }

    // Максимальный уровень анизотропии, который поддерживает текущая
    // видеокарта/драйвер (обычно 4, 8 или 16). Определяется один раз при
    // первом обращении и кэшируется.
    static float MaxSupportedAnisotropy();

private:
    void ApplyFilter(TextureFilter filter, bool generateMipmaps);

    unsigned int m_id = 0;
    int m_width = 0, m_height = 0, m_channels = 0;
};
