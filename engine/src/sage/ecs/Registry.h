#pragma once
#include <entt/entt.hpp>

// Тонкий фасад над entt — стабильные имена типов ECS в пространстве sage::ecs.
// Смысл: игры и системы движка ссылаются на sage::ecs::Registry/Entity, а не на
// entt напрямую, поэтому базовую ECS-реализацию можно заменить, не переписывая
// потребителей. Scene (sage/scene/Scene.h) владеет одним таким Registry.
namespace sage::ecs {

using Registry = entt::registry;
using Entity = entt::entity;
inline constexpr auto Null = entt::null;

} // namespace sage::ecs
