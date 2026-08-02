#pragma once
#include <glm/glm.hpp>

// ---------------------------------------------------------------------
// Якоря UI — лёгкий заголовок без зависимостей рендера: его включают и
// immediate-mode виджеты (UIElement.h), и ECS-компонент интерфейса
// (UIElementComponent в scene/Components.h), и чистая математика вёрстки
// (ResolveAnchored — юнит-тестируется без GL).
//
// Элемент привязан к якорю РОДИТЕЛЬСКОГО прямоугольника (угол/грань/центр)
// и смещён от него на Offset В СТОРОНУ ЦЕНТРА: {16,16} у TopRight означает
// «16px от правого края, 16px от верхнего». Благодаря этому интерфейс
// автоматически держится за своё место при любом размере экрана/контейнера.
// ---------------------------------------------------------------------

enum class UIAnchor {
    TopLeft,    TopCenter,    TopRight,
    CenterLeft, Center,       CenterRight,
    BottomLeft, BottomCenter, BottomRight
};

namespace sage::ui {

// Прямоугольник в экранных пикселях, (x, y) — левый верхний угол.
struct UIRect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

// Левый верхний угол элемента размера size, заякоренного внутри parent.
inline glm::vec2 ResolveAnchored(UIAnchor anchor, glm::vec2 offset, glm::vec2 size,
                                 const UIRect& parent) {
    float x = 0.0f, y = 0.0f;
    switch (anchor) {
        case UIAnchor::TopLeft: case UIAnchor::CenterLeft: case UIAnchor::BottomLeft:
            x = parent.x + offset.x; break;
        case UIAnchor::TopCenter: case UIAnchor::Center: case UIAnchor::BottomCenter:
            x = parent.x + parent.w * 0.5f - size.x * 0.5f + offset.x; break;
        case UIAnchor::TopRight: case UIAnchor::CenterRight: case UIAnchor::BottomRight:
            x = parent.x + parent.w - size.x - offset.x; break;
    }
    switch (anchor) {
        case UIAnchor::TopLeft: case UIAnchor::TopCenter: case UIAnchor::TopRight:
            y = parent.y + offset.y; break;
        case UIAnchor::CenterLeft: case UIAnchor::Center: case UIAnchor::CenterRight:
            y = parent.y + parent.h * 0.5f - size.y * 0.5f + offset.y; break;
        case UIAnchor::BottomLeft: case UIAnchor::BottomCenter: case UIAnchor::BottomRight:
            y = parent.y + parent.h - size.y - offset.y; break;
    }
    return {x, y};
}

// ОБРАТНАЯ задача: какой Offset даст элементу размера size ЭТОТ левый верхний
// угол внутри parent. Точный обратный ход к ResolveAnchored.
//
// Нужна везде, где положение задаётся мышью, а хранится якорем: редактор
// интерфейса тянет прямоугольник, а записать обязан Offset. Складывать дельту
// с Offset напрямую нельзя — у якорей справа и снизу он растёт В ОБРАТНУЮ
// сторону, а у центральных зависит ещё и от размера. Одна формула здесь взамен
// девяти особых случаев у каждого вызывающего.
inline glm::vec2 OffsetForTopLeft(UIAnchor anchor, glm::vec2 topLeft, glm::vec2 size,
                                  const UIRect& parent) {
    glm::vec2 off(0.0f);
    switch (anchor) {
        case UIAnchor::TopLeft: case UIAnchor::CenterLeft: case UIAnchor::BottomLeft:
            off.x = topLeft.x - parent.x; break;
        case UIAnchor::TopCenter: case UIAnchor::Center: case UIAnchor::BottomCenter:
            off.x = topLeft.x - (parent.x + parent.w * 0.5f - size.x * 0.5f); break;
        case UIAnchor::TopRight: case UIAnchor::CenterRight: case UIAnchor::BottomRight:
            off.x = parent.x + parent.w - size.x - topLeft.x; break;
    }
    switch (anchor) {
        case UIAnchor::TopLeft: case UIAnchor::TopCenter: case UIAnchor::TopRight:
            off.y = topLeft.y - parent.y; break;
        case UIAnchor::CenterLeft: case UIAnchor::Center: case UIAnchor::CenterRight:
            off.y = topLeft.y - (parent.y + parent.h * 0.5f - size.y * 0.5f); break;
        case UIAnchor::BottomLeft: case UIAnchor::BottomCenter: case UIAnchor::BottomRight:
            off.y = parent.y + parent.h - size.y - topLeft.y; break;
    }
    return off;
}

} // namespace sage::ui
