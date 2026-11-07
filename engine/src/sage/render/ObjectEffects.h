#pragma once
#include <memory>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "sage/render/PostEffect.h"
#include "sage/rhi/Resources.h"

class Scene;
class Framebuffer;
namespace sage::ecs { class RenderBatch; }

// ---------------------------------------------------------------------------
// ЭФФЕКТЫ ОТДЕЛЬНЫХ ОБЪЕКТОВ: свой набор звеньев изображения у сущности —
// Invert, Grayscale, Blur, Glow, Pixelate, Dissolve и любые другие звенья
// этапа «Линза» (каталог PostEffectCatalog, в том числе звенья игры).
// Несколько эффектов на объекте накладываются по порядку, у каждого своя
// интенсивность; менять их можно во время игры (скрипты: AddObjectEffect,
// SetObjectEffect…).
//
// Действует на сущность и всех её потомков (модель из частей — целиком).
// Своё у потомка важнее родительского. Видно только там, где объект виден:
// заслонённое стеной не меняется.
//
// В файле сцены: components.object_effects = { "effects": [...] } — тот же
// формат, что у тракта камеры (PostChainToJson).
// ---------------------------------------------------------------------------
namespace sage::render {

struct ObjectEffectsComponent {
    PostChain Effects;
    bool Any() const {
        for (const PostEffect& e : Effects.Effects)
            if (e.Enabled) return true;
        return false;
    }
};

// Описание компонента для реестра (scene::RegisterComponent).
void RegisterObjectEffectsComponent();

// Маска эффектов объектов одного вида кадра. Рисуется после геометрии
// (RenderSceneColor того же батча) и отдаётся PostFX::SetObjectLayers.
//
// Объекты с одинаковым набором эффектов делят слой: тысяча светящихся
// кристаллов — один проход цепочки, а не тысяча.
class ObjectEffectsMask {
public:
    ObjectEffectsMask();
    ~ObjectEffectsMask();

    // Собрать маску. false — объектов с эффектами в кадре нет, маска не
    // рисовалась (и слоёв нет). После вызова привязана цель маски —
    // вызывающий возвращает свою.
    bool Render(Scene& scene, const sage::ecs::RenderBatch& batch, const glm::mat4& view, const glm::mat4& proj,
                int width, int height);

    sage::rhi::TextureHandle Mask() const;
    const std::vector<PostChain>& Layers() const { return m_layers; }

private:
    std::unique_ptr<Framebuffer> m_target;
    std::vector<PostChain> m_layers;
};

} // namespace sage::render
