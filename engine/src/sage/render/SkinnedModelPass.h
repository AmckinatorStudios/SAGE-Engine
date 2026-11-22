#pragma once
#include <glm/glm.hpp>

#include "sage/ecs/RenderLayers.h"
#include "sage/render/Reflection.h"
#include "sage/render/ShadowMap.h"

class Scene;
struct LightingEnvironment;
struct Material;

// ---------------------------------------------------------------------------
// Отрисовка моделей со скелетом (AnimationComponent) — проход рендера.
//
// Жила в sage/anim (AnimationSystem), и рендер звал анимацию, чтобы
// нарисовать кадр, а анимация тянула тени, отражения и кластеры света: модули
// зависели друг от друга по кругу. Анимация считает позу (палитру костей и
// веса морфов), рисует её рендер — так же, как статику (RenderBatch).
// ---------------------------------------------------------------------------
namespace sage::render {

class LightClusters;
struct SkinnedMaterial;

// Материал сцены в материал модели со скелетом (им сущность перекрашивает
// модель вместо материалов файла).
SkinnedMaterial SkinnedMaterialFromScene(const Material& mat);

// Рисует все анимированные модели сцены в текущий фреймбуфер (после основного
// прохода сцены — тест глубины включён). Освещение ПОЛНОЕ и совпадает со
// статическими мешами: передаются позиция камеры, окружение и карты теней
// солнца (каскады — см. ShadowMap.h), как в статическом lit-проходе.
// shadingMode — номер отладочного вида кадра (render/DebugView.h), 0 — обычная
// отрисовка. Передаётся насквозь в SkinnedModel::Draw: разбор кадра по
// слагаемым обязан касаться и персонажей.
void DrawAnimatedModels(Scene& scene, const glm::mat4& view, const glm::mat4& proj,
                        const glm::vec3& viewPos, const LightingEnvironment& env,
                        const ShadowBinding& shadows,
                        const ReflectionBinding* reflections = nullptr,
                        int shadingMode = 0,
                        const LightClusters* clusters = nullptr,
                        const sage::ecs::LayerFilter* layers = nullptr);

// Рисует анимированные модели ТОЛЬКО в глубину (со скиннингом) — вызывается
// внутри depth-прохода карты теней солнца, чтобы они отбрасывали тень.
void DrawAnimatedModelsDepth(Scene& scene, const glm::mat4& lightMatrix);

} // namespace sage::render
