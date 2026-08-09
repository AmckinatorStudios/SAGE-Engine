#pragma once
#include "UIRenderer.h"
#include "UIAnchor.h"
#include <glm/glm.hpp>

// ---------------------------------------------------------------------
// Базовый элемент immediate-mode UI-виджетов движка (Widgets.h). Enum якорей
// и математика вёрстки живут в UIAnchor.h — их разделяет ECS-компонент
// интерфейса (компоненты sage::ui, см. UI.h), редактируемый в редакторе.
// ---------------------------------------------------------------------

class UIElement {
public:
    UIAnchor Anchor = UIAnchor::TopLeft;
    glm::vec2 Offset{0.0f};  // отступ от якоря (в сторону центра экрана)
    glm::vec2 Size{0.0f};    // размер элемента в пикселях
    bool Visible = true;

    virtual ~UIElement() = default;

    // Левый верхний угол элемента в экранных координатах для текущего
    // размера экрана (уже с учётом якоря, отступа и размера элемента)
    glm::vec2 ResolvePosition(float screenW, float screenH) const {
        return sage::ui::ResolveAnchored(Anchor, Offset, Size,
                                         sage::ui::UIRect{0.0f, 0.0f, screenW, screenH});
    }

    // Отрисовка элемента. Вызывается между ui.Begin() и ui.End().
    virtual void Draw(UIRenderer& ui) = 0;
};
