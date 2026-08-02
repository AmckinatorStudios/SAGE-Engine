#pragma once
#include <string>
#include <vector>

#include "sage/rhi/GraphicsDevice.h"

// ---------------------------------------------------------------------------
// Null-бэкенд: реализация RHI, которая ничего не рисует.
//
// ЧТО ОН ДОКАЗЫВАЕТ. Что граница RHI держится не на честном слове. Движок
// собирается и работает с устройством, за которым нет ни OpenGL, ни какого-либо
// другого драйвера, — значит нигде за пределами engine/src/rhi/ на OpenGL не
// рассчитывают. Утверждение «мы не трогаем GL напрямую» перестаёт быть
// заявлением и становится проверяемым фактом.
//
// ЧЕГО ОН НЕ ДОКАЗЫВАЕТ, И ЭТО ВАЖНЕЕ. Он НЕ доказывает переносимости на
// Vulkan. Null выдержан в той же GL-подобной форме («выставь состояние, потом
// рисуй»), потому что реализует тот же интерфейс. Пустышка соглашается с любой
// формой — ей нечего возразить. Настоящий Vulkan-бэкенд возразил бы: ему нужны
// командные буферы, проходы и дескрипторы, а их в этом интерфейсе нет.
//
// ЗАЧЕМ ОН ЕЩЁ. Тестам и инструментам, которым нужна сцена, но не картинка:
// проверка загрузки ассетов, сериализации, анимации. С Null они гоняются на
// машине без видеокарты и без xvfb.
//
// СЧЁТЧИКИ. Устройство считает вызовы отрисовки и созданные ресурсы. Это
// превращает его ещё и в измерительный прибор: «сколько вызовов делает эта
// сцена» — вопрос, на который иначе отвечать нечем.
// ---------------------------------------------------------------------------
namespace sage::rhi {

struct NullCounters {
    int DrawCalls = 0;
    int Shaders = 0;
    int Geometries = 0;
    int Textures = 0;
    int RenderTargets = 0;
    int StateChanges = 0;
};

class NullDevice : public GraphicsDevice {
public:
    void Init(ProcLoader loader) override;
    const char* BackendName() const override { return "Null"; }
    std::string ApiVersion() const override { return "Null 1.0 (без графического API)"; }

    void SetViewport(int x, int y, int width, int height) override;
    void SetClearColor(float r, float g, float b, float a) override;
    void Clear(bool color = true, bool depth = true) override;
    void BindDefaultFramebuffer() override;

    void SetBlend(bool enabled) override;
    void SetDepthTest(bool enabled) override;
    void SetDepthWrite(bool enabled) override;
    void SetDepthFunc(DepthFunc func) override;
    void SetCullMode(CullMode mode) override;
    void SetPolygonMode(PolygonMode mode) override;
    void SetScissor(bool enabled, int x = 0, int y = 0, int w = 0, int h = 0) override;
    void SetSRGBWrite(bool enabled) override;
    void BindTexture2D(int unit, sage::rhi::TextureHandle texture) override;
    void ReadPixelsRGB(int x, int y, int width, int height, unsigned char* out) override;
    float MaxAnisotropy() override { return 1.0f; }

    std::unique_ptr<ShaderProgram> CreateShaderProgram(const std::string& vertexSrc,
                                                       const std::string& fragmentSrc) override;
    std::unique_ptr<Geometry> CreateGeometry(const VertexLayout& layout) override;
    std::unique_ptr<Texture2D> CreateTexture2D(const Texture2DDesc& desc, const void* pixels) override;
    std::unique_ptr<Texture3D> CreateTexture3D(const Texture3DDesc& desc, const float* pixels) override;
    std::unique_ptr<TextureCube> CreateTextureCube(const CubeFacePixels faces[6]) override;
    std::unique_ptr<RenderTarget> CreateRenderTarget(const RenderTargetDesc& desc) override;
    std::unique_ptr<CubeRenderTarget> CreateCubeRenderTarget(
        const CubeRenderTargetDesc& desc) override;

    // Счётчики этого устройства. Общие на процесс: ресурсы создаются из
    // разных мест, и таскать ссылку на устройство ради счётчика незачем.
    static NullCounters& Counters();
    static void ResetCounters();
};

} // namespace sage::rhi
