// Реализация распределителя и буферов.
//
// VMA_IMPLEMENTATION живёт ЗДЕСЬ и больше нигде: заголовок VMA содержит и
// объявления, и определения, и второй файл с этим макросом даст дубли символов
// на линковке.
//
// ПОРЯДОК ВКЛЮЧЕНИЯ ЗДЕСЬ ЗНАЧИМ: volk обязан идти первым. Он объявляет
// VK_NO_PROTOTYPES и только потом включает vulkan.h; заголовок VMA включает
// vulkan.h сам, и попав первым, притащил бы обычные прототипы — после чего volk
// отказывается собираться («To use volk, you need to define VK_NO_PROTOTYPES»).
#include "VulkanCommon.h"

// VMA_STATIC_VULKAN_FUNCTIONS=0 обязателен по той же причине: функции Vulkan у
// нас — указатели, заполняемые в рантайме, а не внешние символы. Без этого VMA
// вызвала бы vkAllocateMemory напрямую и потребовала бы линковки с
// vulkan-1.lib, то есть тот самый SDK, от которого мы уходим. Таблицу
// указателей передаём ей явно при создании распределителя.
//
// А VMA_DYNAMIC_VULKAN_FUNCTIONS=1 — обязательное продолжение. При нуле VMA
// ждёт, что в VmaVulkanFunctions заполнены ВСЕ полтора десятка указателей;
// заполнив только два геттера, получаешь вызов по нулевому адресу прямо в
// конструкторе распределителя. С единицей VMA берёт эти два и догружает
// остальное сама — ровно то, что нужно при загрузке через volk.
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include "VulkanMemory.h"
#include "VulkanDevice.h"

#include <cstring>

namespace sage::rhi {

namespace {

VmaMemoryUsage ToVma(MemoryUse use) {
    switch (use) {
        case MemoryUse::GpuOnly: return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        case MemoryUse::CpuToGpu: return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        case MemoryUse::GpuToCpu: return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    }
    return VMA_MEMORY_USAGE_AUTO;
}

VmaAllocationCreateFlags ToVmaFlags(MemoryUse use) {
    switch (use) {
        case MemoryUse::GpuOnly:
            return 0;
        case MemoryUse::CpuToGpu:
            // Отображается один раз на всё время жизни: буфер, который пишут
            // каждый кадр, не должен отображаться и снимать отображение на
            // каждую запись — это обращение к драйверу .
            return VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                   VMA_ALLOCATION_CREATE_MAPPED_BIT;
        case MemoryUse::GpuToCpu:
            return VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                   VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    return 0;
}

} // namespace

VmaAllocator_T* CreateAllocator(VkInstance instance, VkPhysicalDevice gpu, VkDevice device) {
    // Таблица функций из volk — см. пояснение к VMA_STATIC_VULKAN_FUNCTIONS.
    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info{};
    info.physicalDevice = gpu;
    info.device = device;
    info.instance = instance;
    info.vulkanApiVersion = VK_API_VERSION_1_1;
    info.pVulkanFunctions = &fns;

    VmaAllocator allocator = nullptr;
    if (!vk::Check(vmaCreateAllocator(&info, &allocator), "vmaCreateAllocator")) return nullptr;
    return allocator;
}

void DestroyAllocator(VmaAllocator_T* allocator) {
    if (allocator) vmaDestroyAllocator(allocator);
}

VulkanBuffer::~VulkanBuffer() { Destroy(); }

VulkanBuffer& VulkanBuffer::operator=(VulkanBuffer&& other) noexcept {
    if (this == &other) return *this;
    Destroy();
    m_buffer = other.m_buffer;
    m_allocation = other.m_allocation;
    m_allocator = other.m_allocator;
    m_size = other.m_size;
    m_usage = other.m_usage;
    m_use = other.m_use;
    m_mapped = other.m_mapped;
    other.m_buffer = VK_NULL_HANDLE;
    other.m_allocation = nullptr;
    other.m_allocator = nullptr;
    other.m_size = 0;
    other.m_mapped = nullptr;
    return *this;
}

void VulkanBuffer::Destroy() {
    if (m_buffer != VK_NULL_HANDLE && m_allocator) {
        vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
    }
    m_buffer = VK_NULL_HANDLE;
    m_allocation = nullptr;
    m_allocator = nullptr;
    m_size = 0;
    m_mapped = nullptr;
}

bool VulkanBuffer::Create(VulkanDevice& device, VkDeviceSize bytes, VkBufferUsageFlags usage,
                          MemoryUse use) {
    if (bytes == 0 || !device.Ready()) return false;
    // Тот же буфер того же назначения — ничего не делаем. Перезаливка каждый
    // кадр (UI, инстансы) обязана быть дешёвой, а перевыделение памяти дешёвым
    // не бывает.
    if (m_buffer != VK_NULL_HANDLE && m_size >= bytes && m_usage == usage && m_use == use) {
        return true;
    }
    Destroy();

    // GpuOnly всегда получает право быть приёмником копирования: данные в него
    // попадают только так, и забыть этот флаг значит получить отказ валидации
    // при первой же загрузке.
    VkBufferUsageFlags flags = usage;
    if (use == MemoryUse::GpuOnly) flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = flags;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = ToVma(use);
    alloc.flags = ToVmaFlags(use);

    VmaAllocationInfo allocInfo{};
    VmaAllocation allocation = nullptr;
    if (!vk::Check(vmaCreateBuffer(device.Allocator(), &info, &alloc, &m_buffer, &allocation,
                                   &allocInfo),
                   "vmaCreateBuffer")) {
        m_buffer = VK_NULL_HANDLE;
        return false;
    }
    m_allocation = allocation;
    m_allocator = device.Allocator();
    m_size = bytes;
    m_usage = usage;
    m_use = use;
    m_mapped = allocInfo.pMappedData;
    return true;
}

bool VulkanBuffer::Upload(VulkanDevice& device, const void* data, VkDeviceSize bytes,
                          VkDeviceSize offset) {
    if (!Valid() || data == nullptr || bytes == 0) return false;
    if (offset + bytes > m_size) {
        LOG_ERROR("Vulkan") << "запись за границу буфера: " << (offset + bytes) << " > " << m_size;
        return false;
    }

    if (m_mapped) {
        std::memcpy(static_cast<char*>(m_mapped) + offset, data, (size_t)bytes);
        // Память может быть некогерентной — тогда запись процессора не видна
        // GPU, пока её не сбросили. vmaFlushAllocation сам проверяет, нужно ли
        // это: на когерентной памяти вызов бесплатен.
        vmaFlushAllocation(m_allocator, m_allocation, offset, bytes);
        return true;
    }

    // Память устройства процессору не видна — данные едут через промежуточный
    // буфер. Он живёт ровно до конца копирования: держать staging на каждый
    // ресурс значило бы удваивать память на всю сцену.
    VulkanBuffer staging;
    if (!staging.Create(device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUse::CpuToGpu)) {
        return false;
    }
    if (!staging.Upload(device, data, bytes)) return false;

    device.SubmitImmediate([&](VkCommandBuffer cmd) {
        VkBufferCopy region{};
        region.dstOffset = offset;
        region.size = bytes;
        vkCmdCopyBuffer(cmd, staging.Handle(), m_buffer, 1, &region);
    });
    return true;
}

} // namespace sage::rhi
