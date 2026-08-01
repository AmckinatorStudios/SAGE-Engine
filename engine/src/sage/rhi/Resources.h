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
    // Массив матриц (напр. палитра костей скелетной анимации uBones[N]).
    virtual void SetMat4Array(const std::string& name, const glm::mat4* v, int count) const = 0;
    // Массивы скаляров — нужны блендшейпам: индексы и веса активных морф-целей
    // передаются пачкой, по одному вызову на кадр вместо N.
    virtual void SetIntArray(const std::string& name, const int* v, int count) const = 0;
    virtual void SetFloatArray(const std::string& name, const float* v, int count) const = 0;
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
    // Диапазон индексов [firstIndex, firstIndex+indexCount) — для батчей,
    // разбитых на сегменты (UI: смена текстуры/scissor-маски между сегментами
    // одного общего буфера, без перезаливки геометрии).
    virtual void DrawIndexedRange(size_t firstIndex, size_t indexCount) const = 0;
    virtual void DrawArrays(size_t vertexCount) const = 0;
    virtual void DrawInstanced(size_t vertexCount, size_t instanceCount) const = 0;
    // Индексированная инстансная отрисовка (батчинг мешей: один вызов на группу
    // одинаковых мешей, per-instance поток — модельная матрица + цвет).
    virtual void DrawIndexedInstanced(size_t indexCount, size_t instanceCount) const = 0;
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
    // HDR-режим: пиксели — float* (16F-хранилище на GPU). Нужен лайтмапам GI:
    // запечённая освещённость линейна и может быть > 1.0.
    bool FloatPixels = false;
};

class Texture2D {
public:
    virtual ~Texture2D() = default;
    virtual void Bind(int unit) const = 0;
    // Нативный хендл бэкенда (для передачи в сторонние API вроде ImGui::Image).
    virtual unsigned int NativeHandle() const = 0;
};

// --- Объёмная (3D) текстура — GI-объём световых проб (см. sage/gi) ---
// Хранит float-данные (16F): аппаратная трилинейная интерполяция между пробами
// достаётся бесплатно при семплировании в шейдере.
struct Texture3DDesc {
    int Width = 0;   // X — колонки сетки проб
    int Height = 0;  // Y
    int Depth = 0;   // Z
    int Channels = 4;
};

class Texture3D {
public:
    virtual ~Texture3D() = default;
    virtual void Bind(int unit) const = 0;
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

// Кубический рендер-таргет: цветной cubemap с полной цепочкой мип-уровней, в
// КАЖДУЮ грань и каждый мип которого можно рисовать.
//
// Нужен отражениям, и обычным RenderTarget его не заменить по двум причинам.
// Первая: отражение читается по НАПРАВЛЕНИЮ (samplerCube), а шесть отдельных
// 2D-текстур пришлось бы выбирать и фильтровать вручную, теряя правильную
// фильтрацию на стыках граней. Вторая: шероховатость материала выбирает мип, и
// мипы должны быть не просто уменьшением, а размытым окружением — значит, в
// них надо РИСОВАТЬ, а не только читать.
//
// Глубина общая на все грани (renderbuffer): грани рисуются по очереди, и
// хранить шесть буферов глубины ради этого незачем.
class CubeRenderTarget {
public:
    virtual ~CubeRenderTarget() = default;
    // Делает активными грань face (0..5: +X,-X,+Y,-Y,+Z,-Z) и мип mip,
    // выставляя viewport под размер этого мипа.
    virtual void BindFace(int face, int mip = 0) = 0;
    // Достраивает мипы фильтрацией GPU. Нужно, когда мипы не рисуются вручную:
    // так получается дешёвое приближение размытого окружения.
    virtual void GenerateMips() = 0;
    virtual int Size() const = 0;      // сторона нулевого мипа
    virtual int MipLevels() const = 0;
    // Привязать как cubemap-текстуру к юниту (для чтения из шейдера).
    virtual void Bind(int unit) const = 0;
    virtual unsigned int NativeHandle() const = 0;
};

struct CubeRenderTargetDesc {
    int Size = 128;        // сторона грани
    bool WithDepth = true; // нужен ли буфер глубины (проходу сцены — да, фильтру — нет)
    int MipLevels = 0;     // 0 — полная цепочка от Size до 1
};

// --- Рендер-таргеты (offscreen кадровые буферы) ---
enum class RenderTargetKind {
    ColorHDRWithDepth, // float-цвет (HDR, для пост-процессинга) + depth-текстура
    DepthOnly,         // только depth-текстура (карта теней); за границей — «освещено»
    ColorHDR,          // только float-цвет (HDR), без глубины — промежуточные буферы пост-эффектов
};

struct RenderTargetDesc {
    int Width = 0;
    int Height = 0;
    RenderTargetKind Kind = RenderTargetKind::ColorHDRWithDepth;
    // Число сэмплов на пиксель (MSAA). 1 — обычный таргет.
    //
    // Сглаживание кромок бывает двух разных природ, и путать их нельзя.
    // Экранный фильтр (FXAA) работает по ГОТОВОЙ картинке: он ищет перепад
    // яркости и замывает его — то есть догадывается о кромке по результату, уже
    // потерявшему информацию. На пологой кромке (горизонт, длинная грань пола)
    // догадаться не по чему, и лесенка остаётся.
    //
    // MSAA решает задачу там, где информация ещё есть: растеризатор считает
    // ПОКРЫТИЕ пикселя геометрией по нескольким точкам и смешивает по нему.
    // Кромка любого наклона получает честные промежуточные значения.
    //
    // Поддерживается только у ColorHDRWithDepth: сглаживать промежуточные
    // буферы пост-эффектов бессмысленно (в них нет геометрии), а карту теней —
    // вредно (глубину нельзя усреднять, среднее двух глубин не является
    // глубиной ничего).
    int Samples = 1;
};

class RenderTarget {
public:
    virtual ~RenderTarget() = default;
    // Переносит многосэмпловое содержимое в обычные текстуры, которые отдают
    // ColorTextureHandle/DepthTextureHandle. У таргета без MSAA — пустышка,
    // поэтому вызывать можно всегда и не спрашивать, включён ли он.
    //
    // Отдельным шагом, а не внутри ColorTextureHandle, намеренно: разрешение
    // стоит полного копирования кадра, и делать его на каждое обращение к
    // текстуре значило бы платить за него по нескольку раз за проход.
    virtual void Resolve() {}
    // Сколько сэмплов на пиксель реально выделено (1 — MSAA нет).
    virtual int Samples() const { return 1; }
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
