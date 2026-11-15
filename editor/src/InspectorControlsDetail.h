#pragma once
// ---------------------------------------------------------------------------
// ВНУТРЕННЕЕ EDITOR UI KIT: общее для файлов InspectorControls*.cpp.
//
// Снимок истории, рисование рамки и разделителей, кнопки −/+ — одним кодом на
// все контролы, чтобы число, вектор, список и группа кнопок не расходились в
// мелочах (высота, скругление, цвет наведения).
// ---------------------------------------------------------------------------
#include "imgui.h"

#include "EditorTheme.h"
#include "InspectorControls.h"

class EditorHost;

namespace sage::editor::ictl::detail {

// Снимок перед правкой — в историю инструмента, если он её подменил (UndoScope).
void Snapshot(EditorHost* host);   // host может быть nullptr: тогда — только UndoScope
// Перетаскивание и ввод: один шаг истории на всё касание.
void Track(EditorHost* host);

inline const EditorTheme::Kit& K() { return EditorTheme::KitMetrics(); }

// Общая рамка контрола: заливка и (при фокусе) акцентная обводка.
void Frame(ImDrawList* dl, ImVec2 a, ImVec2 b, EditorTheme::State s, ImDrawFlags corners = 0);
// Тонкий вертикальный разделитель внутри рамки высотой h — между сегментами.
void InnerSeparator(ImDrawList* dl, float x, float y, float h);
// Тонкий горизонтальный разделитель внутри рамки шириной w — между рядами.
void InnerSeparatorH(ImDrawList* dl, float x, float y, float w);

// Число внутри готовой рамки: [−] значение [+] (buttons) или только
// значение. Рамку рисует вызывающий; corners — какие углы у кнопок крайние.
// active — поле сейчас правят (для рамки фокуса).
bool NumberIn(EditorHost* host, const char* id, float* v, const NumOpts& o, ImVec2 p, float width, bool buttons,
              ImDrawFlags leftCorners, ImDrawFlags rightCorners, bool* active);

// Ширина кнопки −/+ при высоте h и ширине поля width.
float StepSide(float width, float h);
// Помещаются ли −/+ в поле шириной width (рядом с числом DigitRoom).
bool StepFits(float width, float h);
// Сколько места нужно самому числу («-000.000» текущим шрифтом): меньше —
// цифры обрезаются, и вместо «-1.600» видно «-1.60(».
float DigitRoom();

// Метка оси/стороны («X», «W», «Min») цветной плашкой в точке p высотой h.
// axisColor < 0 — нейтральная метка.
void TagPill(ImDrawList* dl, ImVec2 p, float w, float h, const char* text, int axisColor);
float TagWidth(const char* const* tags, int n);

std::string PrefKey(const char* what, ImGuiID id);

// Вектор без подписи; host может быть nullptr (история — через UndoScope).
bool VectorImpl(EditorHost* host, const char* id, float* v, int n, const NumOpts& o, const VecOpts& vo);

} // namespace sage::editor::ictl::detail
