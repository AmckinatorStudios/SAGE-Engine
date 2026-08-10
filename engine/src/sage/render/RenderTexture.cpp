#include "sage/render/RenderTexture.h"

#include <algorithm>

#include "sage/rhi/GraphicsDevice.h"

namespace sage::render {

RenderTexture::RenderTexture(const std::string& name, int width, int height)
    : m_name(name), m_fbo(std::max(width, 1), std::max(height, 1)) {
    m_view = Texture::Wrap(m_fbo.ColorTexture(), m_fbo.Width(), m_fbo.Height());
}

void RenderTexture::Begin(const glm::vec4& clearColor) {
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    m_fbo.Bind();
    device.SetClearColor(clearColor.r, clearColor.g, clearColor.b, clearColor.a);
    device.Clear();
}

void RenderTexture::End() {
    m_fbo.Resolve();
    sage::rhi::GraphicsDevice::Get().BindDefaultFramebuffer();
}

void RenderTexture::Resize(int width, int height) {
    width = std::max(width, 1);
    height = std::max(height, 1);
    if (width == m_fbo.Width() && height == m_fbo.Height()) return;
    m_fbo.Resize(width, height);
    // Хендл после пересоздания хранилища другой — обёртку надо обновить, иначе
    // интерфейс продолжит показывать текстуру, которой больше нет.
    m_view = Texture::Wrap(m_fbo.ColorTexture(), m_fbo.Width(), m_fbo.Height());
}

RenderTextureRegistry& RenderTextureRegistry::Instance() {
    static RenderTextureRegistry* r = new RenderTextureRegistry();
    return *r;
}

RenderTexture& RenderTextureRegistry::GetOrCreate(const std::string& name, int width, int height) {
    auto it = m_items.find(name);
    if (it != m_items.end()) {
        it->second->Resize(width, height);
        return *it->second;
    }
    auto rt = std::make_unique<RenderTexture>(name, width, height);
    RenderTexture& ref = *rt;
    m_items.emplace(name, std::move(rt));
    return ref;
}

RenderTexture* RenderTextureRegistry::Find(const std::string& name) {
    auto it = m_items.find(name);
    return it == m_items.end() ? nullptr : it->second.get();
}

void RenderTextureRegistry::Remove(const std::string& name) { m_items.erase(name); }
void RenderTextureRegistry::Clear() { m_items.clear(); }

std::shared_ptr<Texture> RenderTextureRegistry::Resolve(const std::string& path) {
    constexpr const char* kPrefix = "rt:";
    if (path.rfind(kPrefix, 0) != 0) return nullptr;
    RenderTexture* rt = Instance().Find(path.substr(3));
    return rt ? rt->AsTexture() : nullptr;
}

} // namespace sage::render
