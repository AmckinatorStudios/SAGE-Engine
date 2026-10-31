#include "sage/render/ShaderMaterial.h"

#include "sage/render/ShaderLibrary.h"
#include "sage/rhi/GraphicsDevice.h"

namespace sage::render {

void ApplyRenderState(sage::rhi::GraphicsDevice& device, const RenderState& s) {
    using BM = sage::rhi::GraphicsDevice::BlendMode;
    switch (s.Blend) {
        case BlendState::Opaque: device.SetBlend(false); break;
        case BlendState::Alpha:
            device.SetBlend(true);
            device.SetBlendMode(BM::Alpha);
            break;
        case BlendState::Premultiplied:
            device.SetBlend(true);
            device.SetBlendMode(BM::Premultiplied);
            break;
        case BlendState::Additive:
            device.SetBlend(true);
            device.SetBlendMode(BM::Additive);
            break;
    }
    device.SetDepthTest(s.DepthTest);
    device.SetDepthWrite(s.DepthWrite);
    device.SetDepthFunc(s.Depth);
    device.SetCullMode(s.Cull);
    device.SetColorWrite(s.ColorWrite);
}

ShaderMaterial::ShaderMaterial(ShaderLibrary& library, std::string programName)
    : m_library(&library), m_programName(std::move(programName)) {}

sage::rhi::ShaderProgram* ShaderMaterial::Program() const {
    if (m_library) return m_library->Get(m_programName);
    return m_program;
}

ShaderMaterial::Param& ShaderMaterial::Slot(const std::string& name, Kind type) {
    for (Param& p : m_params) {
        if (p.Name == name) {
            p.Type = type;
            return p;
        }
    }
    m_params.push_back({name, type, glm::mat4(0.0f), 0});
    return m_params.back();
}

void ShaderMaterial::Set(const std::string& name, float v) { Slot(name, Kind::Float).Value[0][0] = v; }
void ShaderMaterial::Set(const std::string& name, int v) { Slot(name, Kind::Int).I = v; }
void ShaderMaterial::Set(const std::string& name, const glm::vec2& v) {
    Slot(name, Kind::Vec2).Value[0] = glm::vec4(v, 0.0f, 0.0f);
}
void ShaderMaterial::Set(const std::string& name, const glm::vec3& v) {
    Slot(name, Kind::Vec3).Value[0] = glm::vec4(v, 0.0f);
}
void ShaderMaterial::Set(const std::string& name, const glm::vec4& v) { Slot(name, Kind::Vec4).Value[0] = v; }
void ShaderMaterial::Set(const std::string& name, const glm::mat4& v) { Slot(name, Kind::Mat4).Value = v; }

void ShaderMaterial::SetTexture(const std::string& name, int unit, sage::rhi::TextureHandle texture) {
    for (TextureSlot& t : m_textures) {
        if (t.Name == name) {
            t.Unit = unit;
            t.Texture = texture;
            return;
        }
    }
    m_textures.push_back({name, unit, texture});
}

void ShaderMaterial::SetBuffer(int slot, const sage::rhi::GpuBuffer* buffer) {
    for (auto& b : m_buffers) {
        if (b.first == slot) {
            b.second = buffer;
            return;
        }
    }
    m_buffers.emplace_back(slot, buffer);
}

bool ShaderMaterial::Has(const std::string& name) const {
    for (const Param& p : m_params)
        if (p.Name == name) return true;
    for (const TextureSlot& t : m_textures)
        if (t.Name == name) return true;
    return false;
}

bool ShaderMaterial::Apply(sage::rhi::GraphicsDevice& device) const {
    sage::rhi::ShaderProgram* program = Program();
    if (!program) return false;
    program->Use();
    for (const Param& p : m_params) {
        switch (p.Type) {
            case Kind::Float: program->SetFloat(p.Name, p.Value[0][0]); break;
            case Kind::Int: program->SetInt(p.Name, p.I); break;
            case Kind::Vec2: program->SetVec2(p.Name, glm::vec2(p.Value[0])); break;
            case Kind::Vec3: program->SetVec3(p.Name, glm::vec3(p.Value[0])); break;
            case Kind::Vec4: program->SetVec4(p.Name, p.Value[0]); break;
            case Kind::Mat4: program->SetMat4(p.Name, p.Value); break;
        }
    }
    for (const TextureSlot& t : m_textures) {
        device.BindTexture2D(t.Unit, t.Texture);
        program->SetInt(t.Name, t.Unit);
    }
    for (const auto& [slot, buffer] : m_buffers) device.BindBuffer(slot, buffer);
    ApplyRenderState(device, State);
    return true;
}

} // namespace sage::render
