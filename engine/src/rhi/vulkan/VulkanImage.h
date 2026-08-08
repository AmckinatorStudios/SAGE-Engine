#pragma once
#include <utility>

#include "VulkanCommon.h"
#include "VulkanMemory.h"

// ---------------------------------------------------------------------------
// Изображение (VkImage) со своей памятью, представлением и ТЕКУЩЕЙ РАСКЛАДКОЙ.
//
// ЧТО ТАКОЕ РАСКЛАДКА И ПОЧЕМУ ЕЁ ПРИХОДИТСЯ ВЕСТИ. В OpenGL текстура — просто
// текстура: драйвер сам знал, что с ней сейчас делают, и переставлял внутреннее
// представление молча. Vulkan это сняли: у каждого изображения есть layout, и
// чтение шейдером, запись как во вложение и копирование требуют РАЗНЫХ
// раскладок. Ошибка здесь не даёт ни отказа, ни сообщения без слоя валидации —
// она даёт мусор на экране или неопределённое поведение.
//
// Отсюда — раскладка хранится В САМОМ ОБЪЕКТЕ, и переход делается методом,
// который знает, откуда переходит. Требовать этого от вызывающего значило бы
// раздать по бэкенду двадцать мест, где надо не забыть, — а забудут в одном.
// ---------------------------------------------------------------------------
namespace sage::rhi {

class VulkanDevice;

struct VulkanImageDesc {
    int Width = 0;
    int Height = 0;
    int Depth = 1;                                   // > 1 — объёмная текстура
    VkFormat Format = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t Mips = 1;
    uint32_t Layers = 1;                             // 6 — кубическая
    VkSampleCountFlagBits Samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags Usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    bool Cube = false;
};

class VulkanImage {
public:
    VulkanImage() = default;
    ~VulkanImage();
    VulkanImage(const VulkanImage&) = delete;
    VulkanImage& operator=(const VulkanImage&) = delete;
    VulkanImage(VulkanImage&& o) noexcept { *this = std::move(o); }
    VulkanImage& operator=(VulkanImage&& o) noexcept;

    bool Create(VulkanDevice& device, const VulkanImageDesc& desc);
    void Destroy();

    // Заливает пиксели нулевого мипа (все слои подряд для кубической).
    // bytesPerPixel — размер пикселя В ТОМ ЖЕ формате, что у изображения:
    // расширение RGB→RGBA делает вызывающий, потому что трёхканальные форматы
    // на многих устройствах не поддерживаются вовсе.
    bool Upload(VulkanDevice& device, const void* pixels, size_t bytes);

    // Достраивает мипы последовательными blit'ами. Требует у формата
    // возможности линейной фильтрации при blit — иначе честно ничего не делает
    // и оставляет один уровень (лучше чёткая текстура без мипов, чем отказ).
    void GenerateMips(VulkanDevice& device);

    // Перевод в нужную раскладку. Знает текущую, поэтому безопасен к повторам.
    void TransitionTo(VkCommandBuffer cmd, VkImageLayout target);
    void TransitionNow(VulkanDevice& device, VkImageLayout target);

    VkImage Handle() const { return m_image; }
    VkImageView View() const { return m_view; }
    // Представление одной грани/мипа — нужно кубическим рендер-таргетам, где
    // рисуют в конкретную грань.
    VkImageView FaceView(VulkanDevice& device, uint32_t layer, uint32_t mip);
    VkFormat Format() const { return m_desc.Format; }
    VkImageLayout Layout() const { return m_layout; }
    // Сообщить объекту, что раскладку сменил КТО-ТО ДРУГОЙ — на практике
    // проход, у которого finalLayout прописан в описании вложения.
    //
    // Без этого следующий переход считался бы от UNDEFINED, а UNDEFINED как
    // исходная раскладка официально разрешает драйверу ВЫБРОСИТЬ содержимое.
    // Это не гипотетическая придирка: ровно так чтение пикселей вернуло бы
    // чёрный кадр, причём не на всяком драйвере — на том, который решит
    // воспользоваться разрешением.
    void SetTrackedLayout(VkImageLayout layout) { m_layout = layout; }
    int Width() const { return m_desc.Width; }
    int Height() const { return m_desc.Height; }
    uint32_t Mips() const { return m_desc.Mips; }
    VkSampleCountFlagBits Samples() const { return m_desc.Samples; }
    bool Valid() const { return m_image != VK_NULL_HANDLE; }

private:
    VkImageAspectFlags Aspect() const;

    VulkanImageDesc m_desc;
    VkImage m_image = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    std::vector<VkImageView> m_faceViews;   // ленивые представления граней/мипов
    VmaAllocation_T* m_allocation = nullptr;
    VmaAllocator_T* m_allocator = nullptr;
    VkDevice m_device = VK_NULL_HANDLE;
    VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// Сколько мип-уровней у изображения такого размера (полная цепочка до 1x1).
uint32_t MipCount(int width, int height);

// Формат глубины, который устройство действительно поддерживает как вложение.
VkFormat PickDepthFormat(VkPhysicalDevice gpu);

// Число сэмплов, урезанное до того, что устройство умеет для цвета и глубины
// одновременно. MSAA, которого нет, обязан молча превратиться в 1, а не в отказ
// создать таргет: настройка качества не должна ронять игру.
VkSampleCountFlagBits PickSamples(VkPhysicalDevice gpu, int requested);

} // namespace sage::rhi
