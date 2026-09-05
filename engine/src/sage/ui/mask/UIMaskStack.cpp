#include "sage/ui/mask/UIMaskStack.h"

#include <algorithm>
#include <cmath>

namespace sage::ui {

namespace {
// Знаковое расстояние до контура скруглённого прямоугольника: отрицательное
// внутри, ноль на контуре. Ровно та же формула, что в шейдере, — и это важно:
// «видно» (шейдер) и «кликается» (эта функция) обязаны совпадать до пикселя.
float RoundedBoxSDF(glm::vec2 p, glm::vec2 half, const UICorners& c) {
    float r = p.x > 0.0f ? (p.y > 0.0f ? c.BR : c.TR) : (p.y > 0.0f ? c.BL : c.TL);
    r = std::min(r, std::min(half.x, half.y));
    glm::vec2 q = glm::abs(p) - half + glm::vec2(r);
    return glm::length(glm::max(q, glm::vec2(0.0f))) + std::min(std::max(q.x, q.y), 0.0f) - r;
}
} // namespace

float UIMaskValue(const UIMaskEntry& e, glm::vec2 point) {
    if (!UIRectValid(e.Rect)) return 0.0f;
    float value = 0.0f;

    switch (e.Form) {
        case UIMask::Shape::Rect:
        case UIMask::Shape::RoundedRect: {
            const glm::vec2 half{e.Rect.w * 0.5f, e.Rect.h * 0.5f};
            const glm::vec2 local = point - UICenter(e.Rect);
            const UICorners c = e.Form == UIMask::Shape::Rect ? UICorners(0.0f)
                                                              : e.Radius.Clamped(e.Rect.w, e.Rect.h);
            const float d = RoundedBoxSDF(local, half, c);
            // Мягкость — это ширина перехода, а не «размытие»: значение маски
            // плавно едет от 1 внутри до 0 снаружи (§34).
            const float soft = std::max(e.Softness, 0.5f);
            value = UIClamp01(0.5f - d / (2.0f * soft));
            if (e.Softness <= 0.0f) value = d <= 0.0f ? 1.0f : 0.0f;
            break;
        }
        case UIMask::Shape::Ellipse: {
            const glm::vec2 c = UICenter(e.Rect);
            const glm::vec2 r{e.Rect.w * 0.5f, e.Rect.h * 0.5f};
            if (r.x <= 0.0f || r.y <= 0.0f) return 0.0f;
            const glm::vec2 d = (point - c) / r;
            const float dist = std::sqrt(d.x * d.x + d.y * d.y);
            if (e.Softness <= 0.0f) value = dist <= 1.0f ? 1.0f : 0.0f;
            else {
                const float edge = e.Softness / std::max(r.x, r.y);
                value = UIClamp01((1.0f - dist) / std::max(edge, 0.0001f));
            }
            break;
        }
        case UIMask::Shape::Gradient: {
            // Линейное затухание вдоль заданного угла — мягкий край списка,
            // виньетка, «растворение» панели.
            const float rad = e.GradientAngle * 3.14159265358979f / 180.0f;
            const glm::vec2 dir{std::sin(rad), -std::cos(rad)};
            const glm::vec2 local = point - UIPos(e.Rect);
            const float len = std::fabs(dir.x) * e.Rect.w + std::fabs(dir.y) * e.Rect.h;
            const float t = len > 0.0f ? UIClamp01((local.x * dir.x + local.y * dir.y) / len +
                                                   (dir.x < 0.0f || dir.y < 0.0f ? 1.0f : 0.0f))
                                       : 0.0f;
            const float a = e.GradientStart, b = e.GradientEnd;
            value = std::fabs(b - a) < 0.0001f ? (t >= b ? 1.0f : 0.0f)
                                               : UIClamp01((t - a) / (b - a));
            break;
        }
        case UIMask::Shape::Texture:
            // Значение альфы известно только на GPU. На стороне процессора
            // (попадание курсором, тесты) честно считаем маской прямоугольник
            // картинки: врать «здесь дырка» без пикселей нельзя, а отказываться
            // от попадания вовсе — значит сломать клики по фигурным кнопкам.
            value = UIContains(e.Rect, point) ? 1.0f : 0.0f;
            break;
    }

    if (e.Invert) value = 1.0f - value;
    return value;
}

int UIMaskStack::Root() {
    if (m_states.empty()) m_states.push_back(UIMaskState{});
    return 0;
}

int UIMaskStack::Push(int parentState, const UIMaskEntry& mask) {
    Root();
    const UIMaskState& parent = m_states[(size_t)parentState];
    UIMaskState next;

    const int entryIndex = (int)m_entries.size();
    m_entries.push_back(mask);

    // Прямоугольные ножницы — быстрый путь: пересечение прямоугольников
    // покрывает подавляющее большинство масок и стоит один вызов GPU.
    const bool rectLike =
        (mask.Form == UIMask::Shape::Rect) && mask.Softness <= 0.0f && !mask.Invert;

    switch (mask.Mode) {
        case UIMask::Compose::Replace:
            next.HasScissor = rectLike;
            next.Scissor = mask.Rect;
            if (!rectLike) next.Entries.push_back(entryIndex);
            break;
        case UIMask::Compose::Intersect:
        case UIMask::Compose::Multiply:
            next = parent;
            if (rectLike) {
                next.Scissor = parent.HasScissor ? UIIntersectRect(parent.Scissor, mask.Rect)
                                                 : mask.Rect;
                next.HasScissor = true;
            } else {
                // Непрямоугольная маска всё равно ограничена своим
                // прямоугольником — сузим ножницы, чтобы шейдеру досталось
                // меньше пикселей.
                next.Scissor = parent.HasScissor ? UIIntersectRect(parent.Scissor, mask.Rect)
                                                 : mask.Rect;
                next.HasScissor = true;
                next.Entries.push_back(entryIndex);
            }
            break;
        case UIMask::Compose::Subtract:
        case UIMask::Compose::Add:
            // Вычитание и объединение прямоугольником не выражаются: ножницы
            // умеют только пересечение. Такая маска уходит в шейдер целиком, а
            // область родителя сохраняется.
            next = parent;
            next.Entries.push_back(entryIndex);
            break;
    }

    m_states.push_back(std::move(next));
    return (int)m_states.size() - 1;
}

float UIMaskStack::Sample(int state, glm::vec2 point, bool forHitTest) const {
    if (state < 0 || (size_t)state >= m_states.size()) return 1.0f;
    const UIMaskState& s = m_states[(size_t)state];
    float value = 1.0f;
    if (s.HasScissor && !UIContains(s.Scissor, point)) value = 0.0f;

    for (int idx : s.Entries) {
        const UIMaskEntry& e = m_entries[(size_t)idx];
        if (forHitTest && !e.AffectsHitTest) continue;
        const float v = UIMaskValue(e, point);
        switch (e.Mode) {
            case UIMask::Compose::Replace: value = v; break;
            case UIMask::Compose::Intersect: value = std::min(value, v); break;
            case UIMask::Compose::Multiply: value *= v; break;
            case UIMask::Compose::Subtract: value = std::min(value, 1.0f - v); break;
            case UIMask::Compose::Add: value = std::max(value, v); break;
        }
    }
    return value;
}

} // namespace sage::ui
