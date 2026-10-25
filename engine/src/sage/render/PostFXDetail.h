#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sage/render/PostEffect.h"
#include "sage/render/Shader.h"
#include "sage/rhi/GraphicsDevice.h"

// ---------------------------------------------------------------------------
// Внутренняя кухня пост-обработки, общая для файлов PostFX*.cpp.
//
// PostFX.cpp был одним файлом на две с половиной тысячи строк: шейдеры,
// звенья, каталог видов и сам тракт. Правка тон-маппинга пересобирала SSAO, а
// «где рисуется глубина резкости» искали поиском. Теперь по файлу на
// обязанность:
//   PostFXShaders_Light.cpp — шейдеры освещения и оптики (AO, GI, свечение,
//                             резкость, смаз);
//   PostFXShaders_Color.cpp — шейдеры цвета (FXAA, экспозиция, тон, плёнка) и
//                             ленивые программы всех шейдеров;
//   PostFXPasses.cpp        — звенья тракта (Run*) и их помощники;
//   PostFXCatalog.cpp       — встроенные виды эффектов и готовые цепочки;
//   PostFX.cpp              — сам тракт: буферы, порядок, самопроверка.
// Заголовок внутренний: снаружи пост-обработку видят через PostFX.h и
// PostEffect.h.
// ---------------------------------------------------------------------------
namespace sage::render::postfx {

using sage::rhi::GraphicsDevice;
using sage::rhi::RenderTarget;

// Исходники шейдеров освещения (PostFXShaders_Light.cpp).
extern const char* kFsVert;
extern const char* kSsaoFrag;
extern const char* kAoBlurFrag;
extern const char* kSsgiFrag;
extern const char* kSsgiBlurFrag;
extern const char* kSsgiApplyFrag;
extern const char* kBrightFrag;
extern const char* kBlurFrag;
extern const char* kDofCommon;
extern const char* kDofPrepFrag;
extern const char* kDofNearMaxFrag;
extern const char* kDofGatherFrag;
extern const char* kDofCompositeFrag;
extern const char* kMotionFrag;

// Программы шейдеров, помощники и звенья.
Shader& SsaoShader();
Shader& AoBlurShader();
Shader& SsgiShader();
Shader& SsgiBlurShader();
Shader& SsgiApplyShader();
std::string DofSource(const char* body);
Shader& DofPrepShader();
Shader& DofNearMaxShader();
Shader& DofGatherShader();
Shader& DofCompositeShader();
Shader& MotionShader();
Shader& BrightShader();
Shader& BlurShader();
Shader& ExposureShader();
Shader& AutoExposureMeterShader();
Shader& AutoExposureApplyShader();
Shader& ColorShader();
Shader& GradeShader();
Shader& TonemapShader();
Shader& VignetteShader();
Shader& ChromaticShader();
Shader& GrainShader();
Shader& AddShader();
Shader& MulShader();
Shader& FxaaShader();
Shader& CopyShader();
std::unique_ptr<RenderTarget> MakeColor(int w, int h);
GraphicsDevice& Dev(PostContext& ctx);
void DrawFullscreen(PostContext& ctx);
void OnceLog(const std::string& message);
RenderTarget* Keep(std::unique_ptr<RenderTarget>& slot, int w, int h);
PostParamDesc ColorParam(const char* name, const char* label, const char* hint);
PostParamDesc BoolParam(const char* name, const char* label, bool def, const char* hint);
PostParamDesc Heading(const char* label);
void RunAmbientOcclusion(PostContext& ctx, const PostEffect& e);
void RunScreenSpaceGI(PostContext& ctx, const PostEffect& e);
void RunDepthOfField(PostContext& ctx, const PostEffect& e);
void RunMotionBlur(PostContext& ctx, const PostEffect& e);
void RunLensFlare(PostContext& ctx, const PostEffect& e);
void RunBloom(PostContext& ctx, const PostEffect& e);
void RunExposure(PostContext& ctx, const PostEffect& e);
void RunAutoExposure(PostContext& ctx, const PostEffect& e);
void RunColor(PostContext& ctx, const PostEffect& e);
void RunGrading(PostContext& ctx, const PostEffect& e);
void RunTonemap(PostContext& ctx, const PostEffect& e);
void RunVignette(PostContext& ctx, const PostEffect& e);
void RunChromatic(PostContext& ctx, const PostEffect& e);
void RunGrain(PostContext& ctx, const PostEffect& e);
void RunFxaa(PostContext& ctx, const PostEffect& e);
PostParamDesc Param(const char* name, const char* label, float def, float min, float max, const char* hint);
PostParamDesc EnumParam(const char* name, const char* label, int def, std::vector<std::string> options, const char* hint);
void RunSimplePass(PostContext& ctx, Shader& shader, bool ldrOut, const std::function<void(Shader&)>& setup);

} // namespace sage::render::postfx
