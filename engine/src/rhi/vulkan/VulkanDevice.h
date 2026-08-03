#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "VulkanCommon.h"
#include "sage/rhi/GraphicsDevice.h"

// ---------------------------------------------------------------------------
// Vulkan-бэкенд RHI.
//
// ЧЕСТНО О ФОРМЕ ИНТЕРФЕЙСА. RHI движка написан как машина состояний OpenGL:
// SetBlend, SetDepthTest, BindTexture2D, Draw. У Vulkan состояние конвейера
// запекается в объект (VkPipeline) ЗАРАНЕЕ, ресурсы адресуются наборами
// дескрипторов, а команды пишутся в буферы. Об этом расхождении прямо сказано
// в GraphicsDevice.h, и оно никуда не делось.
//
// Разрешается оно так: сеттеры состояния ничего не делают немедленно, а
// складываются в структуру State; в момент отрисовки по ней ИЩЕТСЯ готовый
// конвейер, и создаётся новый, только если такого сочетания ещё не было. Это
// не «эмуляция OpenGL поверх Vulkan» — это ровно то, что делает всякий
// движок с ретро-совместимым слоем: количество реальных сочетаний состояния в
// кадре измеряется десятками, кэш прогревается за первые кадры и дальше стоит
// один поиск в хеш-таблице на вызов отрисовки.
//
// ЧТО ОСТАЁТСЯ ХУЖЕ, ЧЕМ У НАСТОЯЩЕГО VULKAN-РЕНДЕРА, и это надо знать:
//   • команды пишутся в один буфер последовательно — никакой записи из
//     нескольких потоков, ради которой Vulkan обычно и берут;
//   • имена униформ (SetMat4("uModel", ...)) требуют поиска по имени и записи
//     в uniform-буфер на каждый вызов;
//   • переключение рендер-таргета закрывает и открывает проход заново.
// Это цена совместимости с существующим кодом движка. Выигрыш — предсказуемое
// поведение драйвера, валидационные слои и путь на платформы, где OpenGL
// мёртв (macOS через MoltenVK, Android).
//
// БЕЗ SDK. Точки входа грузит volk из системного загрузчика в рантайме, поэтому
// сборка не требует ни LunarG SDK, ни линковки с vulkan-1.lib, а запуск на
// машине без Vulkan не падает: Available() отвечает false, и Application
// откатывается на OpenGL.
// ---------------------------------------------------------------------------
// Распределитель VMA объявляется в ГЛОБАЛЬНОЙ области, как он и определён
// (VK_DEFINE_HANDLE в vk_mem_alloc.h). Написать `struct VmaAllocator_T*` внутри
// namespace нельзя: это объявит НОВЫЙ тип sage::rhi::VmaAllocator_T, сборка
// пройдёт, а линковка упадёт на несовпадении сигнатур — что и случилось.
struct VmaAllocator_T;

namespace sage::rhi {

class VulkanDevice final : public GraphicsDevice {
public:
    VulkanDevice();
    ~VulkanDevice() override;

    // Есть ли на этой машине рабочий Vulkan: загрузчик найден, инстанс
    // создаётся, видеокарта с графической очередью существует. Полная проверка,
    // а не «нашли dll»: загрузчик ставится вместе с системой и на машинах, где
    // драйвер Vulkan не установлен вовсе — там он честно вернёт ноль устройств.
    static bool Available();

    void Init(ProcLoader loader) override;
    const char* BackendName() const override { return "Vulkan"; }
    std::string ApiVersion() const override { return m_apiVersion; }

    // --- Состояние конвейера: складывается, а не применяется ---------------
    void SetViewport(int x, int y, int width, int height) override;
    void SetClearColor(float r, float g, float b, float a) override;
    void Clear(bool color = true, bool depth = true) override;
    void BindDefaultFramebuffer() override;
    void SetBlend(bool enabled) override { m_state.Blend = enabled; }
    void SetDepthTest(bool enabled) override { m_state.DepthTest = enabled; }
    void SetDepthWrite(bool enabled) override { m_state.DepthWrite = enabled; }
    void SetDepthFunc(DepthFunc func) override { m_state.Depth = func; }
    void SetCullMode(CullMode mode) override { m_state.Cull = mode; }
    void SetFrontFace(FrontFace face) override { m_state.Front = face; }
    void SetPolygonMode(PolygonMode mode) override { m_state.Polygon = mode; }
    void SetColorWrite(bool enabled) override { m_state.ColorWrite = enabled; }
    void SetScissor(bool enabled, int x = 0, int y = 0, int w = 0, int h = 0) override;
    void SetSRGBWrite(bool enabled) override { m_state.SrgbWrite = enabled; }
    void BindTexture2D(int unit, TextureHandle texture) override;

    void ReadPixelsRGB(int x, int y, int width, int height, unsigned char* out) override;
    float MaxAnisotropy() override { return m_maxAnisotropy; }

    // --- Фабрики ресурсов ---------------------------------------------------
    std::unique_ptr<ShaderProgram> CreateShaderProgram(const std::string& vertexSrc,
                                                       const std::string& fragmentSrc) override;
    std::unique_ptr<Geometry> CreateGeometry(const VertexLayout& layout) override;
    std::unique_ptr<Texture2D> CreateTexture2D(const Texture2DDesc& desc, const void* pixels) override;
    std::unique_ptr<Texture3D> CreateTexture3D(const Texture3DDesc& desc, const float* pixels) override;
    std::unique_ptr<TextureCube> CreateTextureCube(const CubeFacePixels faces[6]) override;
    std::unique_ptr<RenderTarget> CreateRenderTarget(const RenderTargetDesc& desc) override;

    // --- Внутреннее (для остальных файлов бэкенда) --------------------------
    VkDevice Handle() const { return m_device; }
    VkPhysicalDevice Gpu() const { return m_gpu; }
    VkQueue GraphicsQueue() const { return m_queue; }
    uint32_t QueueFamily() const { return m_queueFamily; }
    bool Ready() const { return m_device != VK_NULL_HANDLE; }

    // Тип памяти под требования + желаемые свойства. UINT32_MAX — не нашлось.
    uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;

    // Распределитель видеопамяти (VMA). Владеет устройство: время жизни
    // распределителя обязано покрывать время жизни всех ресурсов.
    ::VmaAllocator_T* Allocator() const { return m_allocator; }

    // Разовая отправка команд с ожиданием (загрузка текстур, смена раскладок).
    // Медленно по определению — только для инициализации ресурсов, не для кадра.
    void SubmitImmediate(const std::function<void(VkCommandBuffer)>& record);

private:
    // Всё состояние конвейера, которое движок задаёт сеттерами. Ключ кэша
    // конвейеров: одинаковое состояние + одинаковый шейдер = один VkPipeline.
    struct State {
        bool Blend = false;
        bool DepthTest = true;
        bool DepthWrite = true;
        bool ColorWrite = true;
        bool SrgbWrite = false;
        DepthFunc Depth = DepthFunc::Less;
        CullMode Cull = CullMode::Back;
        FrontFace Front = FrontFace::CounterClockwise;
        PolygonMode Polygon = PolygonMode::Fill;
    };

    bool CreateInstance();
    bool PickGpu();
    bool CreateLogicalDevice();

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_gpu = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties m_memProps{};
    VkDebugUtilsMessengerEXT m_debug = VK_NULL_HANDLE;
    ::VmaAllocator_T* m_allocator = nullptr;

    State m_state;
    float m_clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    int m_viewport[4] = {0, 0, 0, 0};
    bool m_scissorOn = false;
    int m_scissor[4] = {0, 0, 0, 0};
    float m_maxAnisotropy = 1.0f;
    std::string m_apiVersion = "Vulkan (не инициализирован)";
};

} // namespace sage::rhi
