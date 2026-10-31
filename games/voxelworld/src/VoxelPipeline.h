#pragma once
#include <memory>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include "sage/render/RenderPipeline.h"
#include "sage/render/ShaderLibrary.h"
#include "sage/render/ShaderMaterial.h"

#include "World.h"

// ---------------------------------------------------------------------------
// VoxelPipeline — свой рендерер блочного мира на системе рендеринга SAGE.
//
// Ни одной строки штатного PBR: свой свет (уровни блоков -> карта света),
// своя геометрия (упакованные меши чанков), свои шейдеры, своя
// пост-обработка. Кадр — граф проходов (RenderGraph):
//
//   Меши чанков      свой     пересборка изменённых чанков в GPU-буферы
//   Карта света      вычисл.  16×16 «уровень -> цвет» (или на CPU без compute)
//   Небо             графика  градиент, квадратные солнце и луна, облака
//   Чанки            графика  глухие блоки и листва (вырез по альфе)
//   Вода и стекло    графика  смешивание, чанки от дальних к ближним
//   Рамка блока      графика  контур блока под прицелом
//   Пост: …          цепочка  подводный эффект (если в воде), тон и виньетка
//
// Почему карта света. Уровни света — целые 0..15, а цвет зависит от времени
// суток и мерцания: перекрасить 256 текселей дешевле, чем пересобирать мир.
// ---------------------------------------------------------------------------
namespace voxel {

// Параметры карты света на кадр — общие для вычислительного шейдера и
// запасного пути на CPU (без compute: macOS, старый OpenGL).
struct LightmapParams {
    float Daylight = 1.0f;
    glm::vec3 SkyTint{1.0f};
    float Flicker = 1.0f;
    float Gamma = 0.5f;
};
// RGBA8, 16×16, строка y = свой уровень. Та же формула, что lightmap.comp.
void ComputeLightmapCpu(const LightmapParams& p, uint8_t out[16 * 16 * 4]);

struct VoxelFrame {
    float TimeOfDay = 0.3f; // 0..1, 0.25 — полдень
    bool Underwater = false;
    bool HasSelection = false;
    glm::ivec3 Selection{0};
};

class VoxelPipeline : public sage::render::RenderPipeline {
public:
    static constexpr const char* kId = "game.voxel";

    explicit VoxelPipeline(World& world);
    ~VoxelPipeline() override;

    std::string Id() const override { return kId; }
    bool Initialize(sage::rhi::GraphicsDevice& device, std::string& error) override;
    void Build(sage::render::RenderGraph& graph, const sage::render::RenderView& view) override;
    void Shutdown() override;

    VoxelFrame Frame;
    int RenderDistance = 8;       // чанков
    int MeshBudgetPerFrame = 24;  // пересборок за кадр (первый кадр — все)
    bool ForceCpuLightmap = false;

    sage::render::ShaderLibrary& Shaders() { return m_shaders; }
    bool UsesComputeLightmap() const { return m_compute && !ForceCpuLightmap; }
    int ChunksDrawn() const { return m_chunksDrawn; }
    int FacesDrawn() const { return m_facesDrawn; }
    int PendingMeshes() const;
    // Пересобрать меши немедленно (для тестов и первого кадра).
    void RebuildAllMeshes();

    // Цвет неба у горизонта — туман и интерфейс берут его же.
    glm::vec3 HorizonColor() const;
    sage::rhi::TextureHandle AtlasHandle() const;

private:
    struct GpuChunk {
        std::unique_ptr<sage::rhi::Geometry> Opaque, Translucent;
        size_t OpaqueIndices = 0, TranslucentIndices = 0;
        int Faces = 0;
        glm::vec3 Center{0.0f};
    };
    void UploadChunk(int index);
    void SkyColors(glm::vec3& zenith, glm::vec3& horizon, glm::vec3& sunDir, float& daylight) const;
    LightmapParams Lightmap(float time) const;

    World& m_world;
    sage::render::ShaderLibrary m_shaders;
    std::optional<sage::render::ShaderMaterial> m_chunkMat, m_waterMat, m_skyMat, m_outlineMat;
    std::optional<sage::render::ShaderMaterial> m_underwaterFx, m_finalFx;
    std::unique_ptr<sage::rhi::Texture2D> m_atlas;
    std::unique_ptr<sage::rhi::Texture2D> m_lightmap;
    std::unique_ptr<sage::rhi::Geometry> m_outline;
    std::vector<GpuChunk> m_gpu;
    bool m_compute = false;
    int m_chunksDrawn = 0;
    int m_facesDrawn = 0;
};

} // namespace voxel
