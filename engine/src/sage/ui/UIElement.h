#pragma once
#include "UIRenderer.h"
#include <glm/glm.hpp>

// ---------------------------------------------------------------------
// Базовый элемент UI-системы движка.
//
// Каждый элемент привязан к ЯКОРЮ экрана (угол/грань/центр) и смещён от
// него на Offset. Благодаря этому интерфейс автоматически "держится" за
// своё место при любом размере окна: полоска здоровья прибита к левому
// верхнему углу, часы — к правому, хотбар — к нижнему центру, и при
// изменении разрешения ничего не разъезжается.
//
// Offset задаётся В СТОРОНУ ЭКРАНА от якоря: для правых якорей смещение
// по X идёт влево, для нижних — вверх. То есть Offset {16, 16} у
// TopRight означает "16px от правого края, 16px от верхнего".
// ---------------------------------------------------------------------

enum class UIAnchor {
    TopLeft,    TopCenter,    TopRight,
    CenterLeft, Center,       CenterRight,
    BottomLeft, BottomCenter, BottomRight
};

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
        float x = 0.0f, y = 0.0f;

        switch (Anchor) {
            case UIAnchor::TopLeft: case UIAnchor::CenterLeft: case UIAnchor::BottomLeft:
                x = Offset.x; break;
            case UIAnchor::TopCenter: case UIAnchor::Center: case UIAnchor::BottomCenter:
                x = screenW * 0.5f - Size.x * 0.5f + Offset.x; break;
            case UIAnchor::TopRight: case UIAnchor::CenterRight: case UIAnchor::BottomRight:
                x = screenW - Size.x - Offset.x; break;
        }
        switch (Anchor) {
            case UIAnchor::TopLeft: case UIAnchor::TopCenter: case UIAnchor::TopRight:
                y = Offset.y; break;
            case UIAnchor::CenterLeft: case UIAnchor::Center: case UIAnchor::CenterRight:
                y = screenH * 0.5f - Size.y * 0.5f + Offset.y; break;
            case UIAnchor::BottomLeft: case UIAnchor::BottomCenter: case UIAnchor::BottomRight:
                y = screenH - Size.y - Offset.y; break;
        }
        return {x, y};
    }

    // Отрисовка элемента. Вызывается между ui.Begin() и ui.End().
    virtual void Draw(UIRenderer& ui) = 0;
};
