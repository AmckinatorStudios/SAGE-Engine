#pragma once
#include <glm/glm.hpp>

class EditorHost;

// ---------------------------------------------------------------------------
// ПОЛЯ КОМПОНЕНТОВ ИНСПЕКТОРА СЦЕНЫ — те же контролы, что у инспектора
// интерфейса (ictl): подпись слева в общей колонке, число — [−|значение|+],
// вкл/выкл — переключатель, список и цвет — подписью слева.
//
// Подписи у ImGui стоят СПРАВА от поля и в узкой панели обрезались
// («Ближняя пло…»), а поля тянулись на всю ширину. Здесь — сигнатуры, близкие
// к ImGui, чтобы секции переводились строка в строку.
//
// ИСТОРИЯ ПРАВОК — внутри: снимок до правки (перетаскивание — одним шагом).
// Вызывающему свой TrackLastImGuiItem / PushUndoSnapshot после поля не нужен.
// ---------------------------------------------------------------------------
namespace sage::editor::ifield {

// Число с перетаскиванием: speed — как у DragFloat (на пиксель), min == max —
// без пределов.
bool Float(EditorHost& host, const char* label, float* v, float speed, float min = 0.0f, float max = 0.0f,
           const char* format = "%.3f");
bool Int(EditorHost& host, const char* label, int* v, int min, int max);
// Ограниченный диапазон — ползунок с точным полем.
bool Slider(EditorHost& host, const char* label, float* v, float min, float max, const char* format = "%.2f");
bool Check(EditorHost& host, const char* label, bool* v);
bool Combo(EditorHost& host, const char* label, int* current, const char* const items[], int count);
bool Color3(EditorHost& host, const char* label, glm::vec3* rgb);
bool Color4(EditorHost& host, const char* label, glm::vec4* rgba);

} // namespace sage::editor::ifield
