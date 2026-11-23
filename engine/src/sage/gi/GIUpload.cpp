#include "sage/gi/GIUpload.h"

namespace sage::gi {

namespace {
void UploadForRender(Shader& sh, GIState* gi) { UploadGIVolume(sh, gi); }
} // namespace

GIRenderHook::GIRenderHook() { sage::render::SetGIVolumeUploader(&UploadForRender); }

} // namespace sage::gi
