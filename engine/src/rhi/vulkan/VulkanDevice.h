#pragma once
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "VulkanCommon.h"
#include "VulkanMemory.h"
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

// Объявление НА УРОВНЕ ПРОСТРАНСТВА ИМЁН, а не внутри класса: `class X;` в теле
// класса объявляет ВЛОЖЕННЫЙ тип VulkanDevice::X, и дальше всё разъезжается —
// метод принимает один тип, а фабрика возвращает другой с тем же именем.
class VulkanRenderTarget;
class VulkanShaderProgram;
class VulkanGeometry;

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
    // Умеет ли устройство диапазон глубины OpenGL. Без него бэкенд не
    // поднимается вовсе (см. CreateLogicalDevice) — геттер нужен конвейерам.
    bool DepthClipControl() const { return m_depthClipControl; }

    // Тип памяти под требования + желаемые свойства. UINT32_MAX — не нашлось.
    uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;

    // Распределитель видеопамяти (VMA). Владеет устройство: время жизни
    // распределителя обязано покрывать время жизни всех ресурсов.
    ::VmaAllocator_T* Allocator() const { return m_allocator; }

    // Разовая отправка команд с ожиданием (загрузка текстур, смена раскладок).
    // Медленно по определению — только для инициализации ресурсов, не для кадра.
    void SubmitImmediate(const std::function<void(VkCommandBuffer)>& record);

    // --- Запись команд кадра -----------------------------------------------
    //
    // ЧЕГО НЕТ В OpenGL. Там команда исполняется в момент вызова (точнее, так
    // это выглядит снаружи). В Vulkan команды ПИШУТСЯ в буфер и исполняются
    // после отправки в очередь, а рисовать можно только внутри открытого
    // прохода. RHI об этом не знает и не должен: он говорит «привяжи таргет»,
    // «очисти», «нарисуй» — как раньше.
    //
    // Поэтому проход открывается ЛЕНИВО, при первой команде, которой он
    // действительно нужен, и закрывается, когда меняют цель или требуют
    // результат (чтение пикселей). Так «Bind() без единой команды» ничего не
    // стоит, а очистка сразу после Bind обходится без отдельной команды —
    // проход открывается сразу с очисткой.
    void FlushCommands();          // закрыть проход, отправить, дождаться

private:
    void EnsureCommandBuffer();
    void EnsureRenderPass(bool wantClear);
    void EndRenderPass();
    // Динамические viewport/scissor текущего прохода. Начало отсчёта у Vulkan
    // СВЕРХУ, а RHI договорился о нижнем — пересчёт здесь, в одном месте.
    void ApplyViewportAndScissor();

public:

    // --- Сэмплеры ----------------------------------------------------------
    //
    // В OpenGL параметры фильтрации живут В ТЕКСТУРЕ; в Vulkan это отдельный
    // объект, и одинаковых сочетаний на всю сцену — единицы. Кэшируем: сэмплер
    // на текстуру означал бы тысячи одинаковых объектов и упор в
    // maxSamplerAllocationCount (на части устройств он всего 4000).
    struct SamplerKey {
        Filter FilterMode = Filter::Trilinear;
        Wrap WrapMode = Wrap::Repeat;
        bool Mips = true;
        // Белая рамка за границей — карта теней: вне покрытия «освещено»
        // (см. RenderTargetKind::DepthOnly в GL-бэкенде).
        bool WhiteBorder = false;
        bool operator==(const SamplerKey& o) const {
            return FilterMode == o.FilterMode && WrapMode == o.WrapMode && Mips == o.Mips &&
                   WhiteBorder == o.WhiteBorder;
        }
    };
    VkSampler SamplerFor(const SamplerKey& key);

    // --- Реестр текстурных хендлов -----------------------------------------
    //
    // RHI отдаёт наружу непрозрачный 64-битный TextureHandle, а Vulkan для
    // выборки нужна ПАРА (представление, сэмплер). Указатель на объект в
    // хендле не годится: хендлы переживают свои текстуры (кадр опаздывает на
    // два), и разыменование мёртвого указателя — это падение вместо чёрного
    // квадрата. Счётчик с таблицей даёт то же самое, но промах по нему —
    // просто «текстуры нет».
    struct TextureBinding {
        VkImageView View = VK_NULL_HANDLE;
        VkSampler Sampler = VK_NULL_HANDLE;
    };
    TextureHandle RegisterTexture(VkImageView view, VkSampler sampler);
    void UpdateTexture(TextureHandle handle, VkImageView view, VkSampler sampler);
    void UnregisterTexture(TextureHandle handle);
    const TextureBinding* LookupTexture(TextureHandle handle) const;
    // Что сейчас привязано к текстурному юниту (nullptr — ничего).
    const TextureBinding* BoundTexture(int unit) const;

    // Текущая цель отрисовки. Пока это только запоминание: открытие прохода и
    // запись команд появятся вместе с конвейерами. Отдельный метод, а не
    // публичное поле, потому что вместе с проходом сюда переедет закрытие
    // предыдущего — и вызывающим об этом знать не нужно.
    void BindRenderTarget(const VulkanRenderTarget* target);

    // Текущая шейдерная программа. Как и цель отрисовки, пока запоминается:
    // конвейер по ней ищется в момент вызова отрисовки.
    void BindShader(const VulkanShaderProgram* shader);

    // Единая точка отрисовки: сюда сходятся все шесть вариантов Draw* из
    // Geometry. Здесь ищется конвейер под текущее состояние, собирается набор
    // дескрипторов и пишутся команды.
    //
    // topology передаётся отдельно, потому что линии (DebugDraw, сетка) — это
    // не флаг состояния в RHI, а отдельный вызов отрисовки; в Vulkan же
    // топология запечена в конвейер, то есть входит в ключ кэша.
    void Draw(const VulkanGeometry& geometry, VkPrimitiveTopology topology, uint32_t vertexCount,
              uint32_t indexCount, uint32_t firstIndex, uint32_t instanceCount);

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

    struct SamplerKeyHash {
        size_t operator()(const SamplerKey& k) const {
            return ((size_t)k.FilterMode << 3) ^ ((size_t)k.WrapMode << 2) ^ ((size_t)k.Mips << 1) ^
                   (size_t)k.WhiteBorder;
        }
    };
    std::unordered_map<SamplerKey, VkSampler, SamplerKeyHash> m_samplers;
    std::unordered_map<uint64_t, TextureBinding> m_textures;
    uint64_t m_nextTextureHandle = 1; // 0 зарезервирован под «невалидный»

    const VulkanRenderTarget* m_target = nullptr;
    const VulkanShaderProgram* m_shader = nullptr;

    // Один командный буфер и один барьер ожидания. Не «кадры в полёте»: пока
    // бэкенд рисует только в offscreen-таргеты и отдаёт результат чтением
    // пикселей, параллелить нечего — а неверная синхронизация в такой схеме
    // ловится не тестом, а чужой видеокартой через полгода. Кольцо появится
    // вместе со swapchain, где оно и нужно.
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;
    bool m_recording = false;
    bool m_inPass = false;

    // --- Кэш конвейеров ----------------------------------------------------
    struct PipelineKey {
        VkShaderModule Vertex = VK_NULL_HANDLE;
        VkShaderModule Fragment = VK_NULL_HANDLE;
        VkPipelineLayout Layout = VK_NULL_HANDLE;
        VkRenderPass Pass = VK_NULL_HANDLE;
        size_t LayoutHash = 0;
        VkPrimitiveTopology Topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkSampleCountFlagBits Samples = VK_SAMPLE_COUNT_1_BIT;
        uint32_t StateBits = 0;   // упакованное State (см. PackState)
        bool operator==(const PipelineKey& o) const {
            return Vertex == o.Vertex && Fragment == o.Fragment && Layout == o.Layout &&
                   Pass == o.Pass && LayoutHash == o.LayoutHash && Topology == o.Topology &&
                   Samples == o.Samples && StateBits == o.StateBits;
        }
    };
    struct PipelineKeyHash {
        size_t operator()(const PipelineKey& k) const {
            size_t h = std::hash<void*>{}((void*)k.Vertex);
            auto mix = [&h](size_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
            mix(std::hash<void*>{}((void*)k.Fragment));
            mix(std::hash<void*>{}((void*)k.Pass));
            mix(k.LayoutHash);
            mix((size_t)k.Topology);
            mix((size_t)k.Samples);
            mix(k.StateBits);
            return h;
        }
    };
    uint32_t PackState() const;
    VkPipeline PipelineFor(const PipelineKey& key, const VulkanGeometry& geometry);
    // Набор дескрипторов под текущий шейдер: буфер униформ + связанные текстуры.
    VkDescriptorSet DescriptorSetFor(const VulkanShaderProgram& shader);
    // Кусок кольца униформ под один вызов. false — места не нашлось и добавить
    // блок не вышло.
    bool ReserveUniforms(VkDeviceSize size, const void* data, VkDescriptorBufferInfo& out);
    // Пустой набор из текущего пула; при исчерпании заводит следующий пул.
    VkDescriptorSet AllocateDescriptorSet(VkDescriptorSetLayout layout);
    bool AddDescriptorPool();
    bool AddUniformBlock();

    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> m_pipelines;

    // Кадровые распределители — цепочки блоков, а не по одному буферу на всё.
    //
    // Один блок фиксированного размера означает жёсткий потолок на кадр: как
    // только он кончится, вызовы отрисовки придётся выбрасывать, и кадр молча
    // станет неправильным. Сколько именно нужно, заранее не знает никто:
    // отражения, каскады теней и зонды умножают число вызовов на порядок, и
    // зависит это от сцены, а не от движка. Поэтому блоки добавляются по
    // требованию и НЕ освобождаются между кадрами — за несколько кадров цепочка
    // дорастает до нужной длины и перестаёт расти.
    static constexpr VkDeviceSize kUniformBlockBytes = 8u << 20;
    static constexpr uint32_t kDescriptorSetsPerPool = 4096;

    struct DescriptorPools {
        std::vector<VkDescriptorPool> Blocks;
        size_t Current = 0;   // из какого блока сейчас берём
    };
    DescriptorPools m_descriptors;

    // Кольцо униформ кадра: каждый вызов отрисовки получает свой кусок. Один
    // буфер на всех означал бы, что значения следующего вызова затирают
    // значения предыдущего ДО того, как GPU его исполнил.
    std::vector<std::unique_ptr<VulkanBuffer>> m_uniformBlocks;
    size_t m_uniformBlock = 0;
    VkDeviceSize m_uniformOffset = 0;
    VkDeviceSize m_uniformAlignment = 256;

    // Что привязано к текстурным юнитам. В OpenGL это состояние держал драйвер;
    // в Vulkan привязка живёт в наборе дескрипторов, который собирается в
    // момент отрисовки — значит до тех пор её надо где-то помнить.
    //
    // 16 юнитов — минимум, гарантированный спецификацией GL 3.3
    // (GL_MAX_TEXTURE_IMAGE_UNITS), и движок больше не использует.
    static constexpr int kMaxTextureUnits = 16;
    TextureHandle m_boundTextures[kMaxTextureUnits]{};

    State m_state;
    float m_clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    int m_viewport[4] = {0, 0, 0, 0};
    bool m_scissorOn = false;
    int m_scissor[4] = {0, 0, 0, 0};
    float m_maxAnisotropy = 1.0f;
    bool m_depthClipControl = false;
    std::string m_apiVersion = "Vulkan (не инициализирован)";
};

} // namespace sage::rhi
