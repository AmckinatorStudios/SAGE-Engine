#pragma once
#include <vector>

#include "VulkanDevice.h"
#include "VulkanImage.h"
#include "sage/rhi/Resources.h"

// ---------------------------------------------------------------------------
// RHI-ресурсы на Vulkan: текстуры и рендер-таргеты.
//
// ЧТО ЗДЕСЬ ОТЛИЧАЕТСЯ ОТ OpenGL ПО СУЩЕСТВУ, а не по синтаксису:
//
//   • Трёхканальных форматов практически нет. VK_FORMAT_R8G8B8_UNORM на
//     большинстве устройств не поддерживается вовсе, поэтому RGB расширяется до
//     RGBA при загрузке. В GL это делал драйвер молча.
//
//   • Параметры фильтрации — не свойство текстуры, а отдельный объект
//     (VkSampler). Кэш живёт в устройстве: одинаковых сочетаний на всю сцену
//     единицы, а сэмплер на текстуру упёрся бы в maxSamplerAllocationCount.
//
//   • У рендер-таргета есть VkRenderPass — описание того, что происходит с
//     вложениями в начале и конце прохода. В GL этого понятия нет: там просто
//     привязывают FBO. Проход создаётся один раз вместе с таргетом.
//
//   • Начало отсчёта по вертикали у Vulkan СВЕРХУ, а RHI договорился о нижнем
//     (см. GraphicsDevice.h). Пересчёт — обязанность бэкенда, и делается он в
//     одном месте: при выставлении viewport/scissor через отрицательную высоту
//     viewport'а (VK_KHR_maintenance1, ядро с 1.1).
// ---------------------------------------------------------------------------
namespace sage::rhi {

// Формат и число байт на пиксель для описания текстуры. Возвращает false, если
// такое сочетание каналов не поддерживается.
bool PixelFormatFor(int channels, bool floatPixels, VkFormat& format, int& bytesPerPixel,
                    bool& needsExpandToRgba);

class VulkanTexture2D final : public Texture2D {
public:
    VulkanTexture2D(VulkanDevice& device, const Texture2DDesc& desc, const void* pixels);
    ~VulkanTexture2D() override;

    void Bind(int unit) const override;
    TextureHandle Handle() const override { return m_handle; }
    // Для ImGui. У Vulkan «нативный хендл» текстуры — это VkDescriptorSet,
    // которого без бэкенда ImGui ещё нет. Отдаём значение нашего хендла: оно
    // ненулевое (этого требует набор соответствия) и осмысленно внутри бэкенда.
    uint64_t NativeHandle() const override { return m_handle.Value; }

private:
    VulkanDevice& m_device;
    VulkanImage m_image;
    TextureHandle m_handle{};
};

class VulkanTexture3D final : public Texture3D {
public:
    VulkanTexture3D(VulkanDevice& device, const Texture3DDesc& desc, const float* pixels);
    ~VulkanTexture3D() override;
    void Bind(int unit) const override;

private:
    VulkanDevice& m_device;
    VulkanImage m_image;
    TextureHandle m_handle{};
};

class VulkanTextureCube final : public TextureCube {
public:
    VulkanTextureCube(VulkanDevice& device, const CubeFacePixels faces[6]);
    ~VulkanTextureCube() override;
    void Bind(int unit) const override;

private:
    VulkanDevice& m_device;
    VulkanImage m_image;
    TextureHandle m_handle{};
};

class VulkanRenderTarget final : public RenderTarget {
public:
    VulkanRenderTarget(VulkanDevice& device, const RenderTargetDesc& desc);
    ~VulkanRenderTarget() override;

    void Bind() const override;
    void Resize(int width, int height) override;
    void Resolve() override;
    int Samples() const override { return m_samples; }
    int Width() const override { return m_width; }
    int Height() const override { return m_height; }
    TextureHandle ColorTextureHandle() const override { return m_colorHandle; }
    TextureHandle DepthTextureHandle() const override { return m_depthHandle; }
    uint64_t NativeColorHandle() const override { return m_colorHandle.Value; }

    // Два описания прохода на один таргет, и это не дублирование.
    //
    // В Vulkan судьба содержимого вложения решается ПРИ ВХОДЕ в проход:
    // loadOp = CLEAR затирает, LOAD сохраняет. В OpenGL такого выбора не было —
    // там привязывают буфер, а очищают отдельной командой и когда захотят.
    // Один проход с CLEAR стирал бы кадр каждый раз, когда таргет просто
    // сделали активным (а движок так делает: сначала рисует небо, потом
    // возвращается к тому же таргету за геометрией). Один проход с LOAD, наоборот,
    // требовал бы отдельной команды очистки даже на полноэкранную — а это
    // медленнее и лишает драйвер возможности не читать старое содержимое вовсе.
    //
    // Поэтому оба, а какой открыть — решает устройство по тому, что попросили
    // первым: очистку всего таргета или что-то ещё.
    VkRenderPass ClearPass() const { return m_passClear; }
    VkRenderPass LoadPass() const { return m_passLoad; }
    VkFramebuffer Framebuffer() const { return m_framebuffer; }
    bool HasColor() const { return m_kind != RenderTargetKind::DepthOnly; }
    bool HasDepth() const { return m_kind != RenderTargetKind::ColorHDR; }
    // Изображение, из которого читаются пиксели (после разрешения MSAA).
    VulkanImage& ColorImage() { return m_color; }
    // Проход закончился: привести отслеживаемые раскладки в соответствие с
    // finalLayout вложений (см. VulkanImage::SetTrackedLayout).
    void OnPassEnded();

private:
    bool CreateStorage();
    VkRenderPass BuildPass(bool clearOnLoad);
    void DestroyStorage();

    VulkanDevice& m_device;
    RenderTargetKind m_kind = RenderTargetKind::ColorHDRWithDepth;
    int m_width = 0, m_height = 0, m_samples = 1;

    VulkanImage m_color;        // однократный цвет (или приёмник разрешения MSAA)
    VulkanImage m_depth;
    VulkanImage m_colorMs;      // многосэмпловый цвет — рисуют СЮДА
    VulkanImage m_depthMs;
    // Состав вложений: собирается один раз в CreateStorage и переиспользуется
    // обоими описаниями прохода. Членом, а не глобалью: таргетов в кадре
    // несколько, и общее состояние они бы затирали друг у друга.
    std::vector<VkAttachmentDescription> m_attachments;
    VkAttachmentReference m_colorRef{}, m_depthRef{}, m_resolveRef{};
    bool m_hasColorRef = false, m_hasDepthRef = false, m_hasResolve = false;
    VkRenderPass m_passClear = VK_NULL_HANDLE;
    VkRenderPass m_passLoad = VK_NULL_HANDLE;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
    TextureHandle m_colorHandle{};
    TextureHandle m_depthHandle{};
};

} // namespace sage::rhi
