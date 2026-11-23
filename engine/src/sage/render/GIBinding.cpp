#include "sage/render/GIBinding.h"

#include <atomic>

#include "sage/render/Shader.h"

namespace sage::render {

namespace {
// atomic: GIState создаётся и в фоновом потоке бейка, а читает слот кадр.
std::atomic<GIVolumeUploader> g_uploader{nullptr};
} // namespace

void SetGISamplerUnits(Shader& sh) {
    sh.SetInt("uGIVol", kGIVolumeUnit);
    sh.SetInt("uLightmap", kLightmapUnit);
}

void SetGIVolumeUploader(GIVolumeUploader fn) { g_uploader.store(fn, std::memory_order_release); }

void UploadGIVolume(Shader& sh, sage::gi::GIState* gi) {
    if (GIVolumeUploader fn = g_uploader.load(std::memory_order_acquire)) {
        fn(sh, gi);
        return;
    }
    SetGISamplerUnits(sh);
    sh.SetInt("uGIVolumeEnabled", 0);
}

} // namespace sage::render
