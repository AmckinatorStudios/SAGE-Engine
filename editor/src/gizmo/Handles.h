#pragma once
#include <vector>

#include <glm/glm.hpp>

#include "imgui.h"

// ---------------------------------------------------------------------------
// РУЧКИ ВО ВЬЮПОРТЕ — общая основа для правки формы коллайдера и света.
//
// Точки захвата рисуются поверх кадра списком ImGui: им не нужна глубина, и
// они не мерцают на гранях, как линии. Тянется ручка вдоль своей оси —
// положение на оси берётся как ближайшая к лучу мыши точка этой оси, а не как
// смещение мыши в пикселях: так ход ручки совпадает с курсором при любом
// ракурсе, масштабе и повороте объекта.
// ---------------------------------------------------------------------------
namespace sage::editor::handles {

struct View {
    glm::mat4 ViewM{1.0f};
    glm::mat4 Proj{1.0f};
    ImVec2 Min{0.0f, 0.0f};    // прямоугольник картинки вида на экране
    ImVec2 Size{1.0f, 1.0f};
    ImDrawList* Draw = nullptr;
    glm::vec3 Eye{0.0f};
    bool Ortho = false;
};

// Мировая точка -> экран; false — за камерой.
bool ToScreen(const View& v, const glm::vec3& world, ImVec2& out);
// Луч из глаза через пиксель экрана.
void Ray(const View& v, ImVec2 screen, glm::vec3& origin, glm::vec3& dir);
// Параметр ближайшей к лучу точки оси p0 + axis * t (axis — единичная).
float AxisParam(const glm::vec3& p0, const glm::vec3& axis, const glm::vec3& ro, const glm::vec3& rd);
// Точка пересечения луча с плоскостью (точка p0, нормаль n); false — параллельно.
bool PlaneHit(const glm::vec3& p0, const glm::vec3& n, const glm::vec3& ro, const glm::vec3& rd, glm::vec3& out);
// Шаг привязки: 0 — без неё.
float Snap(float v, float step);

// Цвета осей — те же, что у гизмо объекта и осей сетки.
ImU32 AxisColor(int axis, float alpha = 1.0f);

struct Point {
    int Id = 0;
    glm::vec3 World{0.0f};
    ImU32 Color = IM_COL32_WHITE;
    bool Back = false;        // на обратной стороне формы: бледнее и вторым выбором
    bool Square = false;      // квадрат — перенос (центр), круг — размер
};

// Какую ручку из набора задевает мышь (-1 — никакую). Передние — первыми.
int Hovered(const View& v, const std::vector<Point>& pts, ImVec2 mouse, float radiusPx = 8.0f);
void DrawPoint(const View& v, const Point& p, bool hot, bool active);
// Подпись у курсора — текущее значение при перетаскивании.
void Label(const View& v, ImVec2 at, const char* text);

} // namespace sage::editor::handles
