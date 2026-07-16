#pragma once
#include "sage/rhi/GraphicsDevice.h"

namespace sage::rhi {

// OpenGL-реализация GraphicsDevice. Это — ЕДИНСТВЕННОЕ место уровня устройства,
// где движок включает glad и трогает GL-состояние напрямую. Остальной движок
// работает через интерфейс GraphicsDevice, поэтому смена бэкенда не затрагивает
// его код. Живёт в engine/src/rhi/opengl/ (реализация, не публичный заголовок).
class OpenGLDevice : public GraphicsDevice {
public:
    void Init(ProcLoader loader) override;

    const char* BackendName() const override { return "OpenGL"; }
    std::string ApiVersion() const override;

    void SetViewport(int x, int y, int width, int height) override;
    void SetClearColor(float r, float g, float b, float a) override;
    void Clear(bool color = true, bool depth = true) override;
    void BindDefaultFramebuffer() override;

    void SetBlend(bool enabled) override;
    void SetDepthTest(bool enabled) override;
    void SetDepthWrite(bool enabled) override;
    void SetCullFace(bool enabled) override;

    void BindTexture2D(int unit, unsigned int nativeHandle) override;
};

} // namespace sage::rhi
