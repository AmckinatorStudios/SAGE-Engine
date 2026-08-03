#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "VulkanCommon.h"
#include "VulkanMemory.h"
#include "sage/rhi/Resources.h"

// ---------------------------------------------------------------------------
// Шейдеры: GLSL 3.30 движка → SPIR-V, и то, ради чего это оказалось непросто —
// УНИФОРМЫ ПО ИМЕНИ.
//
// ЧТО ЗДЕСЬ ГЛАВНАЯ ТРУДНОСТЬ. Весь движок задаёт униформы так:
//
//     shader->SetMat4("uModel", model);
//
// В OpenGL это законно: «свободные» униформы (uniform mat4 uModel;) живут в
// неявном наборе по умолчанию, и драйвер сам находит их по имени. В Vulkan
// такого набора НЕТ ВООБЩЕ: любая униформа обязана лежать в блоке (UBO) или
// быть push-константой, а адресуется она смещением, а не именем.
//
// Переписывать под это одиннадцать тысяч строк рендера — не вариант: это не
// перенос на второй бэкенд, а другой движок. Решение состоит из двух частей:
//
//   1) glslang умеет «ослабленные правила Vulkan»
//      (setEnvInputVulkanRulesRelaxed). Он САМ собирает все свободные униформы
//      шейдера в один блок и раздаёт им привязки. Исходники шейдеров при этом
//      не меняются ни на строку — тот же текст компилируется и для OpenGL, и
//      для Vulkan.
//
//   2) после линковки берётся рефлексия: имя → смещение в этом блоке. Дальше
//      SetMat4("uModel", …) превращается в запись по смещению в буфер униформ.
//
// ЦЕНА. Поиск по хеш-таблице на каждую установку униформы (в GL был кэш локаций
// — та же цена) и буфер униформ на кадр. Это заметно дороже, чем нормальный
// Vulkan-рендер с заранее разложенными структурами, и об этом честно сказано в
// VulkanDevice.h: слой совместимости, а не образцовый Vulkan.
//
// ЧЕГО ЗДЕСЬ НЕТ. Компиляции при каждом запуске можно было бы избежать кэшем
// SPIR-V на диске. Пока не сделано намеренно: сперва правильно, потом быстро —
// кэш, отдающий устаревший SPIR-V после правки шейдера, отлаживается неделю.
// ---------------------------------------------------------------------------
namespace sage::rhi {

class VulkanDevice;

// Результат компиляции пары «вершинный + фрагментный».
struct CompiledShader {
    std::vector<uint32_t> Vertex;
    std::vector<uint32_t> Fragment;

    // Где какая униформа лежит в общем блоке.
    struct UniformSlot {
        uint32_t Offset = 0;
        uint32_t Size = 0;
    };
    std::unordered_map<std::string, UniformSlot> Uniforms;
    uint32_t UniformBlockSize = 0;
    uint32_t UniformBinding = 0;

    // Сэмплеры: имя → номер привязки в наборе дескрипторов. Движок обращается к
    // ним через SetInt("uAlbedo", unit), то есть по НОМЕРУ ЮНИТА — сопоставление
    // «юнит → привязка» и живёт здесь.
    std::unordered_map<std::string, uint32_t> Samplers;

    bool Ok() const { return !Vertex.empty() && !Fragment.empty(); }
};

// Компилирует пару шейдеров. Ошибки компиляции пишет в лог с исходным текстом
// сообщения glslang — без него «шейдер не собрался» бесполезно.
CompiledShader CompileGlsl(const std::string& vertexSrc, const std::string& fragmentSrc,
                           const std::string& debugName);

class VulkanShaderProgram final : public ShaderProgram {
public:
    VulkanShaderProgram(VulkanDevice& device, const std::string& vertexSrc,
                        const std::string& fragmentSrc);
    ~VulkanShaderProgram() override;

    void Use() const override;
    void SetMat4(const std::string& name, const glm::mat4& v) const override;
    void SetMat4Array(const std::string& name, const glm::mat4* v, int count) const override;
    void SetIntArray(const std::string& name, const int* v, int count) const override;
    void SetFloatArray(const std::string& name, const float* v, int count) const override;
    void SetVec4(const std::string& name, const glm::vec4& v) const override;
    void SetVec3(const std::string& name, const glm::vec3& v) const override;
    void SetVec2(const std::string& name, const glm::vec2& v) const override;
    void SetFloat(const std::string& name, float v) const override;
    void SetInt(const std::string& name, int v) const override;

    bool Valid() const { return m_vertex != VK_NULL_HANDLE && m_fragment != VK_NULL_HANDLE; }
    VkShaderModule VertexModule() const { return m_vertex; }
    VkShaderModule FragmentModule() const { return m_fragment; }
    VkDescriptorSetLayout SetLayout() const { return m_setLayout; }
    VkPipelineLayout Layout() const { return m_pipelineLayout; }
    const CompiledShader& Compiled() const { return m_compiled; }

    // Теневая копия блока униформ: Set* пишут сюда, а в GPU-буфер она уезжает
    // одним куском в момент отрисовки. Писать в отображённую память на каждую
    // униформу нельзя — значения одного вызова отрисовки затирали бы значения
    // предыдущего, ещё не исполненного.
    const std::vector<unsigned char>& UniformShadow() const { return m_shadow; }
    // Номера юнитов, назначенные сэмплерам через SetInt.
    const std::unordered_map<uint32_t, int>& SamplerUnits() const { return m_samplerUnits; }

private:
    void Write(const std::string& name, const void* data, uint32_t bytes) const;

    VulkanDevice& m_device;
    CompiledShader m_compiled;
    VkShaderModule m_vertex = VK_NULL_HANDLE;
    VkShaderModule m_fragment = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;

    mutable std::vector<unsigned char> m_shadow;
    mutable std::unordered_map<uint32_t, int> m_samplerUnits; // привязка → юнит
};

} // namespace sage::rhi
