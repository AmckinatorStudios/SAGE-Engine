#include "sage/rhi/GraphicsDevice.h"
#include "rhi/null/NullDevice.h"
#include "rhi/opengl/OpenGLDevice.h"
#include <cctype>
#include <stdexcept>

#ifdef SAGE_HAS_VULKAN
#  include "rhi/vulkan/VulkanDevice.h"
#endif

namespace sage::rhi {

static GraphicsDevice* s_current = nullptr;

std::unique_ptr<GraphicsDevice> GraphicsDevice::Create(Backend backend) {
    switch (backend) {
        case Backend::OpenGL:
            return std::make_unique<OpenGLDevice>();
        case Backend::Null:
            return std::make_unique<NullDevice>();
        case Backend::Vulkan:
#ifdef SAGE_HAS_VULKAN
            return std::make_unique<VulkanDevice>();
#else
            // Сборка без -DSAGE_VULKAN=ON. Не бросаем: выбор бэкенда приходит из
            // настроек игрока, и «собрано без Vulkan» — это повод откатиться на
            // OpenGL, а не отказать в запуске.
            return nullptr;
#endif
    }
    throw std::runtime_error("GraphicsDevice::Create: неизвестный бэкенд");
}

bool GraphicsDevice::Available(Backend backend) {
    switch (backend) {
        case Backend::OpenGL:
        case Backend::Null:
            // Оба всегда доступны: GL-контекст даёт оконная система (без него
            // движок и не запустится), Null не требует ничего.
            return true;
        case Backend::Vulkan:
#ifdef SAGE_HAS_VULKAN
            return VulkanDevice::Available();
#else
            return false;
#endif
    }
    return false;
}

const char* GraphicsDevice::BackendId(Backend backend) {
    switch (backend) {
        case Backend::OpenGL: return "opengl";
        case Backend::Vulkan: return "vulkan";
        case Backend::Null: return "null";
    }
    return "opengl";
}

std::vector<GraphicsDevice::BackendInfo> GraphicsDevice::Catalog() {
    using S = BackendInfo::State;
    std::vector<BackendInfo> list;
    list.push_back({"opengl", "OpenGL 3.3+", S::Ready});
#ifdef SAGE_HAS_VULKAN
    // Устройство Vulkan поднимается, ресурсы и конвейеры — ещё нет (см.
    // ChooseBackend в Application.cpp): выбрать можно, но это эксперимент.
    list.push_back({"vulkan", "Vulkan 1.2",
                    VulkanDevice::Available() ? S::Experimental : S::Unavailable});
#else
    list.push_back({"vulkan", "Vulkan 1.2", S::NotBuilt});
#endif
    // DirectX в движке нет: все шейдеры написаны на GLSL, и бэкенду D3D нужен
    // перевод каждого в HLSL. Пункты здесь затем, чтобы человек увидел ответ,
    // а не пустое место (см. BackendInfo).
    list.push_back({"d3d11", "DirectX 11", S::NotImplemented});
    list.push_back({"d3d12", "DirectX 12", S::NotImplemented});
    return list;
}

bool GraphicsDevice::ParseBackend(const std::string& text, Backend& out) {
    std::string s;
    for (char c : text) s += (char)std::tolower((unsigned char)c);
    if (s == "opengl" || s == "gl") { out = Backend::OpenGL; return true; }
    if (s == "vulkan" || s == "vk") { out = Backend::Vulkan; return true; }
    if (s == "null") { out = Backend::Null; return true; }
    return false;
}

GraphicsDevice& GraphicsDevice::Get() {
    if (!s_current) throw std::runtime_error("GraphicsDevice::Get: девайс не инициализирован (нет активного Application?)");
    return *s_current;
}

void GraphicsDevice::SetCurrent(GraphicsDevice* device) {
    s_current = device;
}

} // namespace sage::rhi
