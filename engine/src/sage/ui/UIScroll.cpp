#include "sage/ui/UIScroll.h"

#include <algorithm>
#include <unordered_map>

#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIMasks.h"
#include "UIRenderer.h"

namespace sage::ui {
namespace {

bool In(const UIRect& r, glm::vec2 p) { return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h; }

// Порог, после которого нажатие в списке — уже прокрутка, а не щелчок. В
// экранных пикселях: дрожь пальца не зависит от масштаба холста.
constexpr float kDragThreshold = 6.0f;
// Бегунок не короче этого: у списка на тысячу строк он иначе стал бы точкой,
// в которую не попасть.
constexpr float kMinThumb = 16.0f;

bool AxisOn(const Scroll& s, int axis) { return axis == 0 ? s.Horizontal : s.Vertical; }

float MaxOffset(const Scroll& s, const UIRect& rect, float scale, int axis) {
    const float view = (axis == 0 ? rect.w : rect.h) / std::max(scale, 1e-4f);
    return std::max(0.0f, s.Content[axis] - view);
}

bool HasBar(const Scroll& s, const UIRect& rect, float scale, int axis) {
    return s.Scrollbar && AxisOn(s, axis) && s.Content[axis] > 0.0f && MaxOffset(s, rect, scale, axis) > 0.5f;
}

} // namespace

bool ScrollbarRects(const Scroll& s, const UIRect& rect, float scale, int axis, UIRect& track,
                    UIRect& thumb) {
    if (!HasBar(s, rect, scale, axis)) return false;
    const float maxOff = MaxOffset(s, rect, scale, axis);
    const float w = std::max(1.0f, s.ScrollbarWidth) * scale;
    // Две полосы не накрывают друг друга: каждая кончается там, где
    // начинается другая.
    const float corner = HasBar(s, rect, scale, 1 - axis) ? w : 0.0f;
    if (axis == 1) track = {rect.x + rect.w - w, rect.y, w, std::max(0.0f, rect.h - corner)};
    else track = {rect.x, rect.y + rect.h - w, std::max(0.0f, rect.w - corner), w};
    const float len = axis == 1 ? track.h : track.w;
    const float view = (axis == 0 ? rect.w : rect.h) / std::max(scale, 1e-4f);
    const float thumbLen = std::min(len, std::max(kMinThumb * scale, len * view / s.Content[axis]));
    const float t = glm::clamp(s.Offset[axis] / maxOff, 0.0f, 1.0f);
    const float at = (len - thumbLen) * t;
    if (axis == 1) thumb = {track.x, track.y + at, track.w, thumbLen};
    else thumb = {track.x + at, track.y, thumbLen, track.h};
    return true;
}

ScrollInput UpdateScrolling(entt::registry& reg, const std::vector<Solved>& items,
                            const UIInputState& input, UIInputResult& result) {
    ScrollInput out;
    auto reachable = [&](const Solved& it) {
        if (it.Clipped && !In(it.Clip, input.Mouse)) return false;
        return MaskAccepts(reg, it.MaskOwner, it.MaskRect, input.Mouse);
    };

    // --- КОЛЕСО --------------------------------------------------------------
    //
    // САМЫЙ ВЕРХНИЙ прокручиваемый под курсором, ловит ли он мышь или нет:
    // список обычно — панель с кнопками внутри, и «крутится только то, что
    // нажимается» смешало бы два разных вопроса. Вложенные: крутится
    // внутренний — items идут в порядке отрисовки, последний совпавший глубже.
    if (input.Wheel != 0.0f) {
        Scroll* target = nullptr;
        for (const Solved& it : items) {
            if (!reachable(it) || !In(it.Rect, input.Mouse)) continue;
            if (Scroll* sc = reg.try_get<Scroll>(it.Entity)) target = sc;
        }
        if (target) {
            // Вертикаль в приоритете: колесо у мыши одно.
            if (target->Vertical) target->Offset.y -= input.Wheel * target->Speed;
            else if (target->Horizontal) target->Offset.x -= input.Wheel * target->Speed;
            // Щелчок колеса съеден: иначе он же отъедет камерой сцены.
            result.WantsMouse = true;
        }
    }

    // --- НАЖАТИЕ: полоса или начало перетаскивания содержимого ---------------
    if (input.MousePressed) {
        Scroll* bar = nullptr;
        const Solved* barItem = nullptr;
        int barAxis = -1;
        bool onThumb = false;
        Scroll* content = nullptr;
        for (const Solved& it : items) {
            Scroll* sc = reg.try_get<Scroll>(it.Entity);
            if (!sc || !reachable(it) || !In(it.Rect, input.Mouse)) continue;
            // Самый глубокий выигрывает, как и у колеса: перебираем все.
            for (int a = 0; a < 2; ++a) {
                UIRect track, thumb;
                if (!ScrollbarRects(*sc, it.Rect, it.Scale, a, track, thumb) || !In(track, input.Mouse)) continue;
                bar = sc;
                barItem = &it;
                barAxis = a;
                onThumb = In(thumb, input.Mouse);
            }
            if (sc->DragContent) content = sc;
        }
        if (bar) {
            if (!onThumb) {
                // Щелчок по дорожке листает на страницу к курсору — как в любой
                // системной полосе.
                UIRect track, thumb;
                ScrollbarRects(*bar, barItem->Rect, barItem->Scale, barAxis, track, thumb);
                const float page = (barAxis == 0 ? barItem->Rect.w : barItem->Rect.h) /
                                   std::max(barItem->Scale, 1e-4f);
                const bool before = barAxis == 0 ? input.Mouse.x < thumb.x : input.Mouse.y < thumb.y;
                bar->Offset[barAxis] += before ? -page : page;
            }
            bar->Dragging = barAxis == 0 ? Scroll::Drag::ThumbX : Scroll::Drag::ThumbY;
            bar->DragMouse = input.Mouse;
            bar->DragOffset = bar->Offset;
            out.BlockPress = true;
            result.WantsMouse = true;
        } else if (content) {
            content->Dragging = Scroll::Drag::ContentPending;
            content->DragMouse = input.Mouse;
            content->DragOffset = content->Offset;
        }
    }

    // --- ТЯНУТ ---------------------------------------------------------------
    for (const Solved& it : items) {
        Scroll* sc = reg.try_get<Scroll>(it.Entity);
        if (!sc || sc->Dragging == Scroll::Drag::None) continue;
        if (!input.MouseDown) {
            sc->Dragging = Scroll::Drag::None;
            continue;
        }
        const glm::vec2 moved = input.Mouse - sc->DragMouse;
        const float k = std::max(it.Scale, 1e-4f);
        switch (sc->Dragging) {
        case Scroll::Drag::ThumbX:
        case Scroll::Drag::ThumbY: {
            // Бегунок идёт за мышью: сдвиг мыши по дорожке переводится в сдвиг
            // содержимого в той же пропорции, в какой бегунок меньше дорожки.
            const int a = sc->Dragging == Scroll::Drag::ThumbX ? 0 : 1;
            UIRect track, thumb;
            if (!ScrollbarRects(*sc, it.Rect, it.Scale, a, track, thumb)) break;
            const float free = (a == 0 ? track.w - thumb.w : track.h - thumb.h);
            if (free > 0.0f)
                sc->Offset[a] = sc->DragOffset[a] + moved[a] / free * MaxOffset(*sc, it.Rect, it.Scale, a);
            result.WantsMouse = true;
            break;
        }
        case Scroll::Drag::ContentPending:
            if (std::abs(sc->Horizontal ? moved.x : 0.0f) < kDragThreshold &&
                std::abs(sc->Vertical ? moved.y : 0.0f) < kDragThreshold)
                break;
            sc->Dragging = Scroll::Drag::Content;
            [[fallthrough]];
        case Scroll::Drag::Content:
            // Содержимое едет ЗА пальцем: тянут вверх — список уходит вверх.
            if (sc->Horizontal) sc->Offset.x = sc->DragOffset.x - moved.x / k;
            if (sc->Vertical) sc->Offset.y = sc->DragOffset.y - moved.y / k;
            out.Dragging = true;
            result.WantsMouse = true;
            break;
        case Scroll::Drag::None: break;
        }
    }
    return out;
}

std::vector<std::pair<size_t, size_t>> ScrollbarSlots(Scene& scene, const std::vector<Solved>& items) {
    std::vector<std::pair<size_t, size_t>> slots;
    const entt::registry& reg = scene.Registry();
    std::unordered_map<entt::entity, size_t> scrollAt;
    for (size_t i = 0; i < items.size(); ++i)
        if (const Scroll* sc = reg.try_get<Scroll>(items[i].Entity); sc && sc->Scrollbar)
            scrollAt[items[i].Entity] = i;
    if (scrollAt.empty()) return slots;
    // Последний нарисованный потомок каждого прокручиваемого (или он сам).
    std::unordered_map<entt::entity, size_t> last;
    for (size_t i = 0; i < items.size(); ++i)
        for (entt::entity a = items[i].Entity; a != entt::null && reg.valid(a); a = scene.ParentOf(a))
            if (scrollAt.count(a)) last[a] = i;
    for (const auto& [e, end] : last) slots.push_back({end, scrollAt[e]});
    std::sort(slots.begin(), slots.end());
    return slots;
}

void DrawScrollbars(const entt::registry& reg, const Solved& it, UIRenderer& ui) {
    const Scroll* sc = reg.try_get<Scroll>(it.Entity);
    if (!sc) return;
    for (int a = 0; a < 2; ++a) {
        UIRect track, thumb;
        if (!ScrollbarRects(*sc, it.Rect, it.Scale, a, track, thumb)) continue;
        const float radius = std::min(track.w, track.h) * 0.5f;
        if (sc->ScrollbarTrack.a > 0.0f)
            ui.RoundedRect(track.x, track.y, track.w, track.h, glm::vec3(sc->ScrollbarTrack),
                           sc->ScrollbarTrack.a * it.Alpha, radius);
        ui.RoundedRect(thumb.x, thumb.y, thumb.w, thumb.h, glm::vec3(sc->ScrollbarColor),
                       sc->ScrollbarColor.a * it.Alpha, radius);
    }
}

} // namespace sage::ui
