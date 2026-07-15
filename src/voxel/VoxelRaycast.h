#pragma once
#include "Chunk.h"
#include <glm/glm.hpp>
#include <cmath>

struct VoxelHit {
    bool Hit = false;
    glm::ivec3 BlockPos{0};   // координаты блока, в который попали
    glm::ivec3 PlacePos{0};   // координаты соседней пустой ячейки (куда ставить новый блок)
};

// Идём вдоль луча (origin, dir) шагами по сетке вокселей (алгоритм DDA)
// и возвращаем первый твёрдый блок, который встретили, в пределах maxDistance.
inline VoxelHit RaycastVoxels(Chunk& chunk, const glm::vec3& origin, const glm::vec3& dir, float maxDistance) {
    VoxelHit result;

    glm::vec3 pos = origin - glm::vec3(chunk.WorldPos()); // переводим в локальные координаты чанка
    glm::ivec3 voxel = glm::ivec3(glm::floor(pos));

    glm::ivec3 step = { dir.x > 0 ? 1 : -1, dir.y > 0 ? 1 : -1, dir.z > 0 ? 1 : -1 };

    // tMax — расстояние вдоль луча до следующей границы вокселя по каждой оси
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
        calcTMax(pos.x, dir.x, voxel.x, step.x),
        calcTMax(pos.y, dir.y, voxel.y, step.y),
        calcTMax(pos.z, dir.z, voxel.z, step.z)
    };
    glm::vec3 tDelta = { calcTDelta(dir.x), calcTDelta(dir.y), calcTDelta(dir.z) };

    glm::ivec3 lastStepAxis{0, 0, 0}; // с какой стороны вошли в последний воксель
    float traveled = 0.0f;

    // Проверяем стартовый воксель на всякий случай (если камера уже внутри блока)
    while (traveled < maxDistance) {
        if (Chunk::InBounds(voxel.x, voxel.y, voxel.z) && IsSolid(chunk.GetBlock(voxel.x, voxel.y, voxel.z))) {
            result.Hit = true;
            result.BlockPos = voxel;
            result.PlacePos = voxel - lastStepAxis;
            return result;
        }

        // Шагаем к ближайшей следующей границе
        if (tMax.x < tMax.y && tMax.x < tMax.z) {
            voxel.x += step.x;
            traveled = tMax.x;
            tMax.x += tDelta.x;
            lastStepAxis = {step.x, 0, 0};
        } else if (tMax.y < tMax.z) {
            voxel.y += step.y;
            traveled = tMax.y;
            tMax.y += tDelta.y;
            lastStepAxis = {0, step.y, 0};
        } else {
            voxel.z += step.z;
            traveled = tMax.z;
            tMax.z += tDelta.z;
            lastStepAxis = {0, 0, step.z};
        }

        if (!Chunk::InBounds(voxel.x, voxel.y, voxel.z)) break;
    }

    return result;
}
