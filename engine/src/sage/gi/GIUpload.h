#pragma once
#include "sage/gi/GI.h"
#include "sage/render/Mesh.h"
#include "sage/render/Shader.h"
#include "sage/rhi/GraphicsDevice.h"

// ---------------------------------------------------------------------------
// Runtime-часть GI: ленивое создание GPU-ресурсов из CPU-данных бейка и
// заливка uniform'ов общего PBR-блока (см. kPbrSharedGlsl). Всё, что здесь, —
// требует активного GL-контекста и зовётся ТОЛЬКО из кода рендера (RenderBatch
// и пр.); сам бейк и сериализация к GPU не прикасаются.
//
// Юниты текстур GI фиксированы и не пересекаются с материальными (0..5 у
// текстурного PBR-пути): 6 — 3D-объём проб (три канала слоями), 9 — лайтмапа.
// ---------------------------------------------------------------------------
namespace sage::gi {

constexpr int kGIVolumeUnit = 6;
constexpr int kLightmapUnit = 9;

// GPU-меш сущности с лайтмап-UV (создаётся при первом рендере после бейка).
inline ::Mesh* GetOrCreateGpuMesh(EntityBake& eb) {
    // Разметка по материалам переносится из исходной геометрии: запечённый меш
    // отличается от неё лайтмап-развёрткой, а не составом частей, и потерять
    // здесь границы означало бы, что запечённая многоматериальная модель
    // красится одним материалом.
    if (!eb.GpuMesh && !eb.Mesh.Empty())
        eb.GpuMesh = std::make_shared<::Mesh>(eb.Mesh.Vertices, eb.Mesh.Indices, eb.Mesh.Submeshes);
    return eb.GpuMesh.get();
}

// HDR-текстура страницы атласа (билинейно, clamp, без мипмапов — атлас).
inline void EnsureLightmapGpu(LightmapPage& p) {
    if (p.Gpu || p.Size <= 0 || p.Texels.empty()) return;
    sage::rhi::Texture2DDesc d;
    d.Width = d.Height = p.Size;
    d.Channels = 3;
    d.FloatPixels = true;
    d.FilterMode = sage::rhi::Filter::Bilinear;
    d.WrapMode = sage::rhi::Wrap::ClampEdge;
    d.GenerateMipmaps = false;
    p.Gpu = sage::rhi::GraphicsDevice::Get().CreateTexture2D(d, p.Texels.data());
}

// 3D-текстура объёма проб: по глубине три слоя — R, G, B (каждый Dims.z
// срезов, в текселе vec4 гармоник канала). Слои не смешиваются при выборке:
// шейдер берёт z строго между центрами крайних срезов слоя (EvalGIVolume).
inline void EnsureVolumeGpu(ProbeVolume& v) {
    if (v.Gpu || !v.Valid() || v.Probes.empty()) return;
    const size_t n = v.Probes.size();
    std::vector<glm::vec4> rgb(n * 3);
    for (size_t i = 0; i < n; ++i) {
        rgb[i] = v.Probes[i].R;
        rgb[n + i] = v.Probes[i].G;
        rgb[2 * n + i] = v.Probes[i].B;
    }
    sage::rhi::Texture3DDesc d;
    d.Width = v.Dims.x; d.Height = v.Dims.y; d.Depth = v.Dims.z * 3;
    d.Channels = 4;
    v.Gpu = sage::rhi::GraphicsDevice::Get().CreateTexture3D(d, (const float*)rgb.data());
}

// Назначает шейдеру юниты сэмплеров GI. ОБЯЗАТЕЛЬНО для каждого шейдера с
// kPbrSharedGlsl, даже когда GI нет: иначе sampler3D-униформы остаются на
// юните 0 вместе с sampler2D материала — конфликт типов на одном юните
// (GL_INVALID_OPERATION на части драйверов).
inline void SetGISamplerUnits(Shader& sh) {
    sh.SetInt("uGIVol", kGIVolumeUnit);
    sh.SetInt("uLightmap", kLightmapUnit);
}

// Заливает объём проб (или выключает его) + назначает юниты. Зовётся на
// настройке шейдера кадра. gi может быть nullptr.
inline void UploadGIVolume(Shader& sh, GIState* gi) {
    SetGISamplerUnits(sh);
    ProbeVolume* vol = (gi && gi->Probes.Valid() && !gi->Probes.Probes.empty()) ? &gi->Probes : nullptr;
    if (vol) EnsureVolumeGpu(*vol);
    if (!vol || !vol->Gpu) {
        sh.SetInt("uGIVolumeEnabled", 0);
        return;
    }
    vol->Gpu->Bind(kGIVolumeUnit);
    sh.SetInt("uGIVolumeEnabled", 1);
    sh.SetVec3("uGIVolMin", vol->Min);
    glm::vec3 extent = vol->CellSize * glm::vec3(vol->Dims - glm::ivec3(1));
    sh.SetVec3("uGIVolInvExtent", glm::vec3(1.0f) / glm::max(extent, glm::vec3(1e-4f)));
    sh.SetVec3("uGIVolDims", glm::vec3(vol->Dims));
}

} // namespace sage::gi
