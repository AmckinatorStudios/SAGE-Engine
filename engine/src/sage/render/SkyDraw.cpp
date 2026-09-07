#include "sage/render/SkyDraw.h"

#include "sage/render/ResourceManager.h"
#include "sage/render/SkyRenderer.h"
#include "sage/render/Skybox.h"
#include "sage/scene/Light.h"

namespace sage::render {

std::shared_ptr<Skybox> SceneSkyCubemap(const LightingEnvironment& env) {
    const SkyboxSettings& sky = env.Skybox;
    if (!sky.Enabled) return nullptr;
    switch (sky.Kind) {
        case SkyboxSettings::Source::Cubemap:
            return sky.CubemapDir.empty() ? nullptr
                                          : ResourceManager::Instance().GetSkybox(sky.CubemapDir);
        case SkyboxSettings::Source::Faces:
            return sky.HasFaces() ? ResourceManager::Instance().GetSkyboxFaces(sky.FacePaths)
                                  : nullptr;
        default:
            return nullptr;   // процедурное — картинки нет и не должно быть
    }
}

void DrawSceneSky(SkyRenderer& fallback, const LightingEnvironment& env, const glm::mat4& view,
                  const glm::mat4& proj) {
    if (!env.Skybox.Enabled) return;

    if (std::shared_ptr<Skybox> cubemap = SceneSkyCubemap(env)) {
        cubemap->Draw(view, proj, env.Skybox.Intensity, env.Skybox.RotationDeg);
        return;
    }
    // Сюда попадаем и когда небо процедурное, и когда текстурное не
    // загрузилось. Второе — не повод падать и не повод оставлять чёрный
    // прямоугольник: причина уже в логе, а человек должен видеть сцену и
    // продолжать работать.

    // ЦВЕТА — РАЗРЕШЁННЫЕ, а не авторские: в них уже учтено время суток (см.
    // sage/render/SkyModel.h). Пока здесь стояли поля настроек, небо оставалось
    // полуденным независимо от того, где солнце.
    fallback.Draw(view, proj, env.SkyTop(), env.SkyHorizon(), CelestialsFromEnvironment(env));
}

} // namespace sage::render
