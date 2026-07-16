#pragma once
#include "RenderPass.h"
#include "RenderContext.h"
#include "ShadowMap.h"
#include "Shader.h"
#include "../asset/AssetManager.h"
#include <functional>

// ---------------------------------------------------------------------
// ShadowPass — движковый проход карты теней от направленного света (см.
// ShadowMap: depth-проход из точки зрения солнца + PCF-приёмник в шейдерах
// сцены). Сам ShadowPass не знает, ЧТО отбрасывает тень — это задаёт
// DrawCasters, колбэк, который выставляет игра (например «нарисовать все
// чанки воксельного мира + все объекты сцены»). После своего Execute
// публикует готовую карту в ctx.Shadows — последующие проходы читают её
// оттуда, не держа собственную ссылку на ShadowPass.
//
// Часть ЯДРА рендера — не зависит от вокселей/конкретной игры.
// ---------------------------------------------------------------------
class ShadowPass : public RenderPass {
public:
    using CasterCallback = std::function<void(Shader&, RenderContext&)>;

    explicit ShadowPass(int resolution = 2048) : m_shadows(resolution) {
        m_depthShader = AssetManager::Instance().LoadShader(
            "shaders/shadow_depth.vert", "shaders/shadow_depth.frag");
    }

    // Рисует отбрасывающую тень геометрию текущего кадра. depthShader уже
    // активен (Use() вызван), uLightSpace уже выставлен — колбэку остаётся
    // только пройтись по геометрии и вызвать SetMat4("uModel", ...) + Draw().
    CasterCallback DrawCasters;

    void Execute(RenderContext& ctx) override {
        if (!ctx.ShadowsEnabled || !m_depthShader.IsReady()) {
            ctx.Shadows = nullptr;
            return;
        }
        m_shadows.SetLightMatrix(ctx.Lighting->Sun.Direction, ctx.ShadowCenter, ctx.ShadowRadius);
        m_shadows.BeginRender();

        Shader& depthShader = *m_depthShader;
        depthShader.Use();
        depthShader.SetMat4("uLightSpace", m_shadows.LightMatrix());
        if (DrawCasters) DrawCasters(depthShader, ctx);

        m_shadows.EndRender(ctx.ScreenWidth, ctx.ScreenHeight);
        ctx.Shadows = &m_shadows;
    }

    int Resolution() const { return m_shadows.Resolution(); }

private:
    ShadowMap m_shadows;
    Asset<Shader> m_depthShader;
};
