#pragma once
#include <entt/entt.hpp>

#include "sage/ui/UIAnchor.h"
#include "sage/ui/UIRenderer.h"

// ---------------------------------------------------------------------------
// Маска по форме для отрисовки (см. sage::ui::Mask, UIRenderer::PushMask).
// ---------------------------------------------------------------------------
namespace sage::ui {

// Маска хозяина owner для его поддерева: окно window (экранное), скругление
// (своё или подложки хозяина), мягкий край, инверсия, картинка — в экранных
// единицах при масштабе холста scale. Картинка-маска грузится здесь лениво.
UIRenderer::Mask ShapeMaskFor(const entt::registry& reg, entt::entity owner, const UIRect& window, float scale);

} // namespace sage::ui
