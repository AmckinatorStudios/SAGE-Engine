#pragma once
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include "sage/ui/UIInteraction.h"
#include "sage/ui/UISceneSolved.h"
#include "sage/vars/Value.h"

class UIRenderer;

// ---------------------------------------------------------------------------
// ЭЛЕМЕНТЫ УПРАВЛЕНИЯ: что делают ползунок, поле ввода, переключатель и
// выпадающий список в ответ на мышь и клавиатуру, и навигация фокусом.
// Отдельно от решателя (UISceneSystem.cpp): он считает места и общий для всех
// щелчок, а здесь — поведение каждого вида элемента.
// ---------------------------------------------------------------------------
namespace sage::ui {

struct Dropdown;

// Сигнал элемента (имя из sage/scene/Signals.h): рассылает решатель после
// прохода — обработчик волен удалить элемент, а проход держит список кадра.
using SignalFn = std::function<void(entt::entity, const char*, sage::vars::Value)>;

// Где раскрытый список: прямоугольник, высота строки и сколько строк видно.
// Встаёт под полем, а если снизу не хватает экрана — над ним; по ширине не
// выходит за экран.
struct DropdownLayout {
    UIRect List;
    float Row = 0.0f;
    int Visible = 0;
};
bool DropdownListLayout(const Dropdown& d, const UIRect& field, glm::vec2 screen, DropdownLayout& out);

// Выпадающие списки. hovered — элемент под курсором. true — ввод кадра забрал
// список: ни наведения, ни нажатия, ни колеса ниже него не происходит.
bool UpdateDropdowns(entt::registry& reg, const std::vector<Solved>& items, entt::entity hovered,
                     const UIInputState& input, glm::vec2 screen, const SignalFn& fire);
// Раскрытые списки — поверх всего, после всех элементов.
void DrawDropdownOverlays(const entt::registry& reg, const std::vector<Solved>& items, glm::vec2 screen,
                          UIRenderer& ui);

// Ползунки: тянутся, пока кнопка удерживается.
void UpdateSliders(entt::registry& reg, const std::vector<Solved>& items, const UIInputState& input,
                   UIInputResult& result, const SignalFn& fire);
// Поля ввода с фокусом: набор, правка, проверка, перенос строки, Enter.
void UpdateTextInputs(entt::registry& reg, const std::vector<Solved>& items, const UIInputState& input,
                      UIInputResult& result, const SignalFn& fire);
// Навигация фокусом: Tab, стрелки, Enter. true — клавиши кадра забрала она.
bool UpdateNavigation(entt::registry& reg, const std::vector<Solved>& items, const UIInputState& input,
                      UIInputResult& result, const SignalFn& fire);
// Бегунки переключателей едут к значению.
void AnimateToggles(entt::registry& reg, const std::vector<Solved>& items, float dt);

// Шкалы (Progress): показанное значение догоняет цель с пределом скорости
// (Smoothing) и смягчением (Easing); отрезок Indeterminate бежит.
void UpdateProgress(entt::registry& reg, const std::vector<Solved>& items, float dt);

// Переключить галку/выключатель (щелчок или Enter) — с сигналом.
void FlipToggle(entt::registry& reg, entt::entity e, const SignalFn& fire);

} // namespace sage::ui
