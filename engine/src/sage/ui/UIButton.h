#pragma once
#include <string>

#include <entt/entt.hpp>

#include "sage/ui/components/Layout.h"

class Scene;

// ---------------------------------------------------------------------------
// СОДЕРЖИМОЕ КНОПКИ: подпись, значок, выравнивание и отступ между ними.
//
// Подпись и значок — дети кнопки (объекты «Текст» и «Значок»), а не поля
// внутри неё: так у каждого свой шрифт, цвет по состоянию и стиль. Расставляет
// их Arrangement самой кнопки — ряд с выравниванием, — поэтому значок слева от
// подписи не задаётся координатами, которые разъедутся при смене текста.
// ---------------------------------------------------------------------------
namespace sage::ui {

struct ButtonContent {
    std::string Label;   // пусто — без подписи
    std::string Icon;    // имя значка движка; пусто — без значка
    Arrangement::Spread Alignment = Arrangement::Spread::Center;   // Start / Center / End
    float Spacing = 8.0f;
};

// Прочитать содержимое кнопки из её детей. false — это не элемент.
bool ReadButtonContent(Scene& scene, entt::entity button, ButtonContent& out);
// Привести детей кнопки к описанию: завести/убрать значок и подпись,
// выставить их текст и ряд кнопки.
void ApplyButtonContent(Scene& scene, entt::entity button, const ButtonContent& content);

} // namespace sage::ui
