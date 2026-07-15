#pragma once
#include "World.h"
#include <glm/glm.hpp>
#include <cmath>

struct WorldVoxelHit {
    bool Hit = false;
    glm::ivec3 BlockPos{0};
    glm::ivec3 PlacePos{0};
};

// То же самое, что VoxelRaycast.h, но работает по мировым координатам через
// World::GetBlock — то есть корректно пересекает границы чанков.
inline WorldVoxelHit RaycastWorld(World& world, const glm::vec3& origin, const glm::vec3& dir, float maxDistance) {
    WorldVoxelHit result;

    glm::ivec3 voxel = glm::ivec3(glm::floor(origin));
    glm::ivec3 step = { dir.x > 0 ? 1 : -1, dir.y > 0 ? 1 : -1, dir.z > 0 ? 1 : -1 };

    auto calcTMax = [](float p, float d, int v, int s) -> float {
        if (d == 0.0f) return 1e30f;
        float boundary = (s > 0) ? (v + 1) : v;
        return (boundary - p) / d;
    };
    auto calcTDelta = [](float d) -> float {
        if (d == 0.0f) return 1e30f;
        return std::abs(1.0f / d);
    };

    glm::vec3 tMax = {
        calcTMax(origin.x, dir.x, voxel.x, step.x),
        calcTMax(origin.y, dir.y, voxel.y, step.y),
        calcTMax(origin.z, dir.z, voxel.z, step.z)
    };
    glm::vec3 tDelta = { calcTDelta(dir.x), calcTDelta(dir.y), calcTDelta(dir.z) };

    glm::ivec3 lastStepAxis{0, 0, 0};
    float traveled = 0.0f;

    while (traveled < maxDistance) {
        if (IsSolid(world.GetBlock(voxel.x, voxel.y, voxel.z))) {
            result.Hit = true;
            result.BlockPos = voxel;
            result.PlacePos = voxel - lastStepAxis;
            return result;
        }

        if (tMax.x < tMax.y && tMax.x < tMax.z) {
            voxel.x += step.x; traveled = tMax.x; tMax.x += tDelta.x; lastStepAxis = {step.x, 0, 0};
        } else if (tMax.y < tMax.z) {
            voxel.y += step.y; traveled = tMax.y; tMax.y += tDelta.y; lastStepAxis = {0, step.y, 0};
        } else {
            voxel.z += step.z; traveled = tMax.z; tMax.z += tDelta.z; lastStepAxis = {0, 0, step.z};
        }
    }

    return result;
}
