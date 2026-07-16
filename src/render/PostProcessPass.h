#pragma once
#include "RenderContext.h"
#include "Framebuffer.h"
#include "PostProcess.h"
#include "Shader.h"
#include "../asset/AssetManager.h"
#include <glad/glad.h>

// ---------------------------------------------------------------------
// PostProcessPass — HDR-пингпонг: Begin() готовит offscreen float-буфер под
// сцену этого кадра (или, если пост-процесс выключен, просто чистит текущий
// целевой буфер — эффективно пропуская HDR-этап), Resolve() — финальный
// fullscreen tonemap-проход на экран.
//
// Это НЕ RenderPass в обычном смысле: между Begin() и Resolve() вызывающий
// код (RenderPipeline с остальными проходами/CallbackPass) рисует саму
// сцену — PostProcessPass не пытается "обернуть" их сам, только даёт два
// метода, которые оборачивающий код (main.cpp) зовёт по краям сцены,
// например через два CallbackPass в общем списке пайплайна:
//
//   pipeline.Add<CallbackPass>([&](RenderContext& ctx) { post.Begin(ctx); });
//   pipeline.Add<CallbackPass>(sceneContentLambda);
//   pipeline.Add<CallbackPass>([&](RenderContext& ctx) { post.Resolve(ctx); });
//
// Часть ЯДРА рендера — не зависит от вокселей/конкретной игры.
// ---------------------------------------------------------------------
class PostProcessPass {
public:
    PostProcessSettings Settings;

    PostProcessPass(int width, int height) : m_fbo(width, height) {
        m_shader = AssetManager::Instance().LoadShader("shaders/post.vert", "shaders/post.frag");
    }

    void Begin(RenderContext& ctx) {
        m_fbo.Resize(ctx.ScreenWidth, ctx.ScreenHeight);
        if (ctx.PostEnabled) m_fbo.Bind();
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    void Resolve(RenderContext& ctx) {
        if (!ctx.PostEnabled || !m_shader.IsReady()) return;

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, ctx.ScreenWidth, ctx.ScreenHeight);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        Shader& shader = *m_shader;
        shader.Use();
        shader.SetFloat("uExposure", Settings.Exposure);
        shader.SetFloat("uGamma", Settings.Gamma);
        shader.SetFloat("uSaturation", Settings.Saturation);
        shader.SetFloat("uContrast", Settings.Contrast);
        shader.SetFloat("uVignette", Settings.VignetteStrength);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_fbo.ColorTexture());
        shader.SetInt("uScene", 0);
        m_post.Draw();
    }

private:
    Framebuffer m_fbo;
    PostProcess m_post;
    Asset<Shader> m_shader;
};
