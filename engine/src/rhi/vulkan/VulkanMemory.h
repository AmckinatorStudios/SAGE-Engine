#pragma once
#include <utility>

#include "VulkanCommon.h"

// ---------------------------------------------------------------------------
// Видеопамять и буферы.
//
// ПОЧЕМУ VMA, А НЕ vkAllocateMemory НАПРЯМУЮ. Число выделений в Vulkan
// ограничено (maxMemoryAllocationCount, на десктопе обычно 4096), и блок под
// каждую текстуру заканчивается на средней сцене — а заканчивается он не
// сообщением, а отказом создать ресурс посреди загрузки уровня. Нужен
// субаллокатор: пулы по типам памяти, выравнивание, раздельное размещение
// линейных и нелинейных ресурсов (bufferImageGranularity). Всё это — код,
// который ошибается молча и проявляется на чужой видеокарте через полгода.
// Взят VMA от AMD: ровно эта задача, написанная теми, кто пишет драйвер.
//
// ЧТО ЗДЕСЬ СВОЁ. Тонкая обёртка: буфер + его выделение одним объектом с
// понятным временем жизни, и загрузка данных через промежуточный (staging)
// буфер там, где память устройства недоступна процессору. Это и есть то, о чём
// OpenGL молчал: glBufferData просто «работал», а под ним драйвер сам решал,
// где лежат данные и когда их копировать.
// ---------------------------------------------------------------------------
struct VmaAllocator_T;
struct VmaAllocation_T;

namespace sage::rhi {

class VulkanDevice;

// Создание/уничтожение распределителя. Объявлены здесь, а определены в
// VulkanMemory.cpp — единственном файле, куда включается реализация VMA.
VmaAllocator_T* CreateAllocator(VkInstance instance, VkPhysicalDevice gpu, VkDevice device);
void DestroyAllocator(VmaAllocator_T* allocator);

// Где живёт память ресурса. Разница не в скорости на проценты, а в том,
// возможна ли запись с процессора вообще.
enum class MemoryUse {
    // Только для GPU: самая быстрая память, процессор в неё не пишет. Данные
    // попадают туда копированием из staging-буфера.
    GpuOnly,
    // Пишется процессором каждый кадр (UI, текст, инстансные потоки). Лежит в
    // видимой процессору памяти — копирование было бы дороже, чем чуть более
    // медленное чтение GPU.
    CpuToGpu,
    // Читается процессором (чтение пикселей для скриншота).
    GpuToCpu,
};

// Буфер вместе со своим выделением. Некопируемый: два владельца одного
// VkBuffer означают двойное освобождение, и ловится оно уже на выходе.
class VulkanBuffer {
public:
    VulkanBuffer() = default;
    ~VulkanBuffer();
    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;
    VulkanBuffer(VulkanBuffer&& other) noexcept { *this = std::move(other); }
    VulkanBuffer& operator=(VulkanBuffer&& other) noexcept;

    // Создаёт (или пересоздаёт) буфер нужного размера. Пересоздание при том же
    // размере и назначении — no-op: перезаливка данных каждый кадр не должна
    // означать перевыделение памяти.
    bool Create(VulkanDevice& device, VkDeviceSize bytes, VkBufferUsageFlags usage, MemoryUse use);
    void Destroy();

    // Записывает данные. Для CpuToGpu — прямая запись в отображённую память,
    // для GpuOnly — через промежуточный буфер и копирование в очереди.
    bool Upload(VulkanDevice& device, const void* data, VkDeviceSize bytes, VkDeviceSize offset = 0);

    VkBuffer Handle() const { return m_buffer; }
    VkDeviceSize Size() const { return m_size; }
    bool Valid() const { return m_buffer != VK_NULL_HANDLE; }
    // Отображённый указатель (только для памяти, видимой процессору).
    void* Mapped() const { return m_mapped; }

private:
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VmaAllocation_T* m_allocation = nullptr;
    VmaAllocator_T* m_allocator = nullptr;
    VkDeviceSize m_size = 0;
    VkBufferUsageFlags m_usage = 0;
    MemoryUse m_use = MemoryUse::GpuOnly;
    void* m_mapped = nullptr;
};

} // namespace sage::rhi
