#pragma once
#include <cstddef>
#include <vector>

#include "VulkanCommon.h"
#include "VulkanMemory.h"
#include "sage/rhi/Resources.h"

// ---------------------------------------------------------------------------
// Геометрия и конвейеры.
//
// ГЛАВНОЕ РАСХОЖДЕНИЕ С OpenGL. Там состояние конвейера — глобальные
// переключатели: включил смешивание, поменял тест глубины, нарисовал. В Vulkan
// всё это ЗАПЕКАЕТСЯ в объект VkPipeline заранее, вместе с шейдером, форматом
// вершин и описанием прохода. Поменять один флаг «на лету» нельзя.
//
// Движок написан в стиле OpenGL и переписываться не будет, поэтому бэкенд
// собирает состояние в ключ и ИЩЕТ по нему готовый конвейер, создавая новый
// только для невиданного сочетания. Это не эмуляция ради галочки: реальных
// сочетаний в кадре десятки (непрозрачное, прозрачное, тени, каркас, небо),
// кэш прогревается за первые кадры, и дальше вызов отрисовки стоит один поиск
// в хеш-таблице.
//
// ЧТО ВХОДИТ В КЛЮЧ и почему именно это: шейдер (модули), состояние
// конвейера, ФОРМАТ ВЕРШИН и ПРОХОД. Последние два неочевидны, но обязательны:
// конвейер привязан к раскладке вершинных атрибутов и к описанию вложений, и
// использовать его с другими — ошибка, которую валидация ловит, а драйвер без
// неё может и не заметить.
// ---------------------------------------------------------------------------
namespace sage::rhi {

class VulkanDevice;
class VulkanShaderProgram;

class VulkanGeometry final : public Geometry {
public:
    VulkanGeometry(VulkanDevice& device, const VertexLayout& layout);
    ~VulkanGeometry() override = default;

    void SetVertexData(const void* data, size_t bytes, bool dynamic) override;
    void SetIndexData(const unsigned int* indices, size_t count, bool dynamic) override;
    void SetInstanceData(const void* data, size_t bytes) override;

    void DrawIndexed(size_t indexCount) const override;
    void DrawIndexedRange(size_t firstIndex, size_t indexCount) const override;
    void DrawArrays(size_t vertexCount) const override;
    void DrawInstanced(size_t vertexCount, size_t instanceCount) const override;
    void DrawIndexedInstanced(size_t indexCount, size_t instanceCount) const override;
    void DrawLines(size_t vertexCount) const override;

    const VertexLayout& Layout() const { return m_layout; }
    VkBuffer VertexBuffer() const { return m_vertex.Handle(); }
    VkBuffer IndexBuffer() const { return m_index.Handle(); }
    VkBuffer InstanceBuffer() const { return m_instance.Handle(); }
    bool HasIndices() const { return m_index.Valid(); }
    bool HasInstances() const { return m_instance.Valid(); }
    // Хеш формата вершин — часть ключа конвейера. Считается один раз при
    // создании: формат геометрии за её жизнь не меняется.
    size_t LayoutHash() const { return m_layoutHash; }

private:
    VulkanDevice& m_device;
    VertexLayout m_layout;
    size_t m_layoutHash = 0;
    VulkanBuffer m_vertex;
    VulkanBuffer m_index;
    VulkanBuffer m_instance;
};

// Формат атрибута вершины по описанию RHI.
VkFormat AttributeFormat(AttribType type, int components);

} // namespace sage::rhi
