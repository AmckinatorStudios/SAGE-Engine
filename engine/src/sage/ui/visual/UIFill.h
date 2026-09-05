#pragma once
#include <string>

#include "sage/ui/core/UIComponent.h"
#include "sage/ui/visual/UIGradient.h"

// ---------------------------------------------------------------------------
// ЗАЛИВКА (§23 ТЗ) — чем закрашен прямоугольник узла.
//
// Заливка не знает, что она «подложка кнопки» или «фон панели»: она просто
// закрашивает прямоугольник узла тем, что в ней задано. Скругления живут здесь
// же, потому что скругление — свойство ФОРМЫ заливки, а не узла: у одного узла
// может быть заливка со скруглением и картинка без него.
// ---------------------------------------------------------------------------
namespace sage::ui {

struct UIFill : UIComponentOf<UIFill> {
    static const UIComponentType& StaticType();

    enum class Kind {
        Solid,
        Gradient,
        Texture, // картинка как заливка (узор, шум, фон)
    };

    Kind Type = Kind::Solid;
    UIColor Color{0.09f, 0.10f, 0.14f, 0.9f};
    UIGradient Gradient;

    std::string TexturePath; // для Kind::Texture
    glm::vec2 TextureScale{1.0f, 1.0f};
    glm::vec2 TextureOffset{0.0f, 0.0f};
    bool Repeat = false;

    UICorners Radius{0.0f};
    // Мягкость края в пикселях: 0 — резкий SDF-край, больше — размытая
    // «подсветка». Здесь, а не в эффектах, потому что это свойство самой
    // фигуры, и платить за него отдельным проходом незачем.
    float Softness = 0.0f;
};

} // namespace sage::ui
