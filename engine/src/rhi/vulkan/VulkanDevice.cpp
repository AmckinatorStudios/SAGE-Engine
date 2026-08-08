#include "VulkanDevice.h"
#include "VulkanMemory.h"
#include "VulkanResources.h"
#include "VulkanPipeline.h"
#include "VulkanShader.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace sage::rhi {

namespace {

// Валидационные слои включаются, только если они РЕАЛЬНО установлены. Просить
// их безусловно нельзя: на машине игрока их нет, и vkCreateInstance откажет
// целиком — то есть отладочное удобство стоило бы запуска у всех остальных.
bool HasLayer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties& l : layers) {
        if (std::strcmp(l.layerName, name) == 0) return true;
    }
    return false;
}

bool HasInstanceExtension(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
    for (const VkExtensionProperties& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void*) {
    // Слой валидации — главная причина, по которой Vulkan вообще стоит брать:
    // он ловит ошибки использования API, о которых OpenGL молчит и рисует
    // мусор. Пускаем его в НАШ лог, а не в stderr: иначе в редакторе его никто
    // не увидит — ровно та ошибка, из-за которой пропадали сообщения ImGui.
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        LOG_ERROR("Vulkan") << data->pMessage;
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOG_WARN("Vulkan") << data->pMessage;
    }
    return VK_FALSE; // VK_FALSE — «не прерывать вызов», сообщение уже записано
}

std::string DeviceTypeName(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "дискретная";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "встроенная";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "виртуальная";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "программная";
        default: return "неизвестная";
    }
}

// Насколько устройство нам подходит. Дискретная видеокарта лучше встроенной,
// встроенная — лучше программной. Программную (lavapipe) НЕ исключаем: на ней
// гоняются headless-тесты, и запретить её значило бы остаться без единственного
// способа проверить бэкенд в CI.
int ScoreGpu(const VkPhysicalDeviceProperties& props) {
    switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 400;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 300;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 200;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return 100;
        default: return 50;
    }
}

// Индекс семейства очередей с поддержкой графики. UINT32_MAX — нет такого.
uint32_t FindGraphicsFamily(VkPhysicalDevice gpu) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());
    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) return i;
    }
    return UINT32_MAX;
}

// volkInitialize зовётся один раз на процесс: он открывает системный загрузчик
// и заполняет глобальные точки входа. Повторный вызов безвреден, но проверка
// доступности вызывается и до Init, и из неё — держим один флаг.
bool EnsureVolk() {
    static const bool ok = (volkInitialize() == VK_SUCCESS);
    return ok;
}

} // namespace

VulkanDevice::VulkanDevice() = default;

VulkanDevice::~VulkanDevice() {
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
        // Распределитель — ПОСЛЕ всех ресурсов и ДО устройства: он держит
        // выделения, а они принадлежат устройству.
        for (auto& [key, pipeline] : m_pipelines) vkDestroyPipeline(m_device, pipeline, nullptr);
        m_pipelines.clear();
        if (m_descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        }
        m_uniformRing.Destroy();
        if (m_fence != VK_NULL_HANDLE) vkDestroyFence(m_device, m_fence, nullptr);
        for (auto& [key, sampler] : m_samplers) vkDestroySampler(m_device, sampler, nullptr);
        m_samplers.clear();
        DestroyAllocator(m_allocator);
        m_allocator = nullptr;
        if (m_pool != VK_NULL_HANDLE) vkDestroyCommandPool(m_device, m_pool, nullptr);
        vkDestroyDevice(m_device, nullptr);
    }
    if (m_debug != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT) {
        vkDestroyDebugUtilsMessengerEXT(m_instance, m_debug, nullptr);
    }
    if (m_instance != VK_NULL_HANDLE) vkDestroyInstance(m_instance, nullptr);
}

bool VulkanDevice::Available() {
    // Проверка ДО всякой попытки рисовать: Application спрашивает её, чтобы
    // молча откатиться на OpenGL, а не выдать игроку окно с ошибкой.
    if (!EnsureVolk()) return false;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "SAGE probe";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;

    VkInstance probe = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, nullptr, &probe) != VK_SUCCESS) return false;
    volkLoadInstanceOnly(probe);

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(probe, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    if (count) vkEnumeratePhysicalDevices(probe, &count, gpus.data());

    // Устройство без графической очереди бесполезно: такое бывает у
    // вычислительных ускорителей, и «Vulkan есть» про них — неправда.
    bool usable = false;
    for (VkPhysicalDevice gpu : gpus) {
        if (FindGraphicsFamily(gpu) != UINT32_MAX) { usable = true; break; }
    }
    vkDestroyInstance(probe, nullptr);
    return usable;
}

bool VulkanDevice::CreateInstance() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "SAGE Engine";
    app.pEngineName = "SAGE";
    app.apiVersion = VK_API_VERSION_1_1;

    std::vector<const char*> layers;
    std::vector<const char*> extensions;

    // Отладочные слои — по явной просьбе (SAGE_VK_VALIDATION=1) и только если
    // установлены. По умолчанию выключены: они стоят заметной доли кадра, и
    // платить её в игре у игрока не за что.
    const bool wantValidation = std::getenv("SAGE_VK_VALIDATION") != nullptr;
    const bool haveDebugUtils = HasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (wantValidation && HasLayer("VK_LAYER_KHRONOS_validation")) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        if (haveDebugUtils) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    } else if (wantValidation) {
        LOG_WARN("Vulkan") << "SAGE_VK_VALIDATION задан, но слой "
                              "VK_LAYER_KHRONOS_validation не установлен — пропускаю";
    }

    // Расширения под поверхность окна запрашиваются ТОЛЬКО если они есть.
    // Headless-прогон (тесты, CI) обходится без них вовсе, и требовать их
    // безусловно значило бы не запускаться там, где как раз и проверяют.
    for (const char* name : {VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(_WIN32)
                             "VK_KHR_win32_surface",
#elif defined(__APPLE__)
                             "VK_EXT_metal_surface",
#else
                             "VK_KHR_xlib_surface", "VK_KHR_xcb_surface",
#endif
                            }) {
        if (HasInstanceExtension(name)) extensions.push_back(name);
    }

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledLayerCount = (uint32_t)layers.size();
    info.ppEnabledLayerNames = layers.data();
    info.enabledExtensionCount = (uint32_t)extensions.size();
    info.ppEnabledExtensionNames = extensions.data();

    if (!vk::Check(vkCreateInstance(&info, nullptr, &m_instance), "vkCreateInstance")) return false;
    volkLoadInstanceOnly(m_instance);

    if (!layers.empty() && haveDebugUtils && vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dbg.pfnUserCallback = DebugCallback;
        vkCreateDebugUtilsMessengerEXT(m_instance, &dbg, nullptr, &m_debug);
        LOG_INFO("Vulkan") << "включён слой валидации VK_LAYER_KHRONOS_validation";
    }
    return true;
}

bool VulkanDevice::PickGpu() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) {
        LOG_ERROR("Vulkan") << "загрузчик есть, но ни одного устройства Vulkan не найдено "
                               "(драйвер не установлен?)";
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(m_instance, &count, gpus.data());

    int best = -1;
    VkPhysicalDeviceProperties bestProps{};
    for (VkPhysicalDevice gpu : gpus) {
        if (FindGraphicsFamily(gpu) == UINT32_MAX) continue;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(gpu, &props);
        const int score = ScoreGpu(props);
        if (score > best) {
            best = score;
            bestProps = props;
            m_gpu = gpu;
        }
    }
    if (m_gpu == VK_NULL_HANDLE) {
        LOG_ERROR("Vulkan") << "ни у одного устройства нет графической очереди";
        return false;
    }

    m_queueFamily = FindGraphicsFamily(m_gpu);
    vkGetPhysicalDeviceMemoryProperties(m_gpu, &m_memProps);
    m_maxAnisotropy = bestProps.limits.maxSamplerAnisotropy;

    char buf[256];
    std::snprintf(buf, sizeof(buf), "Vulkan %u.%u.%u — %s (%s)",
                  VK_VERSION_MAJOR(bestProps.apiVersion), VK_VERSION_MINOR(bestProps.apiVersion),
                  VK_VERSION_PATCH(bestProps.apiVersion), bestProps.deviceName,
                  DeviceTypeName(bestProps.deviceType).c_str());
    m_apiVersion = buf;
    LOG_INFO("Vulkan") << m_apiVersion;
    return true;
}

bool VulkanDevice::CreateLogicalDevice() {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = m_queueFamily;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;

    // Возможности запрашиваем ТОЛЬКО те, что устройство реально даёт: просьба о
    // недоступной — отказ в создании устройства целиком. Каркасный режим
    // (fillModeNonSolid) и анизотропия нужны редактору, но игра без них
    // проживёт, поэтому они не обязательны.
    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(m_gpu, &have);
    VkPhysicalDeviceFeatures want{};
    want.samplerAnisotropy = have.samplerAnisotropy;
    want.fillModeNonSolid = have.fillModeNonSolid;
    want.depthClamp = have.depthClamp;
    want.independentBlend = have.independentBlend;
    want.occlusionQueryPrecise = have.occlusionQueryPrecise;
    if (!have.fillModeNonSolid) {
        LOG_WARN("Vulkan") << "устройство не умеет каркасный режим (fillModeNonSolid) — "
                              "wireframe в редакторе будет рисоваться заливкой";
    }
    if (!have.samplerAnisotropy) m_maxAnisotropy = 1.0f;

    std::vector<const char*> extensions;
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(m_gpu, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> available(extCount);
    vkEnumerateDeviceExtensionProperties(m_gpu, nullptr, &extCount, available.data());
    auto has = [&](const char* name) {
        for (const VkExtensionProperties& e : available) {
            if (std::strcmp(e.extensionName, name) == 0) return true;
        }
        return false;
    };
    // swapchain нужен только окну — headless-прогону он ни к чему.
    if (has(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    // --- Диапазон глубины: [-1, 1] против [0, 1] ---------------------------
    //
    // RHI договорился о диапазоне OpenGL: NDC-глубина от -1 до 1 (см.
    // GraphicsDevice.h). У Vulkan он [0, 1], и матрицы проекции движка,
    // построенные glm по правилам GL, дают там неверную глубину — БЛИЗКИЕ
    // объекты рисуются правильно, а дальние уезжают. Дефект, который на первый
    // взгляд выглядит как «что-то с камерой».
    //
    // VK_EXT_depth_clip_control разрешает попросить у Vulkan диапазон GL. Это
    // единственный способ не трогать ни одну матрицу в движке.
    //
    // ЕСЛИ РАСШИРЕНИЯ НЕТ — устройство честно считается непригодным, и
    // Application откатывается на OpenGL. Альтернатива (переписывать
    // gl_Position.z в каждом вершинном шейдере) работала бы везде, но это
    // текстовая правка чужого кода, которую здесь нечем проверить: на этой
    // машине расширение есть, значит ветка без него осталась бы непроверенной.
    // Правильный OpenGL лучше непроверенного Vulkan.
    VkPhysicalDeviceDepthClipControlFeaturesEXT depthClip{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    if (has(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME)) {
        VkPhysicalDeviceDepthClipControlFeaturesEXT probe{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 all{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        all.pNext = &probe;
        vkGetPhysicalDeviceFeatures2(m_gpu, &all);
        if (probe.depthClipControl) {
            extensions.push_back(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
            depthClip.depthClipControl = VK_TRUE;
            m_depthClipControl = true;
        }
    }
    if (!m_depthClipControl) {
        LOG_WARN("Vulkan") << "нет VK_EXT_depth_clip_control — диапазон глубины OpenGL "
                              "([-1,1]) запросить нечем, бэкенд непригоден";
        return false;
    }

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &depthClip;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.pEnabledFeatures = &want;
    info.enabledExtensionCount = (uint32_t)extensions.size();
    info.ppEnabledExtensionNames = extensions.data();

    if (!vk::Check(vkCreateDevice(m_gpu, &info, nullptr, &m_device), "vkCreateDevice")) return false;
    volkLoadDevice(m_device);
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);

    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = m_queueFamily;
    return vk::Check(vkCreateCommandPool(m_device, &pool, nullptr, &m_pool), "vkCreateCommandPool");
}

void VulkanDevice::Init(ProcLoader /*loader*/) {
    // loader не используется намеренно: он отдаёт адреса функций OpenGL от
    // оконной системы, а Vulkan грузит свои через собственный загрузчик. Аргумент
    // остаётся в интерфейсе, потому что он часть контракта GraphicsDevice.
    if (!EnsureVolk()) {
        LOG_ERROR("Vulkan") << "загрузчик Vulkan не найден (vulkan-1.dll / libvulkan.so.1)";
        return;
    }
    if (!CreateInstance()) return;
    if (!PickGpu()) return;
    if (!CreateLogicalDevice()) return;
    m_allocator = CreateAllocator(m_instance, m_gpu, m_device);
    if (m_allocator) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_gpu, &props);
        m_uniformAlignment = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 4);

        // Кольцо униформ на кадр. 8 МиБ — с большим запасом: тяжёлый кадр
        // движка делает порядка тысячи вызовов отрисовки, блок униформ у
        // самого крупного шейдера меньше килобайта.
        m_uniformRing.Create(*this, 8u << 20, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                             MemoryUse::CpuToGpu);

        // Пул дескрипторов. Сбрасывается целиком при отправке кадра — по набору
        // на вызов отрисовки, освобождать поштучно незачем.
        const uint32_t kSets = 4096;
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = kSets;
        sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[1].descriptorCount = kSets * 8;
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = kSets;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = sizes;
        vk::Check(vkCreateDescriptorPool(m_device, &pool, nullptr, &m_descriptorPool),
                  "vkCreateDescriptorPool");
    }
    if (!m_allocator) {
        // Без распределителя ресурсы не создать — устройство честно считается
        // непригодным, а не «почти работающим».
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
        m_apiVersion = "Vulkan (распределитель памяти не создан)";
    }
}

uint32_t VulkanDevice::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const {
    for (uint32_t i = 0; i < m_memProps.memoryTypeCount; ++i) {
        const bool allowed = (typeBits & (1u << i)) != 0;
        const bool suits = (m_memProps.memoryTypes[i].propertyFlags & props) == props;
        if (allowed && suits) return i;
    }
    return UINT32_MAX;
}

void VulkanDevice::SubmitImmediate(const std::function<void(VkCommandBuffer)>& record) {
    if (!Ready()) return;

    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = m_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!vk::Check(vkAllocateCommandBuffers(m_device, &alloc, &cmd), "vkAllocateCommandBuffers")) return;

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    record(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(m_queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_queue);
    vkFreeCommandBuffers(m_device, m_pool, 1, &cmd);
}

// --- Сэмплеры и реестр текстур ------------------------------------------------

VkSampler VulkanDevice::SamplerFor(const SamplerKey& key) {
    auto it = m_samplers.find(key);
    if (it != m_samplers.end()) return it->second;

    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const bool nearest = key.FilterMode == Filter::Nearest;
    info.magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.minFilter = info.magFilter;
    // Trilinear и Anisotropic отличаются от Bilinear именно интерполяцией МЕЖДУ
    // мипами; без мипов разницы нет, и просить её было бы враньём.
    const bool between = key.Mips && (key.FilterMode == Filter::Trilinear ||
                                      key.FilterMode == Filter::Anisotropic);
    info.mipmapMode = between ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    const VkSamplerAddressMode address =
        key.WhiteBorder ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER
                        : (key.WrapMode == Wrap::ClampEdge ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                                           : VK_SAMPLER_ADDRESS_MODE_REPEAT);
    info.addressModeU = info.addressModeV = info.addressModeW = address;
    info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    info.maxLod = key.Mips ? VK_LOD_CLAMP_NONE : 0.0f;
    if (key.FilterMode == Filter::Anisotropic && m_maxAnisotropy > 1.0f) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = m_maxAnisotropy;
    }

    VkSampler sampler = VK_NULL_HANDLE;
    if (!vk::Check(vkCreateSampler(m_device, &info, nullptr, &sampler), "vkCreateSampler")) {
        return VK_NULL_HANDLE;
    }
    m_samplers.emplace(key, sampler);
    return sampler;
}

TextureHandle VulkanDevice::RegisterTexture(VkImageView view, VkSampler sampler) {
    const uint64_t id = m_nextTextureHandle++;
    m_textures[id] = TextureBinding{view, sampler};
    return TextureHandle{id};
}

void VulkanDevice::UpdateTexture(TextureHandle handle, VkImageView view, VkSampler sampler) {
    auto it = m_textures.find(handle.Value);
    if (it != m_textures.end()) it->second = TextureBinding{view, sampler};
}

void VulkanDevice::UnregisterTexture(TextureHandle handle) {
    m_textures.erase(handle.Value);
}

const VulkanDevice::TextureBinding* VulkanDevice::LookupTexture(TextureHandle handle) const {
    auto it = m_textures.find(handle.Value);
    return it == m_textures.end() ? nullptr : &it->second;
}

const VulkanDevice::TextureBinding* VulkanDevice::BoundTexture(int unit) const {
    if (unit < 0 || unit >= kMaxTextureUnits) return nullptr;
    return LookupTexture(m_boundTextures[unit]);
}

void VulkanDevice::BindTexture2D(int unit, TextureHandle texture) {
    if (unit < 0 || unit >= kMaxTextureUnits) return;
    // Невалидный хендл — «отвязать», ровно как договорился RHI. Молча
    // игнорировать его нельзя: тогда в юните остался бы прошлый кадр, и на
    // экране появилась бы чужая текстура вместо честно пустого места.
    m_boundTextures[unit] = texture.Valid() && LookupTexture(texture) ? texture : TextureHandle{};
}

void VulkanDevice::BindShader(const VulkanShaderProgram* shader) { m_shader = shader; }

// ============================================================================
//  Запись команд
// ============================================================================

void VulkanDevice::EnsureCommandBuffer() {
    if (m_recording) return;
    if (m_cmd == VK_NULL_HANDLE) {
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = m_pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        if (!vk::Check(vkAllocateCommandBuffers(m_device, &alloc, &m_cmd),
                       "vkAllocateCommandBuffers (кадр)")) {
            return;
        }
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vk::Check(vkCreateFence(m_device, &fence, nullptr, &m_fence), "vkCreateFence");
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(m_cmd, 0);
    vkBeginCommandBuffer(m_cmd, &begin);
    m_recording = true;
}

void VulkanDevice::ApplyViewportAndScissor() {
    if (!m_inPass) return;
    const int targetH = m_target ? m_target->Height() : m_viewport[3];

    // ОТРИЦАТЕЛЬНАЯ ВЫСОТА — и это не трюк, а штатный способ (VK_KHR_maintenance1,
    // ядро с Vulkan 1.1). У Vulkan начало отсчёта сверху, у RHI — снизу
    // (см. GraphicsDevice.h). Перевернув viewport, мы приводим весь бэкенд к
    // соглашению RHI один раз здесь, вместо того чтобы переворачивать каждую
    // матрицу проекции и каждую текстурную координату по всему движку.
    VkViewport vp{};
    vp.x = (float)m_viewport[0];
    vp.y = (float)(targetH - m_viewport[1]);
    vp.width = (float)m_viewport[2];
    vp.height = -(float)m_viewport[3];
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(m_cmd, 0, 1, &vp);

    // Ножницы переворачиваются вручную: у них нет отрицательной высоты, а
    // договорённость та же — (x, y) считается от ЛЕВОГО НИЖНЕГО угла.
    VkRect2D rect{};
    if (m_scissorOn) {
        rect.offset.x = std::max(m_scissor[0], 0);
        rect.offset.y = std::max(targetH - m_scissor[1] - m_scissor[3], 0);
        rect.extent.width = (uint32_t)std::max(m_scissor[2], 0);
        rect.extent.height = (uint32_t)std::max(m_scissor[3], 0);
    } else {
        rect.extent.width = (uint32_t)std::max(m_viewport[2], 0);
        rect.extent.height = (uint32_t)std::max(m_viewport[3], 0);
    }
    vkCmdSetScissor(m_cmd, 0, 1, &rect);
}

void VulkanDevice::EnsureRenderPass(bool wantClear) {
    if (m_inPass || !m_target) return;
    EnsureCommandBuffer();
    if (!m_recording) return;

    VkClearValue clears[3]{};
    clears[0].color = {{m_clear[0], m_clear[1], m_clear[2], m_clear[3]}};
    clears[1].depthStencil = {1.0f, 0};
    clears[2].color = {{m_clear[0], m_clear[1], m_clear[2], m_clear[3]}};

    VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    info.renderPass = wantClear ? m_target->ClearPass() : m_target->LoadPass();
    info.framebuffer = m_target->Framebuffer();
    info.renderArea.extent = {(uint32_t)m_target->Width(), (uint32_t)m_target->Height()};
    info.clearValueCount = 3;
    info.pClearValues = clears;
    vkCmdBeginRenderPass(m_cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
    m_inPass = true;
    ApplyViewportAndScissor();
}

void VulkanDevice::EndRenderPass() {
    if (!m_inPass) return;
    vkCmdEndRenderPass(m_cmd);
    m_inPass = false;
    // Проход сменил раскладки вложений по своему описанию — объекты об этом
    // сами не узнают, а следующий барьер обязан считать от правильной.
    if (m_target) const_cast<VulkanRenderTarget*>(m_target)->OnPassEnded();
}

void VulkanDevice::FlushCommands() {
    EndRenderPass();
    if (!m_recording) return;
    vkEndCommandBuffer(m_cmd);
    m_recording = false;

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &m_cmd;
    vkResetFences(m_device, 1, &m_fence);
    vkQueueSubmit(m_queue, 1, &submit, m_fence);
    // Ждём здесь и не притворяемся, что это быстро: пока результат кадра
    // забирают чтением пикселей, ждать всё равно придётся, а барьер в одном
    // месте лучше, чем гонка в нескольких.
    vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);

    // Кадр исполнен — наборы дескрипторов и кольцо униформ свободны. Сброс
    // ЗДЕСЬ, после ожидания: сбросить их раньше значило бы отобрать у GPU то,
    // что он ещё читает.
    if (m_descriptorPool != VK_NULL_HANDLE) vkResetDescriptorPool(m_device, m_descriptorPool, 0);
    m_uniformOffset = 0;
}

void VulkanDevice::Clear(bool color, bool depth) {
    if (!m_target) return;
    // Очистка ВСЕГО таргета без ножниц и до всяких команд — это ровно тот
    // случай, ради которого существует проход с loadOp = CLEAR: драйвер не
    // читает старое содержимое вовсе. Открываем проход с очисткой.
    if (!m_inPass && !m_scissorOn) {
        EnsureRenderPass(/*wantClear=*/true);
        return;
    }
    // Иначе очищаем внутри уже открытого прохода — здесь очистка уважает
    // ножницы, как того и требует контракт RHI.
    EnsureRenderPass(/*wantClear=*/false);
    if (!m_inPass) return;

    VkClearAttachment attach[2]{};
    uint32_t count = 0;
    if (color && m_target->HasColor()) {
        attach[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attach[count].colorAttachment = 0;
        attach[count].clearValue.color = {{m_clear[0], m_clear[1], m_clear[2], m_clear[3]}};
        ++count;
    }
    if (depth && m_target->HasDepth()) {
        attach[count].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        attach[count].clearValue.depthStencil = {1.0f, 0};
        ++count;
    }
    if (count == 0) return;

    const int targetH = m_target->Height();
    VkClearRect rect{};
    rect.layerCount = 1;
    if (m_scissorOn) {
        rect.rect.offset.x = std::max(m_scissor[0], 0);
        rect.rect.offset.y = std::max(targetH - m_scissor[1] - m_scissor[3], 0);
        rect.rect.extent.width = (uint32_t)std::max(m_scissor[2], 0);
        rect.rect.extent.height = (uint32_t)std::max(m_scissor[3], 0);
    } else {
        rect.rect.extent = {(uint32_t)m_target->Width(), (uint32_t)targetH};
    }
    vkCmdClearAttachments(m_cmd, count, attach, 1, &rect);
}

void VulkanDevice::BindDefaultFramebuffer() {
    // Экранного буфера у бэкенда пока нет (swapchain — следующий этап).
    // Отвязываем цель и закрываем проход: движок зовёт это в конце кадра, и
    // «ничего не делать» здесь означало бы оставить проход открытым навсегда.
    EndRenderPass();
    m_target = nullptr;
}

void VulkanDevice::ReadPixelsRGB(int x, int y, int width, int height, unsigned char* out) {
    if (!out || width <= 0 || height <= 0) return;
    std::memset(out, 0, (size_t)width * height * 3);
    if (!m_target || !m_target->HasColor()) return;

    // Всё записанное обязано ИСПОЛНИТЬСЯ до чтения. В OpenGL за это отвечал
    // glFinish внутри бэкенда; здесь — закрытие прохода, отправка и ожидание.
    const VulkanRenderTarget* target = m_target;
    FlushCommands();

    VulkanImage& image = const_cast<VulkanRenderTarget*>(target)->ColorImage();
    const int targetH = target->Height();

    VulkanBuffer readback;
    // Цвет таргета — RGBA16F (см. CreateStorage), то есть 8 байт на пиксель.
    const VkDeviceSize bytes = (VkDeviceSize)width * height * 8;
    if (!readback.Create(*this, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUse::GpuToCpu)) {
        return;
    }

    SubmitImmediate([&](VkCommandBuffer cmd) {
        image.TransitionTo(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        // Строки RHI идут СНИЗУ ВВЕРХ, у изображения — сверху вниз. Читаем
        // прямоугольник, перевёрнутый по вертикали, и разворачиваем ниже.
        region.imageOffset = {x, std::max(targetH - y - height, 0), 0};
        region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
        vkCmdCopyImageToBuffer(cmd, image.Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback.Handle(), 1, &region);
        image.TransitionTo(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });

    const uint16_t* src = static_cast<const uint16_t*>(readback.Mapped());
    if (!src) return;

    // Половинная точность → байты. Своя распаковка half: тянуть ради неё
    // зависимость незачем, а формат прост — знак, пять бит порядка, десять
    // мантиссы. Денормали и бесконечности здесь не встречаются (цвет кадра
    // после тон-маппинга лежит в [0,1]), но обрабатываются, чтобы мусор в
    // буфере не превращался в случайные яркие точки.
    auto halfToFloat = [](uint16_t h) -> float {
        const uint32_t sign = (uint32_t)(h & 0x8000) << 16;
        uint32_t exp = (h >> 10) & 0x1F;
        uint32_t mant = h & 0x3FF;
        if (exp == 0) {
            if (mant == 0) { float f; uint32_t bits = sign; std::memcpy(&f, &bits, 4); return f; }
            // Денормаль: нормализуем сдвигом.
            while (!(mant & 0x400)) { mant <<= 1; --exp; }
            ++exp;
            mant &= 0x3FF;
        } else if (exp == 31) {
            const uint32_t bits = sign | 0x7F800000u | (mant << 13);
            float f; std::memcpy(&f, &bits, 4); return f;
        }
        const uint32_t bits = sign | ((exp + 112) << 23) | (mant << 13);
        float f; std::memcpy(&f, &bits, 4); return f;
    };

    for (int row = 0; row < height; ++row) {
        // Переворот: первая строка выхода — нижняя строка изображения.
        const uint16_t* line = src + (size_t)(height - 1 - row) * width * 4;
        unsigned char* dst = out + (size_t)row * width * 3;
        for (int col = 0; col < width; ++col) {
            for (int c = 0; c < 3; ++c) {
                const float v = halfToFloat(line[col * 4 + c]);
                dst[col * 3 + c] = (unsigned char)std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f);
            }
        }
    }
}

void VulkanDevice::BindRenderTarget(const VulkanRenderTarget* target) {
    // Смена цели закрывает открытый проход: рисовать в два таргета одним
    // проходом нельзя, а «забыть закрыть» — это отказ валидации при отправке.
    if (target != m_target) EndRenderPass();
    m_target = target;
    if (target) SetViewport(0, 0, target->Width(), target->Height());
}

// --- Состояние ---------------------------------------------------------------

void VulkanDevice::SetViewport(int x, int y, int width, int height) {
    m_viewport[0] = x; m_viewport[1] = y; m_viewport[2] = width; m_viewport[3] = height;
    ApplyViewportAndScissor();
}

void VulkanDevice::SetClearColor(float r, float g, float b, float a) {
    m_clear[0] = r; m_clear[1] = g; m_clear[2] = b; m_clear[3] = a;
}

void VulkanDevice::SetScissor(bool enabled, int x, int y, int w, int h) {
    m_scissorOn = enabled;
    m_scissor[0] = x; m_scissor[1] = y; m_scissor[2] = w; m_scissor[3] = h;
    ApplyViewportAndScissor();
}

// --- Пока не реализовано -----------------------------------------------------
//
// Ниже — то, что ждёт следующих этапов бэкенда (буферы и текстуры, компиляция
// GLSL в SPIR-V, кэш конвейеров, проходы). Заглушки возвращают nullptr и
// говорят об этом ОДИН раз: движок в этом состоянии не рисует, и повторять
// сообщение на каждый ресурс значило бы залить лог так, что настоящую ошибку в
// нём не найти.

namespace {
void OnceNotImplemented(const char* what) {
    static std::vector<std::string> said;
    for (const std::string& s : said) {
        if (s == what) return;
    }
    said.emplace_back(what);
    LOG_WARN("Vulkan") << what << ": ещё не реализовано в этом бэкенде";
}
} // namespace






std::unique_ptr<ShaderProgram> VulkanDevice::CreateShaderProgram(const std::string& vertexSrc,
                                                                 const std::string& fragmentSrc) {
    if (!Ready()) return nullptr;
    auto program = std::make_unique<VulkanShaderProgram>(*this, vertexSrc, fragmentSrc);
    // Несобравшийся шейдер отдаётся как nullptr, а не как нерабочий объект:
    // вызывающий уже умеет обходиться без шейдера (в GL-бэкенде так же), а
    // объект, который молча ничего не делает, дал бы чёрный экран без причины.
    if (!program->Valid()) return nullptr;
    return program;
}
std::unique_ptr<Geometry> VulkanDevice::CreateGeometry(const VertexLayout& layout) {
    if (!Ready()) return nullptr;
    return std::make_unique<VulkanGeometry>(*this, layout);
}
std::unique_ptr<Texture2D> VulkanDevice::CreateTexture2D(const Texture2DDesc& desc,
                                                         const void* pixels) {
    if (!Ready()) return nullptr;
    return std::make_unique<VulkanTexture2D>(*this, desc, pixels);
}
std::unique_ptr<Texture3D> VulkanDevice::CreateTexture3D(const Texture3DDesc& desc,
                                                         const float* pixels) {
    if (!Ready()) return nullptr;
    return std::make_unique<VulkanTexture3D>(*this, desc, pixels);
}
std::unique_ptr<TextureCube> VulkanDevice::CreateTextureCube(const CubeFacePixels faces[6]) {
    if (!Ready()) return nullptr;
    return std::make_unique<VulkanTextureCube>(*this, faces);
}
std::unique_ptr<RenderTarget> VulkanDevice::CreateRenderTarget(const RenderTargetDesc& desc) {
    if (!Ready()) return nullptr;
    return std::make_unique<VulkanRenderTarget>(*this, desc);
}

} // namespace sage::rhi
