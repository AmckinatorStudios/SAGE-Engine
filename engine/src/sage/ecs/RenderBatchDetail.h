#pragma once
#include "sage/ecs/RenderBatch.h"
#include "sage/anim/AnimationComponents.h"
#include "sage/core/Profiler.h"
#include <algorithm>
#include <string>
#include "sage/core/JobSystem.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/render/Frustum.h"
#include "sage/render/LightingUpload.h"
#include "sage/render/Material.h"
#include "sage/render/PbrShader.h"
#include "sage/render/Shader.h"
#include "sage/render/Texture.h"
#include "sage/gi/GI.h"
#include "sage/gi/GIUpload.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Components.h"
#include "sage/scene/Light.h"
#include "sage/scene/Scene.h"

// Шейдеры и помощники батча, общие для сбора и цветового прохода
// (RenderBatch.cpp) и цветового прохода (RenderBatch_Color.cpp).
namespace sage::ecs::batch_detail {

extern const char* kLitVert;
extern const char* kDepthInstVert;
extern const char* kDepthUModelVert;
extern const char* kDepthFrag;
extern const char* kDepthAlphaFrag;
std::string LitFragSource();
std::string TexFragSource();
void UploadTiling(Shader& sh, const MaterialRender& render);
void UploadParams(Shader& sh, const std::unordered_map<std::string, ShaderParam>& params);
Shader& LitShader();
Shader& TexShader();
Shader& DepthInstShader();
Shader& DepthUModelShader();
Shader& DepthAlphaShader();
bool CutsByAlpha(const Material* mat);

} // namespace sage::ecs::batch_detail
