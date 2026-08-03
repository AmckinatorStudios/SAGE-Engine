#include "VulkanShader.h"
#include "VulkanDevice.h"

#include <cstring>
#include <mutex>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

namespace sage::rhi {

namespace {

// glslang требует однократной инициализации на процесс и не потокобезопасен в
// ней. Флаг + мьютекс, а не «инициализируем в Init устройства»: шейдеры
// компилируются и из фоновой загрузки ресурсов.
void EnsureGlslang() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

// Размер типа в блоке униформ по описанию рефлексии. glslang отдаёт тип
// числом OpenGL (GL_FLOAT_MAT4 и т.п.); нам нужен только размер, чтобы знать,
// сколько байт писать и не выйти за границу блока.
uint32_t SizeOfGlType(int glType, int arraySize) {
    const int count = arraySize > 0 ? arraySize : 1;
    switch (glType) {
        case 0x1406: return 4u * count;   // GL_FLOAT
        case 0x8B50: return 8u * count;   // GL_FLOAT_VEC2
        case 0x8B51: return 12u * count;  // GL_FLOAT_VEC3
        case 0x8B52: return 16u * count;  // GL_FLOAT_VEC4
        case 0x1404: return 4u * count;   // GL_INT
        case 0x8B56: return 4u * count;   // GL_BOOL
        case 0x8B5B: return 36u * count;  // GL_FLOAT_MAT3
        case 0x8B5C: return 64u * count;  // GL_FLOAT_MAT4
        default: return 0;
    }
}

bool IsSamplerType(int glType) {
    switch (glType) {
        case 0x8B5E: // GL_SAMPLER_2D
        case 0x8B60: // GL_SAMPLER_CUBE
        case 0x8B5F: // GL_SAMPLER_3D
        case 0x8B62: // GL_SAMPLER_2D_SHADOW
            return true;
        default:
            return false;
    }
}

} // namespace

CompiledShader CompileGlsl(const std::string& vertexSrc, const std::string& fragmentSrc,
                           const std::string& debugName) {
    EnsureGlslang();
    CompiledShader out;

    glslang::TShader vertex(EShLangVertex);
    glslang::TShader fragment(EShLangFragment);
    const char* vsrc = vertexSrc.c_str();
    const char* fsrc = fragmentSrc.c_str();
    vertex.setStrings(&vsrc, 1);
    fragment.setStrings(&fsrc, 1);

    for (glslang::TShader* s : {&vertex, &fragment}) {
        const EShLanguage stage = (s == &vertex) ? EShLangVertex : EShLangFragment;
        s->setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
        s->setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
        s->setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
        // ВОТ РАДИ ЧЕГО ВСЁ ЭТО: ослабленные правила разрешают свободные
        // униформы и сами собирают их в блок. Без этой строки шейдеры движка
        // не компилируются под Vulkan вообще — «uniform mat4 uModel;» вне блока
        // там незаконен.
        s->setEnvInputVulkanRulesRelaxed();
        // Привязки и локации раздаёт glslang: в исходниках движка их нет,
        // потому что GLSL 3.30 их и не требует.
        s->setAutoMapBindings(true);
        s->setAutoMapLocations(true);
    }

    const EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
    const TBuiltInResource* limits = GetDefaultResources();

    if (!vertex.parse(limits, 100, false, messages)) {
        LOG_ERROR("Vulkan") << "вершинный шейдер (" << debugName << "): " << vertex.getInfoLog();
        return out;
    }
    if (!fragment.parse(limits, 100, false, messages)) {
        LOG_ERROR("Vulkan") << "фрагментный шейдер (" << debugName << "): " << fragment.getInfoLog();
        return out;
    }

    glslang::TProgram program;
    program.addShader(&vertex);
    program.addShader(&fragment);
    if (!program.link(messages)) {
        LOG_ERROR("Vulkan") << "линковка шейдера (" << debugName << "): " << program.getInfoLog();
        return out;
    }
    // mapIO обязателен при автоматических привязках: без него собранные
    // glslang'ом номера остаются несогласованными между стадиями.
    if (!program.mapIO()) {
        LOG_ERROR("Vulkan") << "распределение привязок (" << debugName << "): "
                            << program.getInfoLog();
        return out;
    }

    glslang::SpvOptions spv;
    spv.disableOptimizer = true;    // оптимизатор SPIRV-Tools не собран (см. CMake)
    spv.validate = false;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangVertex), out.Vertex, &spv);
    glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment), out.Fragment, &spv);

    // --- Рефлексия: имя → смещение -----------------------------------------
    program.buildReflection(EShReflectionSeparateBuffers | EShReflectionAllBlockVariables);

    const int count = program.getNumUniformVariables();
    for (int i = 0; i < count; ++i) {
        const glslang::TObjectReflection& u = program.getUniform(i);
        if (IsSamplerType(u.glDefineType)) {
            if (u.getBinding() >= 0) out.Samplers[u.name] = (uint32_t)u.getBinding();
            continue;
        }
        if (u.offset < 0) continue; // не в блоке — попасть сюда неоткуда, но проверим
        const uint32_t size = SizeOfGlType(u.glDefineType, u.size);
        if (size == 0) continue;
        out.Uniforms[u.name] = CompiledShader::UniformSlot{(uint32_t)u.offset, size};
        out.UniformBlockSize = std::max(out.UniformBlockSize, (uint32_t)u.offset + size);
    }

    // Блок свободных униформ у glslang один; его привязку берём из описания
    // блоков. Если блока нет вовсе (шейдер без униформ) — размер ноль, и буфер
    // не создаётся.
    const int blocks = program.getNumUniformBlocks();
    for (int i = 0; i < blocks; ++i) {
        const glslang::TObjectReflection& b = program.getUniformBlock(i);
        if (b.getBinding() >= 0) {
            out.UniformBinding = (uint32_t)b.getBinding();
            if (b.size > 0) out.UniformBlockSize = std::max(out.UniformBlockSize, (uint32_t)b.size);
            break;
        }
    }
    return out;
}

// ============================================================================
//  VulkanShaderProgram
// ============================================================================

VulkanShaderProgram::VulkanShaderProgram(VulkanDevice& device, const std::string& vertexSrc,
                                         const std::string& fragmentSrc)
    : m_device(device) {
    m_compiled = CompileGlsl(vertexSrc, fragmentSrc, "материал");
    if (!m_compiled.Ok()) return;

    auto makeModule = [&](const std::vector<uint32_t>& code) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size() * sizeof(uint32_t);
        info.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        vk::Check(vkCreateShaderModule(device.Handle(), &info, nullptr, &module),
                  "vkCreateShaderModule");
        return module;
    };
    m_vertex = makeModule(m_compiled.Vertex);
    m_fragment = makeModule(m_compiled.Fragment);
    if (!Valid()) return;

    m_shadow.assign(m_compiled.UniformBlockSize, 0);

    // Раскладка набора дескрипторов: один буфер униформ + все сэмплеры.
    // Видимость — обе стадии сразу: разбирать, какая униформа нужна вершинной,
    // а какая фрагментной, пришлось бы по рефлексии каждой стадии отдельно, а
    // выигрыш от этого нулевой при одном наборе на программу.
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    if (m_compiled.UniformBlockSize > 0) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = m_compiled.UniformBinding;
        b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(b);
    }
    for (const auto& [name, binding] : m_compiled.Samplers) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = binding;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(b);
        m_samplerUnits[binding] = 0; // юнит назначит SetInt
    }

    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = (uint32_t)bindings.size();
    setInfo.pBindings = bindings.data();
    if (!vk::Check(vkCreateDescriptorSetLayout(device.Handle(), &setInfo, nullptr, &m_setLayout),
                   "vkCreateDescriptorSetLayout")) {
        return;
    }

    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &m_setLayout;
    vk::Check(vkCreatePipelineLayout(device.Handle(), &layout, nullptr, &m_pipelineLayout),
              "vkCreatePipelineLayout");
}

VulkanShaderProgram::~VulkanShaderProgram() {
    VkDevice dev = m_device.Handle();
    if (dev == VK_NULL_HANDLE) return;
    if (m_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_vertex != VK_NULL_HANDLE) vkDestroyShaderModule(dev, m_vertex, nullptr);
    if (m_fragment != VK_NULL_HANDLE) vkDestroyShaderModule(dev, m_fragment, nullptr);
}

void VulkanShaderProgram::Use() const { m_device.BindShader(this); }

void VulkanShaderProgram::Write(const std::string& name, const void* data, uint32_t bytes) const {
    auto it = m_compiled.Uniforms.find(name);
    if (it == m_compiled.Uniforms.end()) return; // нет такой униформы — как в GL
    const uint32_t offset = it->second.Offset;
    // Пишем не больше, чем объявлено в шейдере: массив на 64 кости, которому
    // передали 128, обязан обрезаться, а не затереть соседние униформы.
    const uint32_t size = std::min(bytes, it->second.Size);
    if (offset + size > m_shadow.size()) return;
    std::memcpy(m_shadow.data() + offset, data, size);
}

void VulkanShaderProgram::SetMat4(const std::string& name, const glm::mat4& v) const {
    Write(name, &v[0][0], 64);
}
void VulkanShaderProgram::SetMat4Array(const std::string& name, const glm::mat4* v,
                                       int count) const {
    Write(name, v, (uint32_t)(64 * std::max(count, 0)));
}
void VulkanShaderProgram::SetIntArray(const std::string& name, const int* v, int count) const {
    Write(name, v, (uint32_t)(4 * std::max(count, 0)));
}
void VulkanShaderProgram::SetFloatArray(const std::string& name, const float* v, int count) const {
    Write(name, v, (uint32_t)(4 * std::max(count, 0)));
}
void VulkanShaderProgram::SetVec4(const std::string& name, const glm::vec4& v) const {
    Write(name, &v.x, 16);
}
void VulkanShaderProgram::SetVec3(const std::string& name, const glm::vec3& v) const {
    Write(name, &v.x, 12);
}
void VulkanShaderProgram::SetVec2(const std::string& name, const glm::vec2& v) const {
    Write(name, &v.x, 8);
}
void VulkanShaderProgram::SetFloat(const std::string& name, float v) const {
    Write(name, &v, 4);
}

void VulkanShaderProgram::SetInt(const std::string& name, int v) const {
    // ЕДИНСТВЕННОЕ МЕСТО, ГДЕ SetInt ЗНАЧИТ НЕ ТО ЖЕ, ЧТО В OpenGL. Там
    // `SetInt("uAlbedo", 2)` означает «сэмплер uAlbedo читает текстурный юнит 2».
    // В Vulkan сэмплер — элемент набора дескрипторов, и номер юнита в блок
    // униформ писать нельзя (его там нет). Запоминаем сопоставление, а
    // собирается набор в момент отрисовки.
    auto sampler = m_compiled.Samplers.find(name);
    if (sampler != m_compiled.Samplers.end()) {
        m_samplerUnits[sampler->second] = v;
        return;
    }
    Write(name, &v, 4);
}

} // namespace sage::rhi
