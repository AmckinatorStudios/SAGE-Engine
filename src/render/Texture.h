#pragma once
#include <glad/glad.h>
#include <string>
#include <vector>

// Декодированное изображение в ОЗУ (RGBA/RGB/RED, 8 бит на канал) — без GL.
// Именно это готовят на фоновом потоке при асинхронной загрузке текстуры
// (декод PNG/JPG через stb_image), а затем в главном потоке из него делают
// GPU-текстуру. LoadImageFile ниже потокобезопасен (stb собран с
// STBI_THREAD_LOCAL — см. Texture.cpp).
struct ImageData {
    std::vector<unsigned char> Pixels;
    int Width = 0;
    int Height = 0;
    int Channels = 0;
    bool Ok() const { return !Pixels.empty() && Width > 0 && Height > 0; }
};

// Читает и декодирует файл изображения в ImageData. НЕ трогает GL — безопасно
// вызывать на фоновом потоке (JobSystem). При ошибке возвращает пустой
// ImageData (Ok() == false); reason, если нужен, кладётся в *error.
ImageData LoadImageFile(const std::string& path, std::string* error = nullptr);

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

    // Создаёт GPU-текстуру из уже декодированного изображения (см. ImageData /
    // LoadImageFile). Это GL-часть асинхронной загрузки: пиксели готовит фон,
    // а этот конструктор вызывается в главном потоке. Бросает при пустом data.
    Texture(const ImageData& data, TextureFilter filter = TextureFilter::Trilinear,
            bool generateMipmaps = true);

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
    void UploadFromImage(const ImageData& data, TextureFilter filter, bool generateMipmaps);

    unsigned int m_id = 0;
    int m_width = 0, m_height = 0, m_channels = 0;
};
