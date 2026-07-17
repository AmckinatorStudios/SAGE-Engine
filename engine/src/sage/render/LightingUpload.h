#pragma once
#include "sage/scene/Light.h"
#include "Shader.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <string>

// Заливает в шейдер все uniform'ы, описывающие освещение сцены: фоновую
// засветку, солнце и точечные источники. Любые шейдеры-приёмники, ожидающие
// одинаковый набор имён uniform'ов (uAmbient*, uSun*, uPointLights[]),
// используют один и тот же вызов — не нужно дублировать имена на каждый шейдер.
inline void UploadLighting(Shader& shader, const LightingEnvironment& env) {
    shader.SetVec3("uAmbientSky", env.SkyColor);
    shader.SetVec3("uAmbientGround", env.GroundColor);
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
    }
}

// Заливает uniform'ы карты теней в шейдер-приёмник: матрицу пространства
// света, номер текстурного юнита карты теней и флаг
// включённости. Тень применяется только к вкладу солнца (см. .frag).
// Вызывается после UploadLighting для тех же шейдеров.
inline void UploadShadowUniforms(Shader& shader, const glm::mat4& lightMatrix,
                                 int shadowMapUnit, bool enabled) {
    shader.SetMat4("uLightSpace", lightMatrix);
    shader.SetInt("uShadowMap", shadowMapUnit);
    shader.SetInt("uShadowsEnabled", enabled ? 1 : 0);
}
