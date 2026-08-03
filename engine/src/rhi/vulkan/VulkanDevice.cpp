#include "VulkanDevice.h"
#include "VulkanMemory.h"

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
    // Список расширений устройства: swapchain нужен только окну, и его наличие
    // проверяется отдельно — headless-прогону он ни к чему.
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(m_gpu, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> available(extCount);
    vkEnumerateDeviceExtensionProperties(m_gpu, nullptr, &extCount, available.data());
    for (const VkExtensionProperties& e : available) {
        if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
            extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        }
    }

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
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

// --- Состояние ---------------------------------------------------------------

void VulkanDevice::SetViewport(int x, int y, int width, int height) {
    m_viewport[0] = x; m_viewport[1] = y; m_viewport[2] = width; m_viewport[3] = height;
}

void VulkanDevice::SetClearColor(float r, float g, float b, float a) {
    m_clear[0] = r; m_clear[1] = g; m_clear[2] = b; m_clear[3] = a;
}

void VulkanDevice::SetScissor(bool enabled, int x, int y, int w, int h) {
    m_scissorOn = enabled;
    m_scissor[0] = x; m_scissor[1] = y; m_scissor[2] = w; m_scissor[3] = h;
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

void VulkanDevice::Clear(bool, bool) { OnceNotImplemented("Clear"); }
void VulkanDevice::BindDefaultFramebuffer() { OnceNotImplemented("BindDefaultFramebuffer"); }
void VulkanDevice::BindTexture2D(int, TextureHandle) { OnceNotImplemented("BindTexture2D"); }

void VulkanDevice::ReadPixelsRGB(int, int, int width, int height, unsigned char* out) {
    OnceNotImplemented("ReadPixelsRGB");
    if (out && width > 0 && height > 0) std::memset(out, 0, (size_t)width * height * 3);
}

std::unique_ptr<ShaderProgram> VulkanDevice::CreateShaderProgram(const std::string&, const std::string&) {
    OnceNotImplemented("CreateShaderProgram");
    return nullptr;
}
std::unique_ptr<Geometry> VulkanDevice::CreateGeometry(const VertexLayout&) {
    OnceNotImplemented("CreateGeometry");
    return nullptr;
}
std::unique_ptr<Texture2D> VulkanDevice::CreateTexture2D(const Texture2DDesc&, const void*) {
    OnceNotImplemented("CreateTexture2D");
    return nullptr;
}
std::unique_ptr<Texture3D> VulkanDevice::CreateTexture3D(const Texture3DDesc&, const float*) {
    OnceNotImplemented("CreateTexture3D");
    return nullptr;
}
std::unique_ptr<TextureCube> VulkanDevice::CreateTextureCube(const CubeFacePixels[6]) {
    OnceNotImplemented("CreateTextureCube");
    return nullptr;
}
std::unique_ptr<RenderTarget> VulkanDevice::CreateRenderTarget(const RenderTargetDesc&) {
    OnceNotImplemented("CreateRenderTarget");
    return nullptr;
}

} // namespace sage::rhi
