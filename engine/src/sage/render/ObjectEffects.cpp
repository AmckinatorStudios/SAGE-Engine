#include "sage/render/ObjectEffects.h"

#include <string>
#include <unordered_map>

#include "sage/core/Profiler.h"
#include "sage/ecs/RenderBatch.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/PostChainIO.h"
#include "sage/render/Shader.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/ComponentRegistry.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneComponents.h"

namespace sage::render {

void RegisterObjectEffectsComponent() {
    sage::scene::RegisterComponent(sage::scene::MakeComponentType<ObjectEffectsComponent>(
        "object_effects", [](const ObjectEffectsComponent& c) { return PostChainToJson(c.Effects); },
        [](const nlohmann::json& j, ObjectEffectsComponent& c) { c.Effects = PostChainFromJson(j); }));
}

namespace {

const char* kMaskVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uViewProj;
uniform mat4 uModel;
void main() { gl_Position = uViewProj * uModel * vec4(aPos, 1.0); }
)";

const char* kMaskFrag = R"(#version 330 core
out vec4 FragColor;
uniform float uLayer;   // номер слоя / 255; 0 — объект без эффектов (только заслоняет)
void main() { FragColor = vec4(uLayer, 0.0, 0.0, 1.0); }
)";

Shader& MaskShader() {
    static Shader* s = new Shader(Shader::FromSource(kMaskVert, kMaskFrag, "ObjectEffectsMask"));
    return *s;
}

// Эффекты сущности: свои или ближайшего предка (модель из частей — целиком).
const ObjectEffectsComponent* EffectsOf(const entt::registry& reg, entt::entity e) {
    // Глубина иерархии ограничена — защита от петли в битом файле.
    for (int guard = 0; e != entt::null && reg.valid(e) && guard < 4096; ++guard) {
        if (const ObjectEffectsComponent* fx = reg.try_get<ObjectEffectsComponent>(e))
            if (fx->Any()) return fx;
        const HierarchyComponent* h = reg.try_get<HierarchyComponent>(e);
        e = h ? h->Parent : entt::null;
    }
    return nullptr;
}

} // namespace

ObjectEffectsMask::ObjectEffectsMask() = default;
ObjectEffectsMask::~ObjectEffectsMask() = default;

sage::rhi::TextureHandle ObjectEffectsMask::Mask() const {
    return m_target ? m_target->ColorTexture() : sage::rhi::TextureHandle{};
}

bool ObjectEffectsMask::Render(Scene& scene, const sage::ecs::RenderBatch& batch, const glm::mat4& view,
                               const glm::mat4& proj, int width, int height) {
    m_layers.clear();
    entt::registry& reg = scene.Registry();
    if (reg.view<ObjectEffectsComponent>().empty() || width < 1 || height < 1) return false;

    // Слой сущности. Одинаковые наборы — один слой: сравнение по JSON набора
    // (он же формат файла), считается один раз на компонент за кадр.
    std::unordered_map<const ObjectEffectsComponent*, int> layerOf;
    std::unordered_map<std::string, int> byText;
    auto layerFor = [&](entt::entity e) -> int {
        const ObjectEffectsComponent* fx = EffectsOf(reg, e);
        if (!fx) return 0;
        auto it = layerOf.find(fx);
        if (it != layerOf.end()) return it->second;
        const std::string text = PostChainToJson(fx->Effects).dump();
        auto t = byText.find(text);
        int layer = 0;
        if (t != byText.end()) {
            layer = t->second;
        } else if (m_layers.size() < 255) {   // номер слоя — байт маски
            m_layers.push_back(fx->Effects);
            layer = (int)m_layers.size();
            byText.emplace(text, layer);
        }
        layerOf.emplace(fx, layer);
        return layer;
    };

    // Есть ли среди видимого хоть один объект с эффектами — иначе проход впустую.
    bool any = false;
    batch.ForEachVisible([&](entt::entity e, Mesh&, const glm::mat4&) {
        if (!any && layerFor(e) > 0) any = true;
    });
    if (!any) {
        m_layers.clear();
        return false;
    }

    SAGE_PROFILE("Маска эффектов объектов");
    if (!m_target) m_target = std::make_unique<Framebuffer>(width, height);
    else m_target->Resize(width, height);

    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    m_target->Bind();
    device.SetViewport(0, 0, width, height);
    device.SetClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    device.Clear(true, true);
    device.SetBlend(false);
    device.SetDepthTest(true);
    device.SetDepthWrite(true);
    device.SetCullMode(sage::rhi::CullMode::Back);

    // Рисуется ВСЁ видимое: объекты без эффектов пишут 0 и заслоняют собой
    // тех, что с эффектами, — иначе светилось бы и то, что за стеной.
    Shader& sh = MaskShader();
    sh.Use();
    sh.SetMat4("uViewProj", proj * view);
    batch.ForEachVisible([&](entt::entity e, Mesh& mesh, const glm::mat4& model) {
        sh.SetMat4("uModel", model);
        sh.SetFloat("uLayer", (float)layerFor(e) / 255.0f);
        mesh.Draw();
    });
    return true;
}

} // namespace sage::render
