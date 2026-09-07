#include "Skybox.h"
#include <stb_image.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>
#include <stdexcept>
#include "sage/core/Log.h"
#include "sage/rhi/GraphicsDevice.h"

using namespace sage::rhi;

// Куб единичного размера для skybox — только позиции, без нормалей/UV
// (текстурные координаты для cubemap — это само направление вершины от центра)
static const float kSkyboxVertices[] = {
    -1.0f,  1.0f, -1.0f,   -1.0f, -1.0f, -1.0f,    1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,    1.0f,  1.0f, -1.0f,   -1.0f,  1.0f, -1.0f,

    -1.0f, -1.0f,  1.0f,   -1.0f, -1.0f, -1.0f,   -1.0f,  1.0f, -1.0f,
    -1.0f,  1.0f, -1.0f,   -1.0f,  1.0f,  1.0f,   -1.0f, -1.0f,  1.0f,

     1.0f, -1.0f, -1.0f,    1.0f, -1.0f,  1.0f,    1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,    1.0f,  1.0f, -1.0f,    1.0f, -1.0f, -1.0f,

    -1.0f, -1.0f,  1.0f,   -1.0f,  1.0f,  1.0f,    1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,    1.0f, -1.0f,  1.0f,   -1.0f, -1.0f,  1.0f,

    -1.0f,  1.0f, -1.0f,    1.0f,  1.0f, -1.0f,    1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,   -1.0f,  1.0f,  1.0f,   -1.0f,  1.0f, -1.0f,

    -1.0f, -1.0f, -1.0f,   -1.0f, -1.0f,  1.0f,    1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,   -1.0f, -1.0f,  1.0f,    1.0f, -1.0f,  1.0f
};

void Skybox::BuildGeometry() {
    GraphicsDevice& device = GraphicsDevice::Get();
    VertexLayout layout;
    layout.Stride = 3 * sizeof(float);
    layout.Attributes = {{0, 3, AttribType::Float, 0}};
    m_geometry = device.CreateGeometry(layout);
    m_geometry->SetVertexData(kSkyboxVertices, sizeof(kSkyboxVertices), /*dynamic=*/false);
}

Skybox::Skybox(const CubeFacePixels faces[6]) {
    BuildGeometry();
    m_cubemap = GraphicsDevice::Get().CreateTextureCube(faces);
    if (!m_cubemap) throw std::runtime_error("Не удалось создать кубическую текстуру неба");
}

Skybox::Skybox(const std::array<std::string, 6>& faces) {
    GraphicsDevice& device = GraphicsDevice::Get();
    BuildGeometry();

    stbi_set_flip_vertically_on_load(false); // у cubemap другая конвенция — грани НЕ переворачиваем

    // Сначала декодируем все 6 граней (падение на любой — без утечки уже
    // декодированных), затем одним вызовом создаём cubemap у бэкенда.
    unsigned char* pixels[6] = {};
    CubeFacePixels cubeFaces[6];
    try {
        for (int i = 0; i < 6; ++i) {
            int width, height, channels;
            pixels[i] = stbi_load(faces[i].c_str(), &width, &height, &channels, 0);
            if (!pixels[i]) {
                throw std::runtime_error("Не удалось загрузить грань skybox: " + faces[i] +
                                         " (" + stbi_failure_reason() + ")");
            }
            cubeFaces[i] = {width, height, channels, pixels[i]};
        }
        m_cubemap = device.CreateTextureCube(cubeFaces);
    } catch (...) {
        for (unsigned char* p : pixels) if (p) stbi_image_free(p);
        stbi_set_flip_vertically_on_load(true);
        throw;
    }
    for (unsigned char* p : pixels) stbi_image_free(p);

    stbi_set_flip_vertically_on_load(true); // возвращаем конвенцию обратно для обычных Texture

    LOG_INFO("Skybox") << "Skybox загружен (6 граней)";
}


// ---------------------------------------------------------------------------
//  Небо из ОДНОГО изображения
// ---------------------------------------------------------------------------

Skybox::Layout Skybox::DetectLayout(int width, int height) {
    if (width <= 0 || height <= 0) return Layout::Auto;
    const double aspect = (double)width / (double)height;
    // Допуск, а не точное равенство: у наборов встречается лишний пиксель по
    // краю, и «4:3 с точностью до пикселя» отбраковало бы годную картинку.
    auto near = [aspect](double want) { return std::abs(aspect - want) < 0.02 * want; };
    if (near(4.0 / 3.0)) return Layout::HorizontalCross;
    if (near(3.0 / 4.0)) return Layout::VerticalCross;
    if (near(6.0)) return Layout::Row;
    if (near(1.0 / 6.0)) return Layout::Column;
    if (near(2.0)) return Layout::Equirectangular;
    return Layout::Auto;   // не распознано
}

const char* Skybox::LayoutName(Layout layout) {
    switch (layout) {
        case Layout::HorizontalCross: return "крест 4:3";
        case Layout::VerticalCross:   return "крест 3:4";
        case Layout::Row:             return "полоса 6:1";
        case Layout::Column:          return "столбец 1:6";
        case Layout::Equirectangular: return "панорама 2:1";
        default:                      return "не распознана";
    }
}

namespace {

// Клетка креста/полосы для каждой грани, в порядке +X, -X, +Y, -Y, +Z, -Z.
// Числа — столбец и строка в сетке; -1 у поворота означает «как есть».
struct Cell { int Col, Row, Rotate180; };

const Cell* CellsFor(Skybox::Layout layout) {
    // Горизонтальный крест 4x3:
    //        [+Y]
    //   [-X] [+Z] [+X] [-Z]
    //        [-Y]
    static const Cell kHoriz[6] = {{2,1,0}, {0,1,0}, {1,0,0}, {1,2,0}, {1,1,0}, {3,1,0}};
    // Вертикальный крест 3x4 — тот же крест, но -Z уехал вниз и лежит вверх
    // ногами: так его и печатают, чтобы крест сворачивался в куб без разрывов.
    static const Cell kVert[6]  = {{2,1,0}, {0,1,0}, {1,0,0}, {1,2,0}, {1,1,0}, {1,3,1}};
    static const Cell kRow[6]   = {{0,0,0}, {1,0,0}, {2,0,0}, {3,0,0}, {4,0,0}, {5,0,0}};
    static const Cell kCol[6]   = {{0,0,0}, {0,1,0}, {0,2,0}, {0,3,0}, {0,4,0}, {0,5,0}};
    switch (layout) {
        case Skybox::Layout::HorizontalCross: return kHoriz;
        case Skybox::Layout::VerticalCross:   return kVert;
        case Skybox::Layout::Row:             return kRow;
        case Skybox::Layout::Column:          return kCol;
        default:                              return nullptr;
    }
}

// Сколько клеток по горизонтали и вертикали у раскладки.
void GridOf(Skybox::Layout layout, int& cols, int& rows) {
    switch (layout) {
        case Skybox::Layout::HorizontalCross: cols = 4; rows = 3; break;
        case Skybox::Layout::VerticalCross:   cols = 3; rows = 4; break;
        case Skybox::Layout::Row:             cols = 6; rows = 1; break;
        case Skybox::Layout::Column:          cols = 1; rows = 6; break;
        default:                              cols = 0; rows = 0; break;
    }
}

// Направление луча для точки (u,v) в пределах грани. u,v идут от 0 до 1,
// начало — левый верхний угол грани, как и в памяти картинки.
glm::vec3 FaceDirection(int face, float u, float v) {
    const float a = 2.0f * u - 1.0f;
    const float b = 1.0f - 2.0f * v;
    switch (face) {
        case 0: return glm::normalize(glm::vec3( 1.0f,  b, -a)); // +X
        case 1: return glm::normalize(glm::vec3(-1.0f,  b,  a)); // -X
        case 2: return glm::normalize(glm::vec3( a,  1.0f, -b)); // +Y
        case 3: return glm::normalize(glm::vec3( a, -1.0f,  b)); // -Y
        case 4: return glm::normalize(glm::vec3( a,  b,  1.0f)); // +Z
        default:return glm::normalize(glm::vec3(-a,  b, -1.0f)); // -Z
    }
}

} // namespace

std::unique_ptr<Skybox> Skybox::LoadFromImage(const std::string& file, Layout layout) {
    stbi_set_flip_vertically_on_load(false);   // у cubemap своя конвенция
    int w = 0, h = 0, comp = 0;
    unsigned char* src = stbi_load(file.c_str(), &w, &h, &comp, 0);
    stbi_set_flip_vertically_on_load(true);
    if (!src) {
        LOG_ERROR("Skybox") << "Небо не прочиталось: " << file << " ("
                            << (stbi_failure_reason() ? stbi_failure_reason() : "?") << ")";
        return nullptr;
    }

    const Layout chosen = layout == Layout::Auto ? DetectLayout(w, h) : layout;
    if (chosen == Layout::Auto) {
        LOG_ERROR("Skybox") << "Не понимаю раскладку неба " << file << " (" << w << "x" << h
                            << "): ожидались крест 4:3 или 3:4, полоса 6:1, столбец 1:6 "
                               "или панорама 2:1";
        stbi_image_free(src);
        return nullptr;
    }

    std::vector<std::vector<unsigned char>> faces(6);
    int faceSize = 0;

    if (chosen == Layout::Equirectangular) {
        // Панорама — не сетка, а проекция: у каждого пикселя грани спрашиваем
        // направление и берём из панорамы точку с теми же широтой и долготой.
        // Размер грани — четверть ширины: столько же деталей на 90°, сколько в
        // исходнике.
        faceSize = std::max(16, w / 4);
        for (int f = 0; f < 6; ++f) {
            faces[f].assign((size_t)faceSize * faceSize * comp, 0);
            for (int y = 0; y < faceSize; ++y) {
                for (int x = 0; x < faceSize; ++x) {
                    const glm::vec3 d = FaceDirection(f, (x + 0.5f) / faceSize,
                                                      (y + 0.5f) / faceSize);
                    const float lon = std::atan2(d.x, -d.z);          // -pi..pi
                    const float lat = std::asin(glm::clamp(d.y, -1.0f, 1.0f));
                    int sx = (int)((lon / (2.0f * 3.14159265f) + 0.5f) * (float)w);
                    int sy = (int)((0.5f - lat / 3.14159265f) * (float)h);
                    sx = glm::clamp(sx, 0, w - 1);
                    sy = glm::clamp(sy, 0, h - 1);
                    const unsigned char* s = src + ((size_t)sy * w + sx) * comp;
                    unsigned char* d8 = faces[f].data() + ((size_t)y * faceSize + x) * comp;
                    for (int c = 0; c < comp; ++c) d8[c] = s[c];
                }
            }
        }
    } else {
        int cols = 0, rows = 0;
        GridOf(chosen, cols, rows);
        const Cell* cells = CellsFor(chosen);
        if (!cells || cols == 0) { stbi_image_free(src); return nullptr; }
        faceSize = w / cols;
        if (faceSize <= 0 || h / rows != faceSize) {
            LOG_ERROR("Skybox") << "Клетки неба не квадратные: " << file << " (" << w << "x" << h
                                << ", раскладка " << LayoutName(chosen) << ")";
            stbi_image_free(src);
            return nullptr;
        }
        for (int f = 0; f < 6; ++f) {
            faces[f].assign((size_t)faceSize * faceSize * comp, 0);
            const int x0 = cells[f].Col * faceSize;
            const int y0 = cells[f].Row * faceSize;
            for (int y = 0; y < faceSize; ++y) {
                for (int x = 0; x < faceSize; ++x) {
                    // Поворот на 180° — это чтение той же клетки с конца по обеим
                    // осям; отдельного прохода он не требует.
                    const int sx = cells[f].Rotate180 ? (faceSize - 1 - x) : x;
                    const int sy = cells[f].Rotate180 ? (faceSize - 1 - y) : y;
                    const unsigned char* s = src + ((size_t)(y0 + sy) * w + (x0 + sx)) * comp;
                    unsigned char* d8 = faces[f].data() + ((size_t)y * faceSize + x) * comp;
                    for (int c = 0; c < comp; ++c) d8[c] = s[c];
                }
            }
        }
    }
    stbi_image_free(src);

    CubeFacePixels cubeFaces[6];
    for (int f = 0; f < 6; ++f) cubeFaces[f] = {faceSize, faceSize, comp, faces[f].data()};

    try {
        auto sky = std::unique_ptr<Skybox>(new Skybox(cubeFaces));
        LOG_INFO("Skybox") << "Небо из одного файла: " << file << " (" << w << "x" << h
                           << ", " << LayoutName(chosen) << ", грань " << faceSize << "px)";
        return sky;
    } catch (const std::exception& e) {
        LOG_ERROR("Skybox") << "Небо не собралось из " << file << ": " << e.what();
        return nullptr;
    }
}

const std::array<const char*, 6>& Skybox::FaceNames() {
    // Порядок обязан совпадать с порядком граней cubemap: +X, -X, +Y, -Y, +Z, -Z.
    static const std::array<const char*, 6> kNames = {"px", "nx", "py", "ny", "pz", "nz"};
    return kNames;
}

std::unique_ptr<Skybox> Skybox::LoadFromDirectory(const std::string& directory) {
    namespace fs = std::filesystem;
    static const char* kExtensions[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp"};

    std::error_code ec;
    if (directory.empty() || !fs::is_directory(directory, ec)) {
        LOG_ERROR("Skybox") << "Каталог неба не найден: " << directory;
        return nullptr;
    }

    std::array<std::string, 6> faces;
    for (int i = 0; i < 6; ++i) {
        for (const char* ext : kExtensions) {
            fs::path candidate = fs::path(directory) / (std::string(FaceNames()[i]) + ext);
            if (fs::exists(candidate, ec)) { faces[i] = candidate.string(); break; }
        }
        if (faces[i].empty()) {
            LOG_ERROR("Skybox") << "В каталоге " << directory << " нет грани '"
                                << FaceNames()[i] << "' (ожидались png/jpg/jpeg/tga/bmp)";
            return nullptr;
        }
    }

    try {
        return std::make_unique<Skybox>(faces);
    } catch (const std::exception& e) {
        LOG_ERROR("Skybox") << "Не удалось собрать небо из " << directory << ": " << e.what();
        return nullptr;
    }
}

// --- Встроенный шейдер ------------------------------------------------------
// Свой, а не файл в assets: небо должно работать у любого, кто просто слинковал
// движок, — ровно как SkyRenderer. Поворот вокруг вертикали делается прямо в
// вершинном шейдере, чтобы не пересобирать матрицу вида на стороне вызывающего.
namespace {

const char* kSkyVert = R"(#version 330 core
layout (location = 0) in vec3 aPos;
out vec3 vDir;
uniform mat4 uView;        // без сдвига: небо бесконечно далеко
uniform mat4 uProjection;
uniform float uRotation;   // радианы, поворот вокруг оси Y
void main() {
    float c = cos(uRotation), s = sin(uRotation);
    vDir = vec3(c * aPos.x + s * aPos.z, aPos.y, -s * aPos.x + c * aPos.z);
    vec4 pos = uProjection * uView * vec4(aPos, 1.0);
    // z = w -> глубина ровно 1.0 после деления: с DepthFunc::LessEqual небо
    // никогда не перекроет геометрию, но заполнит весь незанятый фон.
    gl_Position = pos.xyww;
}
)";

const char* kSkyFrag = R"(#version 330 core
in vec3 vDir;
out vec4 FragColor;
uniform samplerCube uSkybox;
uniform vec3 uTint;
uniform float uIntensity;
void main() {
    FragColor = vec4(texture(uSkybox, normalize(vDir)).rgb * uTint * uIntensity, 1.0);
}
)";

Shader& BuiltinSkyShader() {
    static Shader* s = new Shader(Shader::FromSource(kSkyVert, kSkyFrag, "Skybox.Builtin"));
    return *s;
}

} // namespace

void Skybox::DrawInternal(const glm::mat4& view, const glm::mat4& projection) const {
    GraphicsDevice& device = GraphicsDevice::Get();

    // Рисуем skybox ПЕРЕД остальной сценой, но так, чтобы он всегда оставался
    // позади всего: LessEqual + глубина 1.0 из вершинного шейдера гарантируют,
    // что skybox никогда не перекроет реальную геометрию.
    device.SetDepthFunc(DepthFunc::LessEqual);
    device.SetDepthWrite(false);
    // Skybox рисуется "изнутри" куба — при обычном backface culling все грани
    // оказались бы отвёрнуты от камеры, поэтому на время отрисовки выключаем.
    device.SetCullMode(CullMode::Off);

    m_cubemap->Bind(0);
    m_geometry->DrawArrays(36);

    device.SetCullMode(CullMode::Back);
    device.SetDepthWrite(true);
    device.SetDepthFunc(DepthFunc::Less);
    (void)view; (void)projection; // матрицы уже выставлены вызывающим
}

void Skybox::Draw(Shader& shader, const glm::mat4& view, const glm::mat4& projection,
                  const glm::vec3& tint) const {
    shader.Use();
    // Убираем сдвиг (позицию камеры) из view-матрицы — skybox должен всегда
    // казаться бесконечно далёким, реагируя только на поворот камеры.
    shader.SetMat4("uView", glm::mat4(glm::mat3(view)));
    shader.SetMat4("uProjection", projection);
    shader.SetInt("uSkybox", 0);
    shader.SetVec3("uTint", tint);
    DrawInternal(view, projection);
}

void Skybox::Draw(const glm::mat4& view, const glm::mat4& projection,
                  float intensity, float rotationDeg) const {
    Shader& shader = BuiltinSkyShader();
    shader.Use();
    shader.SetMat4("uView", glm::mat4(glm::mat3(view)));
    shader.SetMat4("uProjection", projection);
    shader.SetInt("uSkybox", 0);
    shader.SetVec3("uTint", glm::vec3(1.0f));
    shader.SetFloat("uIntensity", intensity);
    shader.SetFloat("uRotation", glm::radians(rotationDeg));
    DrawInternal(view, projection);
}
