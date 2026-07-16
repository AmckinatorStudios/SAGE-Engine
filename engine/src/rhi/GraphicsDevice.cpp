#include "sage/rhi/GraphicsDevice.h"
#include "rhi/opengl/OpenGLDevice.h"
#include <stdexcept>

namespace sage::rhi {

static GraphicsDevice* s_current = nullptr;

std::unique_ptr<GraphicsDevice> GraphicsDevice::Create(Backend backend) {
    switch (backend) {
        case Backend::OpenGL:
            return std::make_unique<OpenGLDevice>();
    }
    throw std::runtime_error("GraphicsDevice::Create: неизвестный бэкенд");
}

GraphicsDevice& GraphicsDevice::Get() {
    if (!s_current) throw std::runtime_error("GraphicsDevice::Get: девайс не инициализирован (нет активного Application?)");
    return *s_current;
}

void GraphicsDevice::SetCurrent(GraphicsDevice* device) {
    s_current = device;
}

} // namespace sage::rhi
