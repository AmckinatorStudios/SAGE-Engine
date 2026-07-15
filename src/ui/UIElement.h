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
//
// LayoutScale — коэффициент масштаба вёрстки (см. UICanvas: масштаб под
// референсное разрешение, чтобы интерфейс одинаково смотрелся при разном
// соотношении сторон). Умножает Size и Offset; виджеты дополнительно
// масштабируют свой текст. По умолчанию 1.0 — попиксельная вёрстка.
// ---------------------------------------------------------------------

enum class UIAnchor {
    TopLeft,    TopCenter,    TopRight,
    CenterLeft, Center,       CenterRight,
    BottomLeft, BottomCenter, BottomRight
};

// Состояние ввода для интерактивных виджетов (кнопки, переключатели).
// Заполняется игрой из мыши и передаётся в UICanvas::Update каждый кадр.
struct UIInputState {
    glm::vec2 Mouse{-1.0f};      // позиция курсора в пикселях; (-1,-1) — курсора нет
    bool MouseDown = false;      // ЛКМ удерживается в этом кадре
    bool MousePressed = false;   // ЛКМ нажата именно в этом кадре
    bool MouseReleased = false;  // ЛКМ отпущена именно в этом кадре
};

class UIElement {
public:
    UIAnchor Anchor = UIAnchor::TopLeft;
    glm::vec2 Offset{0.0f};       // отступ от якоря (в сторону центра экрана)
    glm::vec2 Size{0.0f};         // размер элемента в пикселях (до масштаба)
    bool Visible = true;
    float LayoutScale = 1.0f;     // множитель вёрстки (выставляет UICanvas)

    virtual ~UIElement() = default;

    glm::vec2 ScaledSize() const { return Size * LayoutScale; }
    glm::vec2 ScaledOffset() const { return Offset * LayoutScale; }

    // Левый верхний угол элемента в экранных координатах для текущего
    // размера экрана (уже с учётом якоря, отступа, размера и масштаба)
    glm::vec2 ResolvePosition(float screenW, float screenH) const {
        glm::vec2 size = ScaledSize();
        glm::vec2 off = ScaledOffset();
        float x = 0.0f, y = 0.0f;

        switch (Anchor) {
            case UIAnchor::TopLeft: case UIAnchor::CenterLeft: case UIAnchor::BottomLeft:
                x = off.x; break;
            case UIAnchor::TopCenter: case UIAnchor::Center: case UIAnchor::BottomCenter:
                x = screenW * 0.5f - size.x * 0.5f + off.x; break;
            case UIAnchor::TopRight: case UIAnchor::CenterRight: case UIAnchor::BottomRight:
                x = screenW - size.x - off.x; break;
        }
        switch (Anchor) {
            case UIAnchor::TopLeft: case UIAnchor::TopCenter: case UIAnchor::TopRight:
                y = off.y; break;
            case UIAnchor::CenterLeft: case UIAnchor::Center: case UIAnchor::CenterRight:
                y = screenH * 0.5f - size.y * 0.5f + off.y; break;
            case UIAnchor::BottomLeft: case UIAnchor::BottomCenter: case UIAnchor::BottomRight:
                y = screenH - size.y - off.y; break;
        }
        return {x, y};
    }

    // Попадает ли точка (курсор) в прямоугольник элемента на данном экране
    bool Contains(glm::vec2 point, float screenW, float screenH) const {
        glm::vec2 p = ResolvePosition(screenW, screenH);
        glm::vec2 s = ScaledSize();
        return point.x >= p.x && point.x <= p.x + s.x &&
               point.y >= p.y && point.y <= p.y + s.y;
    }

    // Обработка ввода (для интерактивных виджетов). Вызывается UICanvas::Update
    // перед Draw. Неинтерактивные элементы (панель/текст/спрайт) не переопределяют.
    virtual void Update(const UIInputState& /*input*/, float /*screenW*/, float /*screenH*/) {}

    // Отрисовка элемента. Вызывается между ui.Begin() и ui.End().
    virtual void Draw(UIRenderer& ui) = 0;
};
