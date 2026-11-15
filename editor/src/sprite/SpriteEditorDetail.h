#pragma once
#include "imgui.h"

// ---------------------------------------------------------------------------
// Помощники окон редактора спрайтов: общий вид сегментов, подсказок и цветов.
// ---------------------------------------------------------------------------
namespace sage::editor::sprite {

// СЕГМЕНТЫ — выбор одного из нескольких одной строкой («Grid | Automatic |
// Manual», «Sprite Sheet | Nine-Slice»): все варианты видны сразу, выбранный
// залит акцентом. width <= 0 — во всю доступную ширину.
bool Segmented(const char* id, const char* const* labels, int count, int* current, float width = 0.0f);

// ПОДСКАЗКА ПАРАМЕТРА: имя, что делает и пример — у каждого поля нарезки.
// Ставится сразу после группы (BeginGroup/EndGroup) или подписи: показывается,
// если курсор задержался над последним элементом.
void ParamTip(const char* name, const char* what, const char* example);

// ПИКСЕЛИ БЕЗ РАЗМЫТИЯ. Бэкенд ImGui выбирает сглаживание своим сэмплером
// поверх настроек самой текстуры, и увеличенная картинка расплывалась.
// Между Nearest и Linear картинки рисуются по пикселям.
void Nearest(ImDrawList* dl);
void Linear(ImDrawList* dl);

// Шахматка под прозрачной картинкой.
void Checkers(ImDrawList* dl, ImVec2 a, ImVec2 b, float cell);

// Цвета холста: рамка куска, наведённого, выбранного, неверной клетки.
ImU32 FrameColor();
ImU32 HoverColor();
ImU32 SelectedColor();
ImU32 InvalidColor();
ImU32 PreviewColor();

} // namespace sage::editor::sprite
