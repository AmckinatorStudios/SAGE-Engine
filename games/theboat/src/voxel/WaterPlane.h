#pragma once
#include <memory>
#include <glm/glm.hpp>
#include "sage/rhi/GraphicsDevice.h"

// Пока вода — не воксельный блок, а один большой полупрозрачный квад на
// фиксированной высоте. Этого достаточно для ощущения "открытого моря" и
// сильно дешевле, чем честные воксели воды. Настоящую воксельную воду
// (с течениями/уровнями) добавим отдельно, когда понадобится геймплейно.
class WaterPlane {
public:
    WaterPlane(float halfSize, float seaLevel) {
        float y = seaLevel;
        float s = halfSize;
        float vertices[] = {
            -s, y, -s,
             s, y, -s,
             s, y,  s,
            -s, y,  s,
        };
        unsigned int indices[] = { 0, 2, 1, 2, 0, 3 };
        m_indexCount = 6;

        sage::rhi::VertexLayout layout;
        layout.Stride = 3 * sizeof(float);
        layout.Attributes = {{0, 3, sage::rhi::AttribType::Float, 0}};
        m_geometry = sage::rhi::GraphicsDevice::Get().CreateGeometry(layout);
        m_geometry->SetVertexData(vertices, sizeof(vertices), /*dynamic=*/false);
        m_geometry->SetIndexData(indices, 6, /*dynamic=*/false);
    }

    // GPU-ресурсом владеет единолично — копирование запрещено (unique_ptr
    // защищает от двойного освобождения автоматически).
    WaterPlane(const WaterPlane&) = delete;
    WaterPlane& operator=(const WaterPlane&) = delete;
    WaterPlane(WaterPlane&&) noexcept = default;
    WaterPlane& operator=(WaterPlane&&) noexcept = default;

    void Draw() const {
        m_geometry->DrawIndexed(m_indexCount);
    }

private:
    std::unique_ptr<sage::rhi::Geometry> m_geometry;
    size_t m_indexCount = 0;
};
