#include "Skybox.h"
#include <stb_image.h>
#include <filesystem>
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

Skybox::Skybox(const std::array<std::string, 6>& faces) {
    GraphicsDevice& device = GraphicsDevice::Get();

    VertexLayout layout;
    layout.Stride = 3 * sizeof(float);
    layout.Attributes = {{0, 3, AttribType::Float, 0}};
    m_geometry = device.CreateGeometry(layout);
    m_geometry->SetVertexData(kSkyboxVertices, sizeof(kSkyboxVertices), /*dynamic=*/false);

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
