#pragma once
#include <set>
#include <string>
#include <vector>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include "sage/anim/Tween.h"

// ПОДПИСИ И ПОДСКАЗКИ ТВИНОВ — то, что нужно и окну Tween, и инспектору, и
// самопроверке. Отдельным модулем, чтобы инспектор не зависел от самого окна.
// То, что окну нужно и снаружи (инспектор, самопроверка).
namespace tweenui {

// Свойство в меню «+ Добавить свойство»: только то, что у объекта ЕСТЬ, и
// человеческим названием. Section — раздел меню (Transform, Rendering,
// Interface, Camera, Audio); пусто — редкое, уходит под «Ещё».
struct Choice {
    const sage::anim::PropertyType* Property = nullptr;
    const char* Section = "";
    std::string Title;   // уже переведено
};
std::vector<Choice> Choices(const entt::registry& reg, entt::entity e);

// Название свойства для человека («Цвет текста», а не «label.color»).
std::string TitleOf(const std::string& propertyId);
// Название кривой для человека («Плавное замедление», а не «quad-out»).
std::string EaseName(const sage::anim::Ease& e);
// Конец нового твина: заметно отличный от нынешнего значения (позиция +1 по
// Y, масштаб ×1.5, непрозрачность → 0…), чтобы Play сразу что-то показал.
glm::vec4 SuggestedEnd(const std::string& propertyId, const glm::vec4& current);
// Сводка твина для инспектора: «2 свойства · 1.6 с».
std::string Summary(const sage::anim::TweenClip& clip);

} // namespace tweenui
