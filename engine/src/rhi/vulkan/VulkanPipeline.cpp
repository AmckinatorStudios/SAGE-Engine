#include "VulkanPipeline.h"
#include "VulkanDevice.h"
#include "VulkanResources.h"
#include "VulkanShader.h"

#include <algorithm>
#include <functional>

namespace sage::rhi {

VkFormat AttributeFormat(AttribType type, int components) {
    if (type == AttribType::UByteNorm) {
        switch (components) {
            case 1: return VK_FORMAT_R8_UNORM;
            case 2: return VK_FORMAT_R8G8_UNORM;
            case 3: return VK_FORMAT_R8G8B8_UNORM;
            default: return VK_FORMAT_R8G8B8A8_UNORM;
        }
    }
    switch (components) {
        case 1: return VK_FORMAT_R32_SFLOAT;
        case 2: return VK_FORMAT_R32G32_SFLOAT;
        case 3: return VK_FORMAT_R32G32B32_SFLOAT;
        default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

namespace {

size_t HashLayout(const VertexLayout& layout) {
    size_t h = (size_t)layout.Stride * 131 + (size_t)layout.InstanceStride;
    auto mix = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    for (const VertexAttribute& a : layout.Attributes) {
        mix((size_t)a.Location * 97 + (size_t)a.Components * 7 + (size_t)a.Type * 3 +
            (size_t)a.Offset);
    }
    for (const VertexAttribute& a : layout.InstanceAttributes) {
        mix((size_t)a.Location * 89 + (size_t)a.Components * 11 + (size_t)a.Type * 5 +
            (size_t)a.Offset + 1);
    }
    return h;
}

} // namespace

// ============================================================================
//  VulkanGeometry
// ============================================================================

VulkanGeometry::VulkanGeometry(VulkanDevice& device, const VertexLayout& layout)
    : m_device(device), m_layout(layout), m_layoutHash(HashLayout(layout)) {}

void VulkanGeometry::SetVertexData(const void* data, size_t bytes, bool dynamic) {
    if (bytes == 0) return;
    // Динамические данные (UI, текст, отладочные линии) живут в памяти, видимой
    // процессору: копировать их через staging каждый кадр дороже, чем чуть
    // медленнее читать с GPU. Статические уезжают в память устройства.
    const MemoryUse use = dynamic ? MemoryUse::CpuToGpu : MemoryUse::GpuOnly;
    if (!m_vertex.Create(m_device, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, use)) return;
    m_vertex.Upload(m_device, data, bytes);
}

void VulkanGeometry::SetIndexData(const unsigned int* indices, size_t count, bool dynamic) {
    if (!indices || count == 0) return;
    const size_t bytes = count * sizeof(unsigned int);
    const MemoryUse use = dynamic ? MemoryUse::CpuToGpu : MemoryUse::GpuOnly;
    if (!m_index.Create(m_device, bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, use)) return;
    m_index.Upload(m_device, indices, bytes);
}

void VulkanGeometry::SetInstanceData(const void* data, size_t bytes) {
    if (!data || bytes == 0) return;
    // Инстансный поток перезаливается каждый кадр по определению — всегда
    // видимая процессору память.
    if (!m_instance.Create(m_device, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           MemoryUse::CpuToGpu)) {
        return;
    }
    m_instance.Upload(m_device, data, bytes);
}

void VulkanGeometry::DrawIndexed(size_t indexCount) const {
    m_device.Draw(*this, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, (uint32_t)indexCount, 0, 1);
}
void VulkanGeometry::DrawIndexedRange(size_t firstIndex, size_t indexCount) const {
    m_device.Draw(*this, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, (uint32_t)indexCount,
                  (uint32_t)firstIndex, 1);
}
void VulkanGeometry::DrawArrays(size_t vertexCount) const {
    m_device.Draw(*this, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, (uint32_t)vertexCount, 0, 0, 1);
}
void VulkanGeometry::DrawInstanced(size_t vertexCount, size_t instanceCount) const {
    m_device.Draw(*this, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, (uint32_t)vertexCount, 0, 0,
                  (uint32_t)instanceCount);
}
void VulkanGeometry::DrawIndexedInstanced(size_t indexCount, size_t instanceCount) const {
    m_device.Draw(*this, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, (uint32_t)indexCount, 0,
                  (uint32_t)instanceCount);
}
void VulkanGeometry::DrawIndexedInstancedRange(size_t firstIndex, size_t indexCount,
                                               size_t instanceCount) const {
    m_device.Draw(*this, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, (uint32_t)indexCount,
                  (uint32_t)firstIndex, (uint32_t)instanceCount);
}
void VulkanGeometry::DrawLines(size_t vertexCount) const {
    m_device.Draw(*this, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, (uint32_t)vertexCount, 0, 0, 1);
}

// ============================================================================
//  Кэш конвейеров и отрисовка (методы устройства — живут здесь, рядом с тем,
//  что они собирают)
// ============================================================================

uint32_t VulkanDevice::PackState() const {
    // Упаковка в биты, а не структура в ключе: ключ хешируется на каждый вызов
    // отрисовки, и чем он компактнее, тем дешевле.
    uint32_t bits = 0;
    bits |= (uint32_t)m_state.Blend << 0;
    bits |= (uint32_t)m_state.DepthTest << 1;
    bits |= (uint32_t)m_state.DepthWrite << 2;
    bits |= (uint32_t)m_state.ColorWrite << 3;
    bits |= (uint32_t)(m_state.Depth == DepthFunc::LessEqual) << 4;
    bits |= (uint32_t)m_state.Cull << 5;                  // 2 бита
    bits |= (uint32_t)(m_state.Front == FrontFace::Clockwise) << 7;
    bits |= (uint32_t)(m_state.Polygon == PolygonMode::Line) << 8;
    return bits;
}

VkPipeline VulkanDevice::PipelineFor(const PipelineKey& key, const VulkanGeometry& geometry) {
    auto it = m_pipelines.find(key);
    if (it != m_pipelines.end()) return it->second;

    const VertexLayout& layout = geometry.Layout();

    // --- Формат вершин ------------------------------------------------------
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    if (layout.Stride > 0) {
        bindings.push_back({0, (uint32_t)layout.Stride, VK_VERTEX_INPUT_RATE_VERTEX});
        for (const VertexAttribute& a : layout.Attributes) {
            attributes.push_back({(uint32_t)a.Location, 0, AttributeFormat(a.Type, a.Components),
                                  (uint32_t)a.Offset});
        }
    }
    if (layout.InstanceStride > 0 && geometry.HasInstances()) {
        // Второй поток продвигается раз на ИНСТАНС, а не на вершину — этим
        // инстансный батчинг и держится.
        bindings.push_back({1, (uint32_t)layout.InstanceStride, VK_VERTEX_INPUT_RATE_INSTANCE});
        for (const VertexAttribute& a : layout.InstanceAttributes) {
            attributes.push_back({(uint32_t)a.Location, 1, AttributeFormat(a.Type, a.Components),
                                  (uint32_t)a.Offset});
        }
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = (uint32_t)bindings.size();
    vertexInput.pVertexBindingDescriptions = bindings.data();
    vertexInput.vertexAttributeDescriptionCount = (uint32_t)attributes.size();
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = key.Topology;

    // --- Viewport: динамический ---------------------------------------------
    //
    // Размеры в конвейер НЕ запекаются: иначе смена области вывода (а её меняет
    // каждый проход теней и каждый буфер пост-обработки) плодила бы по
    // конвейеру на размер. Динамическое состояние — ровно для этого.
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    // Диапазон глубины OpenGL ([-1, 1]) вместо вулкановского [0, 1]. Без этого
    // матрицы проекции движка давали бы неверную глубину на дальних планах
    // (см. CreateLogicalDevice — без расширения бэкенд не поднимается вовсе).
    VkPipelineViewportDepthClipControlCreateInfoEXT depthClip{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    depthClip.negativeOneToOne = VK_TRUE;
    viewport.pNext = &depthClip;

    const bool blend = (key.StateBits >> 0) & 1;
    const bool depthTest = (key.StateBits >> 1) & 1;
    const bool depthWrite = (key.StateBits >> 2) & 1;
    const bool colorWrite = (key.StateBits >> 3) & 1;
    const bool lessEqual = (key.StateBits >> 4) & 1;
    const CullMode cull = (CullMode)((key.StateBits >> 5) & 3);
    const bool clockwise = (key.StateBits >> 7) & 1;
    const bool wireframe = (key.StateBits >> 8) & 1;

    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    raster.cullMode = cull == CullMode::Off      ? VK_CULL_MODE_NONE
                      : cull == CullMode::Front  ? VK_CULL_MODE_FRONT_BIT
                                                 : VK_CULL_MODE_BACK_BIT;
    // Обмотка: у RHI лицевой считается против часовой (соглашение движка и всех
    // его генераторов геометрии), зеркальные проходы просят обратную.
    raster.frontFace = clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = key.Samples;

    VkPipelineDepthStencilStateCreateInfo depth{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = depthTest ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = lessEqual ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;
    depth.maxDepthBounds = 1.0f;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = blend ? VK_TRUE : VK_FALSE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    // Запись цвета выключают проходы, которым нужна только глубина (проверка
    // перекрытия рисует коробки-заменители — попадать в кадр они не должны).
    blendAttachment.colorWriteMask =
        colorWrite ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)
                   : 0;

    VkPipelineColorBlendStateCreateInfo colorBlend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = key.Vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = key.Fragment;
    stages[1].pName = "main";

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamic;
    info.layout = key.Layout;
    info.renderPass = key.Pass;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!vk::Check(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
                   "vkCreateGraphicsPipelines")) {
        return VK_NULL_HANDLE;
    }
    m_pipelines.emplace(key, pipeline);
    return pipeline;
}

bool VulkanDevice::AddUniformBlock() {
    auto block = std::make_unique<VulkanBuffer>();
    if (!block->Create(*this, kUniformBlockBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                       MemoryUse::CpuToGpu)) {
        LOG_ERROR("Vulkan") << "не удалось выделить ещё блок униформ ("
                            << (kUniformBlockBytes >> 20) << " МиБ)";
        return false;
    }
    m_uniformBlocks.push_back(std::move(block));
    return true;
}

bool VulkanDevice::ReserveUniforms(VkDeviceSize size, const void* data,
                                   VkDescriptorBufferInfo& out) {
    if (size > kUniformBlockBytes) return false;   // блок униформ крупнее целого блока — не бывает
    if (m_uniformBlocks.empty() && !AddUniformBlock()) return false;

    VkDeviceSize aligned =
        (m_uniformOffset + m_uniformAlignment - 1) / m_uniformAlignment * m_uniformAlignment;
    if (aligned + size > m_uniformBlocks[m_uniformBlock]->Size()) {
        // Текущий блок кончился — переходим к следующему. Он либо уже есть с
        // прошлых кадров, либо создаётся сейчас и остаётся навсегда.
        if (m_uniformBlock + 1 >= m_uniformBlocks.size() && !AddUniformBlock()) return false;
        ++m_uniformBlock;
        m_uniformOffset = 0;
        aligned = 0;
    }

    VulkanBuffer& block = *m_uniformBlocks[m_uniformBlock];
    m_uniformOffset = aligned + size;
    block.Upload(*this, data, size, aligned);

    out.buffer = block.Handle();
    out.offset = aligned;
    out.range = size;
    return true;
}

bool VulkanDevice::AddDescriptorPool() {
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = kDescriptorSetsPerPool;
    sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[1].descriptorCount = kDescriptorSetsPerPool * 8;

    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.maxSets = kDescriptorSetsPerPool;
    info.poolSizeCount = 2;
    info.pPoolSizes = sizes;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (!vk::Check(vkCreateDescriptorPool(m_device, &info, nullptr, &pool),
                   "vkCreateDescriptorPool")) {
        return false;
    }
    m_descriptors.Blocks.push_back(pool);
    return true;
}

VkDescriptorSet VulkanDevice::AllocateDescriptorSet(VkDescriptorSetLayout layout) {
    if (m_descriptors.Blocks.empty() && !AddDescriptorPool()) return VK_NULL_HANDLE;

    // Не больше одного нового пула за вызов: если и из свежего пула набор не
    // достался, дело не в его размере, и плодить пулы бессмысленно.
    for (int attempt = 0; attempt < 2; ++attempt) {
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = m_descriptors.Blocks[m_descriptors.Current];
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &layout;

        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult r = vkAllocateDescriptorSets(m_device, &alloc, &set);
        if (r == VK_SUCCESS) return set;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
            vk::Check(r, "vkAllocateDescriptorSets");
            return VK_NULL_HANDLE;
        }
        if (m_descriptors.Current + 1 >= m_descriptors.Blocks.size() && !AddDescriptorPool())
            return VK_NULL_HANDLE;
        ++m_descriptors.Current;
    }
    return VK_NULL_HANDLE;
}

VkDescriptorSet VulkanDevice::DescriptorSetFor(const VulkanShaderProgram& shader) {
    const CompiledShader& compiled = shader.Compiled();

    // Униформы резервируются ДО набора дескрипторов.
    //
    // Иначе выход из-за нехватки места в кольце оставлял бы уже выделенный
    // набор висеть в пуле до конца кадра: пул выедался ещё быстрее — и на
    // llvmpipe исчерпанный пул валит vkAllocateDescriptorSets с порчей кучи,
    // а не возвращает ошибку.
    VkDescriptorBufferInfo bufferInfo{};
    const bool wantUniforms = compiled.UniformBlockSize > 0;
    if (wantUniforms &&
        !ReserveUniforms(compiled.UniformBlockSize, shader.UniformShadow().data(), bufferInfo)) {
        static bool said = false;
        if (!said) {
            said = true;
            LOG_WARN("Vulkan") << "не нашлось места под униформы — вызов отрисовки пропущен";
        }
        return VK_NULL_HANDLE;
    }

    VkDescriptorSet set = AllocateDescriptorSet(shader.SetLayout());
    if (set == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    std::vector<VkWriteDescriptorSet> writes;

    if (wantUniforms) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = compiled.UniformBinding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &bufferInfo;
        writes.push_back(w);
    }

    // Сэмплеры: привязка шейдера → назначенный ей юнит → текстура в этом юните.
    std::vector<VkDescriptorImageInfo> images;
    images.reserve(compiled.Samplers.size());
    for (const auto& [name, binding] : compiled.Samplers) {
        auto unitIt = shader.SamplerUnits().find(binding);
        const int unit = unitIt == shader.SamplerUnits().end() ? 0 : unitIt->second;
        const TextureBinding* bound = BoundTexture(unit);
        if (!bound || bound->View == VK_NULL_HANDLE) continue;

        VkDescriptorImageInfo img{};
        img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        img.imageView = bound->View;
        img.sampler = bound->Sampler;
        images.push_back(img);

        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &images.back();
        writes.push_back(w);
    }

    if (!writes.empty()) {
        vkUpdateDescriptorSets(m_device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    }
    return set;
}

void VulkanDevice::Draw(const VulkanGeometry& geometry, VkPrimitiveTopology topology,
                        uint32_t vertexCount, uint32_t indexCount, uint32_t firstIndex,
                        uint32_t instanceCount) {
    if (!Ready() || !m_shader || !m_target) return;
    if (!m_shader->Valid()) return;

    // Рисовать можно только внутри прохода. Открываем СОХРАНЯЮЩИЙ: очистка —
    // отдельная команда движка, и подменять её здесь значило бы стирать то,
    // что нарисовали до нас.
    EnsureRenderPass(/*wantClear=*/false);
    if (!m_inPass) return;

    PipelineKey key;
    key.Vertex = m_shader->VertexModule();
    key.Fragment = m_shader->FragmentModule();
    key.Layout = m_shader->Layout();
    key.Pass = m_target->LoadPass();
    key.LayoutHash = geometry.LayoutHash();
    key.Topology = topology;
    key.Samples = (VkSampleCountFlagBits)std::max(m_target->Samples(), 1);
    key.StateBits = PackState();

    VkPipeline pipeline = PipelineFor(key, geometry);
    if (pipeline == VK_NULL_HANDLE) return;

    VkDescriptorSet set = DescriptorSetFor(*m_shader);
    if (set == VK_NULL_HANDLE && m_shader->Compiled().UniformBlockSize > 0) return;

    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (set != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shader->Layout(), 0, 1,
                                &set, 0, nullptr);
    }
    ApplyViewportAndScissor();

    VkBuffer buffers[2] = {geometry.VertexBuffer(), geometry.InstanceBuffer()};
    VkDeviceSize offsets[2] = {0, 0};
    const uint32_t bufferCount =
        (geometry.HasInstances() && geometry.Layout().InstanceStride > 0) ? 2u : 1u;
    if (buffers[0] == VK_NULL_HANDLE) return;
    vkCmdBindVertexBuffers(m_cmd, 0, bufferCount, buffers, offsets);

    if (indexCount > 0 && geometry.HasIndices()) {
        vkCmdBindIndexBuffer(m_cmd, geometry.IndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(m_cmd, indexCount, std::max(instanceCount, 1u), firstIndex, 0, 0);
    } else if (vertexCount > 0) {
        vkCmdDraw(m_cmd, vertexCount, std::max(instanceCount, 1u), 0, 0);
    }
}

} // namespace sage::rhi
