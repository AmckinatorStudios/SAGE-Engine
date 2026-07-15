#pragma once
#include "../scene/Light.h"
#include "Shader.h"
#include <algorithm>
#include <string>

// Заливает в шейдер все uniform'ы, описывающие освещение сцены: фоновую
// засветку, солнце и точечные источники. voxel.frag, basic.frag и water.frag
// ожидают одинаковый набор имён uniform'ов (uAmbient*, uSun*, uPointLights[]),
// поэтому вызов один и тот же для всех трёх — не нужно дублировать имена
// в main.cpp на каждый шейдер.
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
}
