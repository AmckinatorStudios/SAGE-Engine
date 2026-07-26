#include "sage/ecs/CameraView.h"

#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

#include "sage/scene/Scene.h"
#include "sage/scene/Components.h"

namespace sage::ecs {

CameraFrame PrimaryCameraFrame(Scene& scene, float aspect) {
    CameraFrame frame;
    entt::entity camEntity = entt::null;
    auto view = scene.Registry().view<CameraComponent, Transform>();
    for (auto e : view) {
        if (view.get<CameraComponent>(e).Primary) { camEntity = e; break; }
    }
    if (camEntity == entt::null) return frame; // HasPrimary = false

    const CameraComponent& cam = view.get<CameraComponent>(camEntity);
    // МИРОВАЯ матрица (иерархия родителей) — единый источник для превью и игры.
    glm::mat4 world = scene.WorldMatrix(camEntity);
    glm::vec3 pos = glm::vec3(world[3]);
    glm::vec3 fwd = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
    glm::vec3 up  = glm::normalize(glm::vec3(world * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)));

    frame.View = glm::lookAt(pos, pos + fwd, up);
    frame.Proj = cam.ProjectionMatrix(aspect);
    frame.Position = pos;
    frame.HasPrimary = true;
    return frame;
}

} // namespace sage::ecs
