#pragma once
#include "gizmo/ColliderEdit.h"
#include "gizmo/Handles.h"

// ---------------------------------------------------------------------------
// Ручки формы коллайдера во вьюпорте (режим «правка коллайдера», клавиша C).
//
// Коробка — шесть кругов на гранях (цвет — ось), шар — по одному на ось с
// каждой стороны, капсула — радиус по бокам и высота сверху и снизу; квадрат в
// центре — сдвиг центра формы. Тянется грань — противоположная стоит, Alt —
// симметрично. Отдельно от масштаба объекта: правится сам коллайдер, а модель
// остаётся какой была.
// ---------------------------------------------------------------------------
namespace sage::editor {

class ColliderHandles {
public:
    struct Result {
        bool WantMouse = false;   // мышь над ручкой или ручку тянут: выбор и гизмо объекта молчат
        bool Began = false;       // нажали на ручку в этом кадре (снимок для отмены)
        bool Changed = false;     // форма изменилась
        bool Ended = false;       // отпустили (запись отмены)
    };

    // world — мировая матрица объекта (с масштабом). snapMove — шаг привязки
    // по осям объекта, 0 — без привязки. allowInput — мышь над картинкой вида и
    // не занята другим инструментом.
    Result Update(ColliderComponent& col, const glm::mat4& world, const handles::View& view, bool allowInput,
                  const glm::vec3& snapMove);

    bool Dragging() const { return m_active != kNone; }

    // Что нарисовано в прошлом кадре: вид и ручки (номер — ось * 2 + сторона,
    // центр — 100). Нужно самопроверке, чтобы целиться мышью ровно в видимую
    // точку, а не пересчитывать её по своим матрицам.
    const handles::View& LastView() const { return m_lastView; }
    const std::vector<handles::Point>& LastPoints() const { return m_lastPoints; }

private:
    static constexpr int kNone = -100;
    static constexpr int kCenter = 100;   // ручки размера — 0..5 (ось * 2 + сторона)
    int m_active = kNone;
    ColliderComponent m_start;
    glm::vec3 m_startWorld{0.0f};
    glm::vec3 m_axisWorld{0.0f};
    float m_startParam = 0.0f;
    handles::View m_lastView;
    std::vector<handles::Point> m_lastPoints;
};

} // namespace sage::editor
