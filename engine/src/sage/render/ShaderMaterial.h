#pragma once
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/rhi/Resources.h"

namespace sage::rhi {
class GraphicsDevice;
}

// ---------------------------------------------------------------------------
// ShaderMaterial — материал СВОЕГО шейдера: программа, любые параметры, любые
// текстуры и буферы, состояние отрисовки.
//
// ОТЛИЧИЕ ОТ Material (render/Material.h). Material — модель PBR: albedo,
// metallic, roughness, карты; свой шейдер там допускается, но объект остаётся
// частью штатного освещения и батчинга сцены. ShaderMaterial ничего о PBR не
// знает: у него нет «цвета» и «шероховатости», есть только то, что объявил
// автор шейдера. Им пользуется свой рендерер (RenderPipeline.h) — чанки
// вокселей, небо, эффекты пост-обработки. Штатные материалы от этого не
// меняются и работают как прежде.
//
// Программа берётся из ShaderLibrary ПО ИМЕНИ при каждом Apply: шейдер,
// пересобранный на лету, подхватывается без пересоздания материалов.
// ---------------------------------------------------------------------------
namespace sage::render {

class ShaderLibrary;

enum class BlendState { Opaque, Alpha, Premultiplied, Additive };

struct RenderState {
    BlendState Blend = BlendState::Opaque;
    bool DepthTest = true;
    bool DepthWrite = true;
    sage::rhi::DepthFunc Depth = sage::rhi::DepthFunc::Less;
    sage::rhi::CullMode Cull = sage::rhi::CullMode::Back;
    bool ColorWrite = true;
};

// Состояние, которого ждут все остальные проходы движка: непрозрачно, с
// глубиной, без лицевых отсечений изнанки. Проход, поменявший состояние, обязан
// вернуть это (см. GraphicsDevice::BlendEnabled — почему).
void ApplyRenderState(sage::rhi::GraphicsDevice& device, const RenderState& state);
inline void RestoreDefaultState(sage::rhi::GraphicsDevice& device) {
    ApplyRenderState(device, RenderState{});
}

class ShaderMaterial {
public:
    ShaderMaterial() = default;
    // Программа из библиотеки по имени (подхватывает пересборку на лету).
    ShaderMaterial(ShaderLibrary& library, std::string programName);
    // Готовая программа (материал ею не владеет).
    explicit ShaderMaterial(sage::rhi::ShaderProgram* program) : m_program(program) {}

    void Set(const std::string& name, float v);
    void Set(const std::string& name, int v);
    void Set(const std::string& name, const glm::vec2& v);
    void Set(const std::string& name, const glm::vec3& v);
    void Set(const std::string& name, const glm::vec4& v);
    void Set(const std::string& name, const glm::mat4& v);
    // Текстура в сэмплер name на юнит unit.
    void SetTexture(const std::string& name, int unit, sage::rhi::TextureHandle texture);
    // Буфер шейдера в binding = slot.
    void SetBuffer(int slot, const sage::rhi::GpuBuffer* buffer);
    bool Has(const std::string& name) const;

    RenderState State;

    sage::rhi::ShaderProgram* Program() const;

    // Включить программу, залить параметры, привязать текстуры и буферы,
    // выставить состояние. false — программы нет (не собралась): рисовать
    // этим материалом нельзя, и вызывающий узнаёт это, а не получает мусор.
    bool Apply(sage::rhi::GraphicsDevice& device) const;

private:
    enum class Kind { Float, Int, Vec2, Vec3, Vec4, Mat4 };
    struct Param {
        std::string Name;
        Kind Type = Kind::Float;
        glm::mat4 Value{0.0f}; // всё помещается в матрицу; читается по типу
        int I = 0;
    };
    struct TextureSlot {
        std::string Name;
        int Unit = 0;
        sage::rhi::TextureHandle Texture;
    };
    Param& Slot(const std::string& name, Kind type);

    ShaderLibrary* m_library = nullptr;
    std::string m_programName;
    sage::rhi::ShaderProgram* m_program = nullptr;
    std::vector<Param> m_params;
    std::vector<TextureSlot> m_textures;
    std::vector<std::pair<int, const sage::rhi::GpuBuffer*>> m_buffers;
};

} // namespace sage::render
