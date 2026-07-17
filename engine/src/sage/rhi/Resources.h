#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// RHI-ресурсы — бэкенд-независимые абстракции GPU-объектов. Весь код движка и
// игр создаёт их через фабрики GraphicsDevice (CreateShaderProgram/
// CreateGeometry/CreateTexture2D/CreateTextureCube/CreateRenderTarget) и
// никогда не трогает графический API напрямую. Реализации живут в
// engine/src/rhi/<backend>/ — чтобы добавить новый бэкенд, реализуют эти
// интерфейсы, не меняя потребителей.
// ---------------------------------------------------------------------------
namespace sage::rhi {

// --- Общие перечисления состояния конвейера ---
enum class DepthFunc { Less, LessEqual };
enum class CullMode { Back, Front, Off };
enum class PolygonMode { Fill, Line }; // Line — каркасный (wireframe) режим отрисовки

// --- Описание вершинного формата ---
enum class AttribType {
    Float,     // 32-битные float-компоненты
    UByteNorm, // байты 0..255, нормализуемые в 0..1 (цвета)
};

struct VertexAttribute {
    int Location = 0;   // layout(location = N) в шейдере
    int Components = 0; // 1..4
    AttribType Type = AttribType::Float;
    int Offset = 0;     // байтовое смещение от начала вершины
};

// Формат геометрии: основной вершинный поток + (опционально) per-instance
// поток (атрибуты, продвигаемые раз на инстанс — как у частиц).
struct VertexLayout {
    int Stride = 0;
    std::vector<VertexAttribute> Attributes;
    int InstanceStride = 0;
    std::vector<VertexAttribute> InstanceAttributes;
};

// --- Шейдерная программа (vertex + fragment) ---
class ShaderProgram {
public:
    virtual ~ShaderProgram() = default;
    virtual void Use() const = 0;
    virtual void SetMat4(const std::string& name, const glm::mat4& v) const = 0;
    virtual void SetVec4(const std::string& name, const glm::vec4& v) const = 0;
    virtual void SetVec3(const std::string& name, const glm::vec3& v) const = 0;
    virtual void SetVec2(const std::string& name, const glm::vec2& v) const = 0;
    virtual void SetFloat(const std::string& name, float v) const = 0;
    virtual void SetInt(const std::string& name, int v) const = 0;
};

// --- Геометрия: вершинный/индексный/инстансный буферы + отрисовка ---
class Geometry {
public:
    virtual ~Geometry() = default;

    // dynamic=true — данные будут перезаливаться часто (UI/текст каждый кадр).
    virtual void SetVertexData(const void* data, size_t bytes, bool dynamic) = 0;
    virtual void SetIndexData(const unsigned int* indices, size_t count, bool dynamic) = 0;
    // Per-instance данные (поток InstanceAttributes) — перезаливаются каждый кадр.
    virtual void SetInstanceData(const void* data, size_t bytes) = 0;

    virtual void DrawIndexed(size_t indexCount) const = 0;
    virtual void DrawArrays(size_t vertexCount) const = 0;
    virtual void DrawInstanced(size_t vertexCount, size_t instanceCount) const = 0;
    // Отрезки (каждая пара вершин — линия). Используется DebugDraw (сетка,
    // wire-примитивы, оси) — линии не выражаются треугольным DrawArrays.
    virtual void DrawLines(size_t vertexCount) const = 0;
};

// --- Текстуры ---
enum class Filter { Nearest, Bilinear, Trilinear, Anisotropic };
enum class Wrap { Repeat, ClampEdge };

struct Texture2DDesc {
    int Width = 0;
    int Height = 0;
    int Channels = 4; // 1 (grayscale), 3 (RGB) или 4 (RGBA), 8 бит на канал
    Filter FilterMode = Filter::Trilinear;
    Wrap WrapMode = Wrap::Repeat;
    bool GenerateMipmaps = true;
};

class Texture2D {
public:
    virtual ~Texture2D() = default;
    virtual void Bind(int unit) const = 0;
    // Нативный хендл бэкенда (для передачи в сторонние API вроде ImGui::Image).
    virtual unsigned int NativeHandle() const = 0;
};

// Одна грань кубической текстуры (порядок граней: +X,-X,+Y,-Y,+Z,-Z).
struct CubeFacePixels {
    int Width = 0;
    int Height = 0;
    int Channels = 3;
    const unsigned char* Pixels = nullptr;
};

class TextureCube {
public:
    virtual ~TextureCube() = default;
    virtual void Bind(int unit) const = 0;
};

// --- Рендер-таргеты (offscreen кадровые буферы) ---
enum class RenderTargetKind {
    ColorHDRWithDepth, // float-цвет (HDR, для пост-процессинга) + depth-буфер
    DepthOnly,         // только depth-текстура (карта теней); за границей — «освещено»
};

struct RenderTargetDesc {
    int Width = 0;
    int Height = 0;
    RenderTargetKind Kind = RenderTargetKind::ColorHDRWithDepth;
};

class RenderTarget {
public:
    virtual ~RenderTarget() = default;
    // Делает таргет активным и выставляет viewport под его размер.
    virtual void Bind() const = 0;
    // Пересоздаёт хранилище под новый размер (no-op, если не изменился).
    virtual void Resize(int width, int height) = 0;
    virtual int Width() const = 0;
    virtual int Height() const = 0;
    // Нативные хендлы текстур вложений (0 — вложения нет). Используются для
    // привязки как обычной текстуры (GraphicsDevice::BindTexture2D) и ImGui.
    virtual unsigned int ColorTextureHandle() const = 0;
    virtual unsigned int DepthTextureHandle() const = 0;
};

} // namespace sage::rhi
