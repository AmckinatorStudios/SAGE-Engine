#include "sage/ecs/CameraView.h"

#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"

namespace sage::ecs {

CameraFrame CameraFrameFor(Scene& scene, entt::entity camera, float aspect) {
    CameraFrame frame;
    if (camera == entt::null || !scene.Registry().valid(camera)) return frame;
    const CameraComponent* cam = scene.Registry().try_get<CameraComponent>(camera);
    if (!cam) return frame;

    // МИРОВАЯ матрица (иерархия родителей) — единый источник для превью и игры.
    glm::mat4 world = scene.WorldMatrix(camera);
    glm::vec3 pos = glm::vec3(world[3]);
    glm::vec3 fwd = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
    glm::vec3 up  = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)));

    frame.View = glm::lookAt(pos, pos + fwd, up);
    frame.Proj = cam->ProjectionMatrix(aspect);
    frame.Position = pos;
    frame.HasPrimary = true;
    return frame;
}

CameraFrame PrimaryCameraFrame(Scene& scene, float aspect) {
    entt::entity camEntity = entt::null;
    auto view = scene.Registry().view<CameraComponent, Transform>();
    for (auto e : view) {
        if (view.get<CameraComponent>(e).Primary) { camEntity = e; break; }
    }
    return CameraFrameFor(scene, camEntity, aspect);
}

} // namespace sage::ecs
