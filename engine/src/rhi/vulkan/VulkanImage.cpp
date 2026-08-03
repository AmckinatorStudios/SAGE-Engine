#include "VulkanCommon.h"

#include <vk_mem_alloc.h>

#include "VulkanImage.h"
#include "VulkanDevice.h"

#include <algorithm>

namespace sage::rhi {

uint32_t MipCount(int width, int height) {
    uint32_t levels = 1;
    int size = std::max(width, height);
    while (size > 1) { size /= 2; ++levels; }
    return levels;
}

VkFormat PickDepthFormat(VkPhysicalDevice gpu) {
    // Порядок не случаен: D32_SFLOAT точнее и не тащит трафарет, который движку
    // не нужен. Дальше — распространённые комбинированные форматы, потому что
    // на части устройств чистой глубины 32 бита нет.
    for (VkFormat f : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT,
                       VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D16_UNORM}) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(gpu, f, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) return f;
    }
    return VK_FORMAT_D16_UNORM; // обязателен по спецификации — сюда не дойти
}

VkSampleCountFlagBits PickSamples(VkPhysicalDevice gpu, int requested) {
    if (requested <= 1) return VK_SAMPLE_COUNT_1_BIT;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(gpu, &props);
    // Пересечение возможностей цвета и глубины: MSAA нужен обоим вложениям
    // сразу, и взять по максимуму одного значит получить отказ на другом.
    const VkSampleCountFlags allowed = props.limits.framebufferColorSampleCounts &
                                       props.limits.framebufferDepthSampleCounts;
    for (auto [bit, count] : {std::pair{VK_SAMPLE_COUNT_8_BIT, 8}, {VK_SAMPLE_COUNT_4_BIT, 4},
                              {VK_SAMPLE_COUNT_2_BIT, 2}}) {
        if (requested >= count && (allowed & bit)) return bit;
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

VulkanImage::~VulkanImage() { Destroy(); }

VulkanImage& VulkanImage::operator=(VulkanImage&& o) noexcept {
    if (this == &o) return *this;
    Destroy();
    m_desc = o.m_desc;
    m_image = o.m_image;
    m_view = o.m_view;
    m_faceViews = std::move(o.m_faceViews);
    m_allocation = o.m_allocation;
    m_allocator = o.m_allocator;
    m_device = o.m_device;
    m_layout = o.m_layout;
    o.m_image = VK_NULL_HANDLE;
    o.m_view = VK_NULL_HANDLE;
    o.m_faceViews.clear();
    o.m_allocation = nullptr;
    o.m_allocator = nullptr;
    o.m_device = VK_NULL_HANDLE;
    return *this;
}

void VulkanImage::Destroy() {
    if (m_device != VK_NULL_HANDLE) {
        for (VkImageView v : m_faceViews) {
            if (v != VK_NULL_HANDLE) vkDestroyImageView(m_device, v, nullptr);
        }
        if (m_view != VK_NULL_HANDLE) vkDestroyImageView(m_device, m_view, nullptr);
    }
    if (m_image != VK_NULL_HANDLE && m_allocator) {
        vmaDestroyImage(m_allocator, m_image, m_allocation);
    }
    m_faceViews.clear();
    m_view = VK_NULL_HANDLE;
    m_image = VK_NULL_HANDLE;
    m_allocation = nullptr;
    m_allocator = nullptr;
    m_device = VK_NULL_HANDLE;
    m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

VkImageAspectFlags VulkanImage::Aspect() const {
    switch (m_desc.Format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            // Трафарет в аспекте ОБЯЗАТЕЛЕН для переходов раскладки, но
            // представление для чтения шейдером обязано содержать только
            // глубину — см. Create ниже. Разница молчаливая: с трафаретом в
            // представлении драйвер откажет уже при создании набора дескрипторов.
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

bool VulkanImage::Create(VulkanDevice& device, const VulkanImageDesc& desc) {
    Destroy();
    if (!device.Ready() || desc.Width <= 0 || desc.Height <= 0) return false;
    m_desc = desc;
    m_device = device.Handle();

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = desc.Depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = desc.Format;
    info.extent = {(uint32_t)desc.Width, (uint32_t)desc.Height, (uint32_t)desc.Depth};
    info.mipLevels = std::max(1u, desc.Mips);
    info.arrayLayers = std::max(1u, desc.Layers);
    info.samples = desc.Samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = desc.Usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (desc.Cube) info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VmaAllocation allocation = nullptr;
    if (!vk::Check(vmaCreateImage(device.Allocator(), &info, &alloc, &m_image, &allocation, nullptr),
                   "vmaCreateImage")) {
        m_image = VK_NULL_HANDLE;
        return false;
    }
    m_allocation = allocation;
    m_allocator = device.Allocator();

    // Представление для ЧТЕНИЯ: только глубина, без трафарета (см. Aspect).
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = m_image;
    view.viewType = desc.Cube        ? VK_IMAGE_VIEW_TYPE_CUBE
                    : desc.Depth > 1 ? VK_IMAGE_VIEW_TYPE_3D
                                     : VK_IMAGE_VIEW_TYPE_2D;
    view.format = desc.Format;
    view.subresourceRange.aspectMask = Aspect() & ~VK_IMAGE_ASPECT_STENCIL_BIT;
    view.subresourceRange.levelCount = info.mipLevels;
    view.subresourceRange.layerCount = info.arrayLayers;
    if (!vk::Check(vkCreateImageView(m_device, &view, nullptr, &m_view), "vkCreateImageView")) {
        Destroy();
        return false;
    }
    return true;
}

VkImageView VulkanImage::FaceView(VulkanDevice& device, uint32_t layer, uint32_t mip) {
    const uint32_t index = layer * std::max(1u, m_desc.Mips) + mip;
    if (m_faceViews.size() <= index) m_faceViews.resize(index + 1, VK_NULL_HANDLE);
    if (m_faceViews[index] != VK_NULL_HANDLE) return m_faceViews[index];

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = m_image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = m_desc.Format;
    view.subresourceRange.aspectMask = Aspect() & ~VK_IMAGE_ASPECT_STENCIL_BIT;
    view.subresourceRange.baseMipLevel = mip;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.baseArrayLayer = layer;
    view.subresourceRange.layerCount = 1;
    vk::Check(vkCreateImageView(device.Handle(), &view, nullptr, &m_faceViews[index]),
              "vkCreateImageView (грань)");
    return m_faceViews[index];
}

namespace {

// Какие обращения и на каком этапе конвейера возможны в этой раскладке.
// Барьер без правильных масок не отказывает, а даёт гонку: данные читаются
// раньше, чем записаны, и на быстрой видеокарте это видно, а на медленной нет.
void AccessFor(VkImageLayout layout, VkAccessFlags& access, VkPipelineStageFlags& stage) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            access = 0; stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            access = VK_ACCESS_TRANSFER_WRITE_BIT; stage = VK_PIPELINE_STAGE_TRANSFER_BIT; break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            access = VK_ACCESS_TRANSFER_READ_BIT; stage = VK_PIPELINE_STAGE_TRANSFER_BIT; break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            access = VK_ACCESS_SHADER_READ_BIT;
            stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT; break;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            access = 0; stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT; break;
        default:
            access = 0; stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; break;
    }
}

} // namespace

void VulkanImage::TransitionTo(VkCommandBuffer cmd, VkImageLayout target) {
    if (!Valid() || m_layout == target) return;

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = m_layout;
    barrier.newLayout = target;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_image;
    barrier.subresourceRange.aspectMask = Aspect();
    barrier.subresourceRange.levelCount = std::max(1u, m_desc.Mips);
    barrier.subresourceRange.layerCount = std::max(1u, m_desc.Layers);

    VkPipelineStageFlags srcStage = 0, dstStage = 0;
    AccessFor(m_layout, barrier.srcAccessMask, srcStage);
    AccessFor(target, barrier.dstAccessMask, dstStage);

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    m_layout = target;
}

void VulkanImage::TransitionNow(VulkanDevice& device, VkImageLayout target) {
    if (!Valid() || m_layout == target) return;
    device.SubmitImmediate([&](VkCommandBuffer cmd) { TransitionTo(cmd, target); });
}

bool VulkanImage::Upload(VulkanDevice& device, const void* pixels, size_t bytes) {
    if (!Valid() || pixels == nullptr || bytes == 0) return false;

    VulkanBuffer staging;
    if (!staging.Create(device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUse::CpuToGpu)) {
        return false;
    }
    if (!staging.Upload(device, pixels, bytes)) return false;

    device.SubmitImmediate([&](VkCommandBuffer cmd) {
        TransitionTo(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = std::max(1u, m_desc.Layers);
        region.imageExtent = {(uint32_t)m_desc.Width, (uint32_t)m_desc.Height,
                              (uint32_t)m_desc.Depth};
        vkCmdCopyBufferToImage(cmd, staging.Handle(), m_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    });
    return true;
}

void VulkanImage::GenerateMips(VulkanDevice& device) {
    if (!Valid() || m_desc.Mips <= 1) return;

    // Blit с фильтрацией доступен не для всякого формата. Отсутствие — не повод
    // отказывать в текстуре: остаётся чёткий нулевой мип, а мыла на дальних
    // планах человек не заметит рядом с «текстура не загрузилась».
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(device.Gpu(), m_desc.Format, &props);
    if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        LOG_WARN("Vulkan") << "формат не поддерживает линейный blit — мипы не строятся";
        return;
    }

    device.SubmitImmediate([&](VkCommandBuffer cmd) {
        // Раскладка ведётся ПОУРОВНЕВО: источник каждого шага — предыдущий мип в
        // TRANSFER_SRC, приёмник — текущий в TRANSFER_DST. Общий m_layout здесь
        // не годится, поэтому барьеры ставятся вручную на диапазон из одного
        // уровня, а в конце всё изображение объявляется читаемым шейдером.
        auto barrier = [&](uint32_t mip, VkImageLayout from, VkImageLayout to,
                           VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                           VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = from;
            b.newLayout = to;
            b.srcAccessMask = srcAccess;
            b.dstAccessMask = dstAccess;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = m_image;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.baseMipLevel = mip;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.layerCount = std::max(1u, m_desc.Layers);
            vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
        };

        int w = m_desc.Width, h = m_desc.Height;
        for (uint32_t mip = 1; mip < m_desc.Mips; ++mip) {
            barrier(mip - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);

            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = mip - 1;
            blit.srcSubresource.layerCount = std::max(1u, m_desc.Layers);
            blit.srcOffsets[1] = {w, h, 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = mip;
            blit.dstSubresource.layerCount = std::max(1u, m_desc.Layers);
            blit.dstOffsets[1] = {std::max(w / 2, 1), std::max(h / 2, 1), 1};
            vkCmdBlitImage(cmd, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

            barrier(mip - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            w = std::max(w / 2, 1);
            h = std::max(h / 2, 1);
        }
        barrier(m_desc.Mips - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    });
    m_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

} // namespace sage::rhi
