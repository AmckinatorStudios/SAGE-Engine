#include "VulkanResources.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace sage::rhi {

bool PixelFormatFor(int channels, bool floatPixels, VkFormat& format, int& bytesPerPixel,
                    bool& needsExpandToRgba) {
    needsExpandToRgba = false;
    if (floatPixels) {
        // 32-битный float, а НЕ 16F как в GL-бэкенде. Разница осознанная:
        // Vulkan не конвертирует данные при загрузке, а float→half пришлось бы
        // писать руками со всеми его краями (денормали, бесконечности). Точность
        // при этом только выше; плата — вдвое больше памяти на лайтмапы, которых
        // в сцене единицы.
        switch (channels) {
            case 1: format = VK_FORMAT_R32_SFLOAT; bytesPerPixel = 4; return true;
            case 2: format = VK_FORMAT_R32G32_SFLOAT; bytesPerPixel = 8; return true;
            case 3: format = VK_FORMAT_R32G32B32A32_SFLOAT; bytesPerPixel = 16;
                    needsExpandToRgba = true; return true;
            case 4: format = VK_FORMAT_R32G32B32A32_SFLOAT; bytesPerPixel = 16; return true;
            default: return false;
        }
    }
    switch (channels) {
        case 1: format = VK_FORMAT_R8_UNORM; bytesPerPixel = 1; return true;
        case 2: format = VK_FORMAT_R8G8_UNORM; bytesPerPixel = 2; return true;
        // Трёхканальный формат в Vulkan почти нигде не поддерживается — в GL это
        // разворачивал драйвер, здесь разворачиваем сами.
        case 3: format = VK_FORMAT_R8G8B8A8_UNORM; bytesPerPixel = 4;
                needsExpandToRgba = true; return true;
        case 4: format = VK_FORMAT_R8G8B8A8_UNORM; bytesPerPixel = 4; return true;
        default: return false;
    }
}

namespace {

// Расширение RGB→RGBA. Альфа = максимум (непрозрачно): текстура без альфы,
// ставшая полупрозрачной, — самый заметный способ ошибиться здесь.
template <typename T>
std::vector<T> ExpandToRgba(const T* src, size_t pixels, T opaque) {
    std::vector<T> out(pixels * 4);
    for (size_t i = 0; i < pixels; ++i) {
        out[i * 4 + 0] = src[i * 3 + 0];
        out[i * 4 + 1] = src[i * 3 + 1];
        out[i * 4 + 2] = src[i * 3 + 2];
        out[i * 4 + 3] = opaque;
    }
    return out;
}

VulkanDevice::SamplerKey SamplerKeyFor(Filter filter, Wrap wrap, bool mips, bool whiteBorder) {
    VulkanDevice::SamplerKey key;
    key.FilterMode = filter;
    key.WrapMode = wrap;
    key.Mips = mips;
    key.WhiteBorder = whiteBorder;
    return key;
}

} // namespace

// ============================================================================
//  Texture2D
// ============================================================================

VulkanTexture2D::VulkanTexture2D(VulkanDevice& device, const Texture2DDesc& desc,
                                 const void* pixels)
    : m_device(device) {
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    int bpp = 4;
    bool expand = false;
    if (!PixelFormatFor(desc.Channels, desc.FloatPixels, format, bpp, expand)) {
        LOG_ERROR("Vulkan") << "неподдерживаемое число каналов текстуры: " << desc.Channels;
        return;
    }

    const uint32_t mips = desc.GenerateMipmaps ? MipCount(desc.Width, desc.Height) : 1;

    VulkanImageDesc img;
    img.Width = desc.Width;
    img.Height = desc.Height;
    img.Format = format;
    img.Mips = mips;
    // TRANSFER_SRC нужен САМОЙ текстуре: мипы строятся blit'ом из её же
    // предыдущего уровня, то есть она одновременно источник и приёмник.
    img.Usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!m_image.Create(device, img)) return;

    if (pixels) {
        const size_t count = (size_t)desc.Width * desc.Height;
        if (expand && desc.FloatPixels) {
            const std::vector<float> rgba =
                ExpandToRgba(static_cast<const float*>(pixels), count, 1.0f);
            m_image.Upload(device, rgba.data(), rgba.size() * sizeof(float));
        } else if (expand) {
            const std::vector<unsigned char> rgba =
                ExpandToRgba(static_cast<const unsigned char*>(pixels), count,
                             (unsigned char)255);
            m_image.Upload(device, rgba.data(), rgba.size());
        } else {
            m_image.Upload(device, pixels, count * (size_t)bpp);
        }
    }

    if (mips > 1 && pixels) {
        m_image.GenerateMips(device);
    } else {
        m_image.TransitionNow(device, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    VkSampler sampler =
        device.SamplerFor(SamplerKeyFor(desc.FilterMode, desc.WrapMode, mips > 1, false));
    m_handle = device.RegisterTexture(m_image.View(), sampler);
}

VulkanTexture2D::~VulkanTexture2D() {
    if (m_handle.Valid()) m_device.UnregisterTexture(m_handle);
}

void VulkanTexture2D::Bind(int unit) const { m_device.BindTexture2D(unit, m_handle); }

// ============================================================================
//  Texture3D — объёмная float-текстура (GI-объём световых проб)
// ============================================================================

VulkanTexture3D::VulkanTexture3D(VulkanDevice& device, const Texture3DDesc& desc,
                                 const float* pixels)
    : m_device(device) {
    VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
    int bpp = 16;
    bool expand = false;
    if (!PixelFormatFor(desc.Channels, /*floatPixels=*/true, format, bpp, expand)) return;

    VulkanImageDesc img;
    img.Width = desc.Width;
    img.Height = desc.Height;
    img.Depth = desc.Depth;
    img.Format = format;
    img.Usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!m_image.Create(device, img)) return;

    if (pixels) {
        const size_t count = (size_t)desc.Width * desc.Height * desc.Depth;
        if (expand) {
            const std::vector<float> rgba = ExpandToRgba(pixels, count, 1.0f);
            m_image.Upload(device, rgba.data(), rgba.size() * sizeof(float));
        } else {
            m_image.Upload(device, pixels, count * (size_t)bpp);
        }
    }
    m_image.TransitionNow(device, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Трилинейно и с зажимом к краю: выборка между пробами интерполируется
    // аппаратно, за границей объёма берётся ближайшая проба, а не заворот с
    // противоположной стороны сцены.
    VkSampler sampler =
        device.SamplerFor(SamplerKeyFor(Filter::Bilinear, Wrap::ClampEdge, false, false));
    m_handle = device.RegisterTexture(m_image.View(), sampler);
}

VulkanTexture3D::~VulkanTexture3D() {
    if (m_handle.Valid()) m_device.UnregisterTexture(m_handle);
}

void VulkanTexture3D::Bind(int unit) const { m_device.BindTexture2D(unit, m_handle); }

// ============================================================================
//  TextureCube — скайбокс и окружение отражений
// ============================================================================

VulkanTextureCube::VulkanTextureCube(VulkanDevice& device, const CubeFacePixels faces[6])
    : m_device(device) {
    if (!faces || faces[0].Pixels == nullptr || faces[0].Width <= 0) return;

    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    int bpp = 4;
    bool expand = false;
    if (!PixelFormatFor(faces[0].Channels, false, format, bpp, expand)) return;

    VulkanImageDesc img;
    img.Width = faces[0].Width;
    img.Height = faces[0].Height;
    img.Format = format;
    img.Layers = 6;
    img.Cube = true;
    img.Usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!m_image.Create(device, img)) return;

    // Шесть граней грузятся ОДНИМ блоком: копирование покрывает все слои сразу
    // (layerCount = 6), поэтому данные должны лежать подряд в порядке
    // +X,-X,+Y,-Y,+Z,-Z — том же, в котором их отдаёт RHI.
    const size_t facePixels = (size_t)faces[0].Width * faces[0].Height;
    std::vector<unsigned char> all(facePixels * 6 * (size_t)bpp);
    for (int i = 0; i < 6; ++i) {
        unsigned char* dst = all.data() + facePixels * (size_t)bpp * i;
        if (!faces[i].Pixels) { std::memset(dst, 0, facePixels * (size_t)bpp); continue; }
        if (expand) {
            const std::vector<unsigned char> rgba =
                ExpandToRgba(faces[i].Pixels, facePixels, (unsigned char)255);
            std::memcpy(dst, rgba.data(), rgba.size());
        } else {
            std::memcpy(dst, faces[i].Pixels, facePixels * (size_t)bpp);
        }
    }
    m_image.Upload(device, all.data(), all.size());
    m_image.TransitionNow(device, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    VkSampler sampler =
        device.SamplerFor(SamplerKeyFor(Filter::Bilinear, Wrap::ClampEdge, false, false));
    m_handle = device.RegisterTexture(m_image.View(), sampler);
}

VulkanTextureCube::~VulkanTextureCube() {
    if (m_handle.Valid()) m_device.UnregisterTexture(m_handle);
}

void VulkanTextureCube::Bind(int unit) const { m_device.BindTexture2D(unit, m_handle); }

// ============================================================================
//  RenderTarget
// ============================================================================

VulkanRenderTarget::VulkanRenderTarget(VulkanDevice& device, const RenderTargetDesc& desc)
    : m_device(device), m_kind(desc.Kind), m_width(desc.Width), m_height(desc.Height) {
    // MSAA имеет смысл только там, где растеризуется геометрия: у буферов
    // пост-эффектов её нет, а глубину карты теней усреднять нельзя — среднее
    // двух глубин не является глубиной ничего.
    if (desc.Kind == RenderTargetKind::ColorHDRWithDepth && desc.Samples > 1) {
        const VkSampleCountFlagBits bits = PickSamples(device.Gpu(), desc.Samples);
        m_samples = (int)bits;
        if (m_samples != desc.Samples) {
            LOG_INFO("RHI") << "MSAA " << desc.Samples << "x недоступен, взято " << m_samples << "x";
        }
    }
    CreateStorage();
}

VulkanRenderTarget::~VulkanRenderTarget() { DestroyStorage(); }

void VulkanRenderTarget::DestroyStorage() {
    if (m_colorHandle.Valid()) { m_device.UnregisterTexture(m_colorHandle); m_colorHandle = {}; }
    if (m_depthHandle.Valid()) { m_device.UnregisterTexture(m_depthHandle); m_depthHandle = {}; }
    VkDevice dev = m_device.Handle();
    if (m_framebuffer != VK_NULL_HANDLE) { vkDestroyFramebuffer(dev, m_framebuffer, nullptr); m_framebuffer = VK_NULL_HANDLE; }
    if (m_passClear != VK_NULL_HANDLE) { vkDestroyRenderPass(dev, m_passClear, nullptr); m_passClear = VK_NULL_HANDLE; }
    if (m_passLoad != VK_NULL_HANDLE) { vkDestroyRenderPass(dev, m_passLoad, nullptr); m_passLoad = VK_NULL_HANDLE; }
    m_attachments.clear();
    m_color.Destroy();
    m_depth.Destroy();
    m_colorMs.Destroy();
    m_depthMs.Destroy();
}

// Описание прохода. Строится ДВАЖДЫ из одного набора вложений — с очисткой на
// входе и с сохранением содержимого (см. пояснение у ClearPass/LoadPass).
VkRenderPass VulkanRenderTarget::BuildPass(bool clearOnLoad) {
    std::vector<VkAttachmentDescription> attachments = m_attachments;
    for (VkAttachmentDescription& a : attachments) {
        a.loadOp = clearOnLoad ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        // initialLayout зависит от того же выбора. При очистке прежнее
        // содержимое не нужно, и UNDEFINED — самый дешёвый вариант: драйвер
        // вправе не читать старую память вовсе. При сохранении раскладка обязана
        // совпасть с той, в которой изображение оставил предыдущий проход
        // (finalLayout), иначе содержимое официально становится неопределённым.
        a.initialLayout = clearOnLoad ? VK_IMAGE_LAYOUT_UNDEFINED : a.finalLayout;
    }
    // Приёмник разрешения MSAA всегда пишется целиком — сохранять его нечего.
    if (m_hasResolve && !attachments.empty()) {
        attachments.back().loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments.back().initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = m_hasColorRef ? 1u : 0u;
    subpass.pColorAttachments = m_hasColorRef ? &m_colorRef : nullptr;
    subpass.pDepthStencilAttachment = m_hasDepthRef ? &m_depthRef : nullptr;
    subpass.pResolveAttachments = m_hasResolve ? &m_resolveRef : nullptr;

    // Зависимости на входе и выходе: содержимое таргета читают следующим
    // проходом как текстуру, и без явного барьера чтение может начаться до
    // конца записи. В OpenGL это гарантировал драйвер.
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    // Не только шейдер: содержимое ещё и КОПИРУЮТ (чтение пикселей), а
    // забытый этап переноса — это гонка, которую видно не на всякой видеокарте.
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = (uint32_t)attachments.size();
    pass.pAttachments = attachments.data();
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = 2;
    pass.pDependencies = deps;

    VkRenderPass result = VK_NULL_HANDLE;
    vk::Check(vkCreateRenderPass(m_device.Handle(), &pass, nullptr, &result), "vkCreateRenderPass");
    return result;
}

bool VulkanRenderTarget::CreateStorage() {
    if (!m_device.Ready() || m_width <= 0 || m_height <= 0) return false;

    const bool wantColor = m_kind == RenderTargetKind::ColorHDRWithDepth ||
                           m_kind == RenderTargetKind::ColorHDR;
    const bool wantDepth = m_kind == RenderTargetKind::ColorHDRWithDepth ||
                           m_kind == RenderTargetKind::DepthOnly;
    const bool ms = m_samples > 1;
    const VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT; // HDR, как в GL-бэкенде
    const VkFormat depthFormat = PickDepthFormat(m_device.Gpu());

    m_attachments.clear();
    std::vector<VkImageView> views;
    m_hasColorRef = m_hasDepthRef = m_hasResolve = false;

    if (wantColor) {
        VulkanImageDesc c;
        c.Width = m_width; c.Height = m_height; c.Format = colorFormat;
        c.Usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!m_color.Create(m_device, c)) return false;

        if (ms) {
            VulkanImageDesc cm = c;
            cm.Samples = (VkSampleCountFlagBits)m_samples;
            // Многосэмпловое вложение не сэмплируется шейдером напрямую — из
            // него только разрешают в обычное. Просить SAMPLED значит требовать
            // поддержки, которая нам не нужна и есть не везде.
            cm.Usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            if (!m_colorMs.Create(m_device, cm)) return false;
        }

        VkAttachmentDescription a{};
        a.format = colorFormat;
        a.samples = ms ? (VkSampleCountFlagBits)m_samples : VK_SAMPLE_COUNT_1_BIT;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.finalLayout = ms ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                           : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        m_colorRef.attachment = (uint32_t)m_attachments.size();
        m_colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        m_attachments.push_back(a);
        views.push_back(ms ? m_colorMs.View() : m_color.View());
        m_hasColorRef = true;
    }

    if (wantDepth) {
        VulkanImageDesc d;
        d.Width = m_width; d.Height = m_height; d.Format = depthFormat;
        d.Samples = ms ? (VkSampleCountFlagBits)m_samples : VK_SAMPLE_COUNT_1_BIT;
        d.Usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VulkanImage& target = ms ? m_depthMs : m_depth;
        if (!target.Create(m_device, d)) return false;

        // Карта теней и SSAO читают глубину как текстуру, поэтому её надо
        // ХРАНИТЬ после прохода (storeOp = STORE). У обычного depth-буфера это
        // выглядело бы избыточным, но у нас другого и нет.
        VkAttachmentDescription a{};
        a.format = depthFormat;
        a.samples = d.Samples;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        m_depthRef.attachment = (uint32_t)m_attachments.size();
        m_depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        m_attachments.push_back(a);
        views.push_back(target.View());
        m_hasDepthRef = true;
    }

    if (ms && m_hasColorRef) {
        // Разрешение MSAA описывается ПРЯМО В ПРОХОДЕ: драйвер делает его на
        // выходе из подпрохода, без отдельной команды и без лишнего чтения
        // памяти. В GL для этого нужен был отдельный blit между двумя FBO.
        VkAttachmentDescription a{};
        a.format = colorFormat;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        m_resolveRef.attachment = (uint32_t)m_attachments.size();
        m_resolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        m_attachments.push_back(a);
        views.push_back(m_color.View());
        m_hasResolve = true;
    }

    m_passClear = BuildPass(/*clearOnLoad=*/true);
    m_passLoad = BuildPass(/*clearOnLoad=*/false);
    if (m_passClear == VK_NULL_HANDLE || m_passLoad == VK_NULL_HANDLE) return false;

    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    // Кадровый буфер совместим с ЛЮБЫМ проходом такого же состава вложений —
    // спецификация требует совпадения форматов и числа сэмплов, а не самого
    // объекта прохода. Поэтому один кадровый буфер на оба описания.
    fb.renderPass = m_passClear;
    fb.attachmentCount = (uint32_t)views.size();
    fb.pAttachments = views.data();
    fb.width = (uint32_t)m_width;
    fb.height = (uint32_t)m_height;
    fb.layers = 1;
    if (!vk::Check(vkCreateFramebuffer(m_device.Handle(), &fb, nullptr, &m_framebuffer),
                   "vkCreateFramebuffer")) {
        return false;
    }

    // Хендлы вложений — то, чем движок сэмплирует результат прохода. У карты
    // теней сэмплер с белой рамкой: за пределами покрытия «освещено».
    if (m_hasColorRef) {
        VkSampler s = m_device.SamplerFor(
            VulkanDevice::SamplerKey{Filter::Bilinear, Wrap::ClampEdge, false, false});
        m_colorHandle = m_device.RegisterTexture(m_color.View(), s);
    }
    if (m_hasDepthRef) {
        const bool shadow = m_kind == RenderTargetKind::DepthOnly;
        VkSampler s = m_device.SamplerFor(VulkanDevice::SamplerKey{
            shadow ? Filter::Bilinear : Filter::Nearest, Wrap::ClampEdge, false, shadow});
        m_depthHandle = m_device.RegisterTexture((ms ? m_depthMs : m_depth).View(), s);
    }
    return true;
}

void VulkanRenderTarget::Resize(int width, int height) {
    if (width == m_width && height == m_height) return; // no-op по контракту RHI
    if (width <= 0 || height <= 0) return;
    vkDeviceWaitIdle(m_device.Handle());
    DestroyStorage();
    m_width = width;
    m_height = height;
    CreateStorage();
}

void VulkanRenderTarget::Resolve() {
    // Пустышка НЕ по недосмотру: разрешение MSAA описано прямо в проходе
    // (pResolveAttachments), драйвер выполняет его на выходе из подпрохода.
    // Метод остаётся, потому что он часть контракта RHI и вызывается всегда.
}

void VulkanRenderTarget::OnPassEnded() {
    // Значения обязаны совпадать с finalLayout из CreateStorage — иначе
    // рассинхрон, и следующий барьер будет неправильным.
    if (m_hasColorRef) {
        m_color.SetTrackedLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (m_samples > 1) m_colorMs.SetTrackedLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    if (m_hasDepthRef) {
        VulkanImage& depth = m_samples > 1 ? m_depthMs : m_depth;
        depth.SetTrackedLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }
}

void VulkanRenderTarget::Bind() const {
    // Проход открывает запись команд — этим занимается устройство, которое
    // ведёт текущий командный буфер. Здесь только сообщаем ему, куда рисовать.
    m_device.BindRenderTarget(this);
}

} // namespace sage::rhi
