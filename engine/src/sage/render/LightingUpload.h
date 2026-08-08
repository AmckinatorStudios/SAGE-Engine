#pragma once
#include "sage/scene/Light.h"
#include "sage/render/ShadowMap.h"
#include "Shader.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <string>

// Заливает в шейдер все uniform'ы, описывающие освещение сцены: фоновую
// засветку, солнце и точечные источники. Любые шейдеры-приёмники, ожидающие
// одинаковый набор имён uniform'ов (uAmbient*, uSun*, uPointLights[]),
// используют один и тот же вызов — не нужно дублировать имена на каждый шейдер.
inline void UploadLighting(Shader& shader, const LightingEnvironment& env) {
    // Ambient берётся из скайбокса, если он включён (см. ResolveAmbient) —
    // освещение согласовано с видимым небом.
    glm::vec3 ambientSky, ambientGround;
    env.ResolveAmbient(ambientSky, ambientGround);
    shader.SetVec3("uAmbientSky", ambientSky);
    shader.SetVec3("uAmbientGround", ambientGround);
    shader.SetFloat("uAmbientStrength", env.AmbientStrength);

    shader.SetVec3("uSunDir", env.Sun.Direction);
    shader.SetVec3("uSunColor", env.Sun.Color);
    shader.SetFloat("uSunIntensity", env.Sun.Intensity);

    int count = std::min(static_cast<int>(env.PointLights.size()), LightingEnvironment::MaxPointLights);
    shader.SetInt("uNumPointLights", count);
    for (int i = 0; i < count; ++i) {
        const PointLight& light = env.PointLights[i];
        std::string prefix = "uPointLights[" + std::to_string(i) + "].";
        shader.SetVec3(prefix + "position", light.Position);
        shader.SetVec3(prefix + "color", light.Color);
        shader.SetFloat(prefix + "intensity", light.Intensity);
        shader.SetFloat(prefix + "constant", light.Constant());
        shader.SetFloat(prefix + "linear", light.Linear());
        shader.SetFloat(prefix + "quadratic", light.Quadratic());
        // Место в атласе теней. Раздаётся покадрово (см. LocalShadowAtlas::
        // Prepare); -1 означает «тени у этого источника в этом кадре нет».
        shader.SetInt(prefix + "shadowSlot", light.ShadowSlot);
    }

    // Прожекторы (spot): то же затухание, плюс направление конуса и косинусы
    // внутреннего/внешнего угла для мягкого края (см. lit.frag CalcSpotLight).
    int spotCount = std::min(static_cast<int>(env.SpotLights.size()), LightingEnvironment::MaxSpotLights);
    shader.SetInt("uNumSpotLights", spotCount);
    for (int i = 0; i < spotCount; ++i) {
        const SpotLight& light = env.SpotLights[i];
        std::string prefix = "uSpotLights[" + std::to_string(i) + "].";
        shader.SetVec3(prefix + "position", light.Position);
        shader.SetVec3(prefix + "direction", light.Direction);
        shader.SetVec3(prefix + "color", light.Color);
        shader.SetFloat(prefix + "intensity", light.Intensity);
        shader.SetFloat(prefix + "constant", light.Constant());
        shader.SetFloat(prefix + "linear", light.Linear());
        shader.SetFloat(prefix + "quadratic", light.Quadratic());
        shader.SetFloat(prefix + "cosInner", light.CosInner());
        shader.SetFloat(prefix + "cosOuter", light.CosOuter());
        shader.SetInt(prefix + "shadowSlot", light.ShadowSlot);
    }

    // Туман (атмосфера сцены) — применяется только в режиме полного освещения.
    shader.SetInt("uFogEnabled", env.Fog.Enabled ? 1 : 0);
    shader.SetVec3("uFogColor", env.Fog.Color);
    shader.SetFloat("uFogStart", env.Fog.Start);
    shader.SetFloat("uFogEnd", env.Fog.End);
}

// Тени прожекторов и точечных источников: один атлас, по прямоугольнику на
// плитку (см. render/ShadowAtlas.h).
//
// Незанятые места заливаются нулевым прямоугольником осознанно: до них шейдер
// не доходит (у источника без места shadowSlot == -1), но оставить там мусор
// от прошлого кадра значило бы, что первая же ошибка в раздаче мест проявится
// не как отсутствие тени, а как тень из ниоткуда.
inline void UploadLocalShadowUniforms(Shader& shader, const sage::render::LocalShadowBinding& local) {
    shader.SetInt("uLocalShadowAtlas", sage::render::kLocalShadowUnit);
    shader.SetInt("uLocalShadowsEnabled", local.Enabled ? 1 : 0);
    shader.SetFloat("uLocalAtlasTexel", local.AtlasTexel);
    for (int i = 0; i < sage::render::LocalShadowBinding::kMaxSpot; ++i) {
        const std::string idx = "[" + std::to_string(i) + "]";
        shader.SetMat4("uSpotShadowMat" + idx, local.SpotMatrix[i]);
        shader.SetVec4("uSpotShadowRect" + idx, local.SpotRect[i]);
        shader.SetVec3("uSpotShadowParams" + idx, local.SpotParams[i]);
    }
    for (int i = 0; i < sage::render::LocalShadowBinding::kMaxPoint; ++i) {
        shader.SetVec3("uPointShadowParams[" + std::to_string(i) + "]", local.PointParams[i]);
    }
    const int rects =
        sage::render::LocalShadowBinding::kMaxPoint * sage::render::LocalShadowBinding::kFaces;
    for (int i = 0; i < rects; ++i) {
        shader.SetVec4("uPointShadowRect[" + std::to_string(i) + "]", local.PointRect[i]);
    }
}

// Заливает uniform'ы карт теней в шейдер-приёмник: матрицы пространства света
// по каскадам, номера текстурных юнитов и флаг включённости. Тень применяется
// только к вкладу солнца (см. PbrShader.h). Вызывается после UploadLighting
// для тех же шейдеров.
//
// Одна карта — частный случай каскадов с Count == 1: шейдер найдёт фрагмент в
// нулевом каскаде и дальше не пойдёт. Поэтому отдельного «некаскадного» пути
// здесь нет и разойтись двум путям негде.
inline void UploadShadowUniforms(Shader& shader, const ShadowBinding& shadows) {
    const int count = std::max(1, std::min(shadows.Count, ShadowMap::kMaxCascades));
    for (int i = 0; i < ShadowMap::kMaxCascades; ++i) {
        const int src = std::min(i, count - 1);
        shader.SetMat4("uShadowLightSpace[" + std::to_string(i) + "]", shadows.Matrices[src]);
        shader.SetFloat("uShadowTexelWorld[" + std::to_string(i) + "]", shadows.TexelWorld[src]);
        shader.SetFloat("uShadowDepthBias[" + std::to_string(i) + "]", shadows.DepthBias[src]);
        shader.SetInt(i == 0 ? "uShadowMap" : ("uShadowMap" + std::to_string(i)),
                      ShadowCascadeUnit(i));
    }
    shader.SetInt("uShadowCascades", count);
    shader.SetInt("uShadowsEnabled", shadows.Enabled ? 1 : 0);
    shader.SetFloat("uShadowFadeStart", shadows.FadeStart);
    shader.SetFloat("uShadowFadeEnd", shadows.FadeEnd);
    UploadLocalShadowUniforms(shader, shadows.Local);
}

// Привязать карты и залить uniform'ы одним вызовом. Разделять эти два шага —
// значит завести место, где можно сделать один и забыть другой; результат
// (шейдер сэмплирует чужую текстуру) выглядит как случайные чёрные пятна и
// ищется долго.
inline void BindAndUploadShadows(Shader& shader, const ShadowBinding& shadows) {
    shadows.Bind();
    UploadShadowUniforms(shader, shadows);
}
