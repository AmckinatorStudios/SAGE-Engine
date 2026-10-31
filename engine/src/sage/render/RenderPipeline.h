#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/render/RenderGraph.h"

class Scene;
class Framebuffer;

namespace sage::rhi {
class GraphicsDevice;
}

// ---------------------------------------------------------------------------
// RenderPipeline — рендерер как ПОДКЛЮЧАЕМАЯ ЧАСТЬ, а не основа движка.
//
// Рендерер решает весь кадр: какие проходы, в каком порядке, какие цели, как
// считается свет и чем заканчивается картинка. Движок даёт ему вид (камера,
// размер, сцена, время), граф кадра (RenderGraph.h), библиотеку шейдеров
// (ShaderLibrary.h), материалы своих шейдеров (ShaderMaterial.h) и прямой
// доступ к устройству — и больше ничего не навязывает.
//
// Встроенный PBR — ОДНА ИЗ реализаций (StandardPipeline.h, "sage.pbr"): она
// зарегистрирована тем же реестром и тем же способом, что и рендерер игры.
// Воксельный мир со светом по блокам, рисованный мир без освещения,
// трассировщик в вычислительном шейдере — всё это тот же интерфейс.
//
// ТРИ СТЕПЕНИ СВОБОДЫ, по возрастанию:
//   1. Дополнить чужой рендерер: PipelineRenderer::AddExtension вставляет свои
//      проходы до/после штатных по имени, выключает или заменяет их.
//   2. Унаследовать встроенный и переопределить его этапы (виртуальные
//      AddShadowPasses/AddScenePasses/AddPostPasses у StandardPipeline).
//   3. Написать свой с нуля: наследник RenderPipeline.
// ---------------------------------------------------------------------------
namespace sage::render {

// Что рендерер получает на кадр.
struct RenderView {
    glm::mat4 View{1.0f};
    glm::mat4 Proj{1.0f};
    glm::vec3 Position{0.0f};
    int Width = 1;  // размер выхода (экранного прямоугольника или цели)
    int Height = 1;
    int ViewportX = 0; // левый нижний угол прямоугольника на экране
    int ViewportY = 0;
    float Time = 0.0f;      // время сцены, секунды
    float DeltaTime = 0.0f; // длительность прошлого кадра
    long long Frame = 0;
    Scene* World = nullptr;         // сцена ECS (может не быть: мир игры свой)
    Framebuffer* Output = nullptr;  // nullptr — экран
};

class RenderPipeline {
public:
    virtual ~RenderPipeline() = default;
    // Идентификатор ("sage.pbr", "game.voxel").
    virtual std::string Id() const = 0;

    // Раз, когда устройство готово: шейдеры, постоянные буферы и текстуры.
    // false — рендерер на этой машине работать не может (причина в error):
    // исполнитель не зовёт Build и сообщает причину, а не рисует чёрное.
    virtual bool Initialize(sage::rhi::GraphicsDevice& device, std::string& error) {
        (void)device;
        (void)error;
        return true;
    }

    // Каждый кадр: объявить проходы в graph. Последний проход пишет
    // graph.Output(). Только ОБЪЯВЛЕНИЕ: рисуют тела проходов при Execute.
    virtual void Build(RenderGraph& graph, const RenderView& view) = 0;

    virtual void Shutdown() {}
};

// --- Реестр рендереров -------------------------------------------------------

struct PipelineInfo {
    std::string Id;
    std::string Label; // подпись для человека
    std::string Description;
    std::function<std::unique_ptr<RenderPipeline>()> Create;
};

class RenderPipelineRegistry {
public:
    // Повторная регистрация того же Id ЗАМЕНЯЕТ запись — так игра подменяет
    // встроенный рендерер своим под тем же именем.
    static void Register(PipelineInfo info);
    static const PipelineInfo* Find(const std::string& id);
    static std::vector<PipelineInfo> All();
    static std::unique_ptr<RenderPipeline> Create(const std::string& id);
    // Регистрирует встроенные ("sage.pbr"). Идемпотентна; зовётся сама при
    // первом обращении к реестру.
    static void RegisterBuiltins();
};

// Дополнение чужого рендерера: зовётся после его Build, может добавить,
// вставить, выключить проходы.
using GraphExtension = std::function<void(RenderGraph&, const RenderView&)>;

// --- Исполнитель -------------------------------------------------------------
//
// Держит рендерер и его граф, собирает и выполняет кадр. Ошибку описания кадра
// (читают то, во что никто не писал) не прячет: кадр не рисуется, причина — в
// LastError() и один раз в лог, а не каждый кадр.
class PipelineRenderer {
public:
    PipelineRenderer() = default;
    explicit PipelineRenderer(std::unique_ptr<RenderPipeline> pipeline,
                              sage::rhi::GraphicsDevice* device = nullptr);
    ~PipelineRenderer();

    // Сменить рендерер на лету. Старый получает Shutdown, пул целей
    // освобождается.
    bool SetPipeline(std::unique_ptr<RenderPipeline> pipeline);
    RenderPipeline* Pipeline() const { return m_pipeline.get(); }
    bool Ready() const { return m_ready; }

    void AddExtension(const std::string& name, GraphExtension extension);
    void RemoveExtension(const std::string& name);

    bool Render(const RenderView& view);

    RenderGraph& Graph() { return m_graph; }
    const RenderGraph& Graph() const { return m_graph; }
    const std::string& LastError() const { return m_error; }

private:
    sage::rhi::GraphicsDevice& Dev() const;

    sage::rhi::GraphicsDevice* m_device = nullptr;
    std::unique_ptr<RenderPipeline> m_pipeline;
    RenderGraph m_graph;
    std::vector<std::pair<std::string, GraphExtension>> m_extensions;
    bool m_ready = false;
    std::string m_error;
    std::string m_reportedError;
};

} // namespace sage::render
