#pragma once
#include <utility>
#include <vector>

#include "sage/ui/UIInteraction.h"
#include "sage/ui/UISceneSolved.h"

class Scene;
class UIRenderer;

// ---------------------------------------------------------------------------
// ПРОКРУТКА: колесо, полоса прокрутки (бегунок и дорожка) и перетаскивание
// самого содержимого. Отдельно от решателя (UISceneSystem.cpp): он считает
// места, а здесь — то, что человек делает руками с уже посчитанным.
// ---------------------------------------------------------------------------
namespace sage::ui {

struct Scroll;

// Дорожка и бегунок полосы по оси (0 — горизонтальная, 1 — вертикальная) в
// экранных координатах. false — полосы нет: выключена, ось не крутится или
// содержимое и так помещается.
bool ScrollbarRects(const Scroll& s, const UIRect& rect, float scale, int axis, UIRect& track,
                    UIRect& thumb);

// Что прокрутка сделала с вводом этого кадра.
struct ScrollInput {
    bool BlockPress = false;   // нажали на полосу — элементу под ней это не нажатие
    bool Dragging = false;     // тянут содержимое: нажатое не щёлкает
};

// Колесо, полосы и перетаскивание. items — решённые элементы кадра (видимые).
ScrollInput UpdateScrolling(entt::registry& reg, const std::vector<Solved>& items,
                            const UIInputState& input, UIInputResult& result);

// Где рисовать полосы: пары (после какого элемента списка, чью полосу) —
// сразу за последним потомком прокручиваемого, чтобы полоса легла поверх его
// содержимого, но под тем, что нарисовано позже (окно поверх списка).
std::vector<std::pair<size_t, size_t>> ScrollbarSlots(Scene& scene, const std::vector<Solved>& items);

// Нарисовать полосы элемента items[index].
void DrawScrollbars(const entt::registry& reg, const Solved& it, UIRenderer& ui);

} // namespace sage::ui
