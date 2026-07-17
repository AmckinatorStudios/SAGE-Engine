#pragma once
#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include "sage/rhi/GraphicsDevice.h"

// ---------------------------------------------------------------------------
// DebugDraw — движковая система гизмо/отладочной отрисовки: батч 3D-линий,
// рисуемых В МИРЕ, с настоящим тестом глубины (в отличие от 2D-оверлеев
// вроде ImGuizmo::DrawGrid, которые рисуются поверх готовой картинки и
// «просвечивают» сквозь геометрию — именно так выглядел старый баг сетки
// редактора).
//
// Использование (каждый кадр, после отрисовки сцены в тот же буфер):
//   dbg.Grid({0,0,0}, 12.0f, 1.0f, {0.45f, 0.45f, 0.5f});
//   dbg.WireBox(transform.GetMatrix(), {1.0f, 0.6f, 0.1f}); // подсветка выбора
//   dbg.Axes(transform.GetMatrix(), 1.5f);                  // оси X/Y/Z
//   dbg.Flush(view, proj); // один draw call на весь батч, очищает батч
//
// Линии проходят тест глубины (объекты сцены их заслоняют), но сами глубину
// НЕ пишут (SetDepthWrite(false) на время Flush) — гизмо не должны заслонять
// то, что рисуется после них. Шейдер встроен строкой — внешних ассетов нет,
// система работает в любом приложении на движке (редактор, игры).
// Часть ЯДРА рендера — о конкретной игре ничего не знает.
// ---------------------------------------------------------------------------
class DebugDraw {
public:
    DebugDraw();

    DebugDraw(const DebugDraw&) = delete;
    DebugDraw& operator=(const DebugDraw&) = delete;

    void Line(glm::vec3 a, glm::vec3 b, glm::vec3 color);

    // Сетка-ориентир в плоскости XZ: клетки размером step внутри квадрата
    // halfExtent от center; оси X и Z подсвечиваются красным/синим.
    void Grid(glm::vec3 center, float halfExtent, float step, glm::vec3 color);

    // Каркас единичного куба (от -0.5 до +0.5), преобразованного матрицей —
    // готовая подсветка сущности с кубическим мешем/AABB.
    void WireBox(const glm::mat4& transform, glm::vec3 color);
    void WireBox(glm::vec3 center, glm::vec3 halfExtents, glm::vec3 color);

    // Каркасная сфера: три больших круга (XY/XZ/YZ).
    void WireSphere(glm::vec3 center, float radius, glm::vec3 color, int segments = 32);

    // Каркасный конус прожектора: от apex вдоль dir на длину length, радиус
    // основания задан полууглом halfAngleDeg — готовая визуализация зоны spot.
    void WireCone(glm::vec3 apex, glm::vec3 dir, float length, float halfAngleDeg,
                  glm::vec3 color, int segments = 20);

    // Каркас усечённой пирамиды камеры (frustum): из apex вдоль dir, с
    // вертикальным углом fovDeg и соотношением сторон aspect, от near до far.
    // Готовая иконка-гизмо для сущности-камеры.
    void WireFrustum(glm::vec3 apex, glm::vec3 dir, float fovDeg, float aspect,
                     float nearDist, float farDist, glm::vec3 color);

    // Оси объекта: X — красная, Y — зелёная, Z — синяя.
    void Axes(const glm::mat4& transform, float size = 1.0f);

    bool Empty() const { return m_vertices.empty(); }

    // Рисует накопленный батч и очищает его. Вызывать после отрисовки сцены
    // в тот же кадровый буфер (иначе тесту глубины не с чем сравнивать).
    void Flush(const glm::mat4& view, const glm::mat4& proj);

private:
    struct LineVertex {
        glm::vec3 Pos;
        glm::vec3 Color;
    };

    std::vector<LineVertex> m_vertices;
    std::unique_ptr<sage::rhi::ShaderProgram> m_shader;
    std::unique_ptr<sage::rhi::Geometry> m_geometry;
};
