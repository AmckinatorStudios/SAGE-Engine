#pragma once
#include "sage/render/Shader.h"
#include "sage/render/Font.h"
#include "sage/render/Texture.h"
#include "sage/rhi/Resources.h"
#include <memory>
#include <glm/glm.hpp>
#include <string>
#include <vector>

// ---------------------------------------------------------------------
// UIRenderer — immediate-mode UI слой движка: каждый кадр игра заново
// "описывает" интерфейс вызовами между Begin() и End(), а End() рисует всё
// минимальным числом draw call'ов поверх 3D-сцены (без depth-теста, с
// альфа-блендингом). Один общий вершинный буфер; батч рвётся на сегменты
// только там, где реально меняется состояние (текстура картинки, clip-маска).
//
// Возможности (v2 — production-набор для игровых интерфейсов):
//   • Rect / RoundedRect — прямоугольники со СКРУГЛЁННЫМИ углами (SDF в
//     шейдере: гладкий антиалиасный край при любом радиусе);
//   • RectOutline / RoundedRectOutline — рамки (скруглённые — SDF-кольцо);
//   • Image — текстурированный квад (иконки, арты, аватары) с тонированием
//     и скруглением углов;
//   • PushClipRect / PopClipRect — стек масок-ножниц: всё, что рисуется
//     внутри, обрезается прямоугольником (списки, миникарты, контейнеры);
//     вложенные маски пересекаются;
//   • Text / TextCentered — TrueType-текст (UTF-8, кириллица) с альфой.
//
//   ui.Begin(width, height);
//   ui.RoundedRect(24, 24, 220, 64, {0.1f,0.1f,0.14f}, 0.9f, 12.0f);
//   ui.RoundedRectOutline(24, 24, 220, 64, 12.0f, 2.0f, {0.9f,0.8f,0.5f});
//   ui.PushClipRect(32, 32, 204, 48);
//   ui.Text(36, 40, 2.0f, {1,1,1}, "Инвентарь");
//   ui.PopClipRect();
//   ui.End();
//
// Координаты — экранные пиксели, (0,0) в левом верхнем углу. Шрифт по
// умолчанию грузится сам (assets/fonts/sage-default.ttf → системные), при
// отсутствии — fallback на stb_easy_font (ASCII).
// ---------------------------------------------------------------------
class UIRenderer {
public:
    UIRenderer();

    // Заменить шрифт своим (.ttf/.otf). true при успехе; при ошибке шрифт не
    // меняется (лог + возврат false). pixelHeight — базовый размер запекания.
    bool SetFont(const std::string& path, float pixelHeight = 48.0f);
    // Загружен ли настоящий TrueType-шрифт (иначе — stb_easy_font fallback).
    bool HasFont() const { return m_font != nullptr; }

    UIRenderer(const UIRenderer&) = delete;
    UIRenderer& operator=(const UIRenderer&) = delete;

    void Begin(int screenWidth, int screenHeight);

    // Прямоугольник в экранных пикселях. alpha < 1 — полупрозрачный.
    void Rect(float x, float y, float w, float h, glm::vec3 color, float alpha = 1.0f);

    // Прямоугольник со скруглёнными углами (radius в пикселях; SDF-край
    // антиалиасный). radius 0 — обычный прямоугольник со сглаженным краем.
    void RoundedRect(float x, float y, float w, float h, glm::vec3 color,
                     float alpha = 1.0f, float radius = 8.0f);

    // Рамка толщиной t (четыре прямоугольника, прямые углы)
    void RectOutline(float x, float y, float w, float h, float t, glm::vec3 color, float alpha = 1.0f);

    // Скруглённая рамка: SDF-кольцо толщиной t по контуру скруглённого
    // прямоугольника (гладкие углы, в отличие от RectOutline).
    void RoundedRectOutline(float x, float y, float w, float h, float radius, float t,
                            glm::vec3 color, float alpha = 1.0f);

    // Текстурированный квад: иконки/арты. tint умножается на текстуру
    // (белый = как есть); radius скругляет углы картинки. texture обязана
    // жить до End() (кадр UI короткий — обычно ресурс из ResourceManager).
    void Image(float x, float y, float w, float h, const Texture* texture,
               glm::vec3 tint = glm::vec3(1.0f), float alpha = 1.0f, float radius = 0.0f);

    // --- Маски (ножницы): всё между Push и Pop обрезается прямоугольником.
    // Вложенные вызовы пересекаются с текущей маской. Пары обязаны сходиться
    // до End() (незакрытые маски закрываются с предупреждением в лог).
    void PushClipRect(float x, float y, float w, float h);
    void PopClipRect();

    // Текст с левым верхним углом в (x, y). scale 1.0 — "родные" ~7px глифы,
    // на практике для читаемости используем 1.5–2.5. alpha — прозрачность.
    void Text(float x, float y, float scale, glm::vec3 color, const std::string& text,
              float alpha = 1.0f);

    // Текст, отцентрированный по горизонтали относительно centerX
    void TextCentered(float centerX, float y, float scale, glm::vec3 color,
                      const std::string& text, float alpha = 1.0f);

    // Ширина строки в экранных пикселях при данном масштабе (для вёрстки).
    // Учитывает текущий шрифт (пропорциональные метрики TrueType).
    float MeasureText(const std::string& text, float scale) const;
    // Высота строки в пикселях при данном масштабе (для вертикальной вёрстки).
    float TextHeight(float scale) const { return 8.0f * scale; }

    void End();

    int ScreenWidth() const { return m_screenWidth; }
    int ScreenHeight() const { return m_screenHeight; }

private:
    // Вершина UI: позиция + цвет + UV + SDF-атрибуты скругления.
    //   UV: (-1,-1) — сплошной квад; иначе — атлас шрифта ИЛИ текстура
    //   картинки (режим задаёт сегмент). Local/Half: позиция вершины
    //   относительно центра квада и полуразмер — по ним шейдер считает
    //   SDF скруглённого прямоугольника; Half = 0 выключает SDF (глифы).
    //   Params: (радиус скругления, толщина рамки; 0 — заливка).
    struct UIVertex {
        float x, y, z;
        unsigned char r, g, b, a;
        float u, v;
        float lx, ly;   // Local
        float hx, hy;   // Half
        float radius, border; // Params
    };

    // Сегмент батча: непрерывный диапазон квадов с одним состоянием
    // (текстура картинки или атлас шрифта; текущая clip-маска).
    struct Segment {
        size_t FirstQuad = 0, QuadCount = 0;
        const Texture* Image = nullptr; // nullptr — шрифт/сплошные квады
        bool Clipped = false;
        glm::vec4 Clip{0.0f}; // x, y, w, h (экранные координаты, верхний левый угол)
    };

    void PushQuad(float x, float y, float w, float h, glm::vec3 color, float alpha,
                  float radius, float border, bool solidUv);
    void PushGlyphQuad(float x0, float y0, float x1, float y1,
                       glm::vec2 uv0, glm::vec2 uv1, glm::vec3 color, float alpha);
    void TextEasyFont(float x, float y, float scale, glm::vec3 color, const std::string& text);
    void EnsureIndexCapacity(size_t quadCount);
    // Сегмент для следующего квада с нужным состоянием (текстура/клип):
    // продолжает текущий, если состояние совпадает, иначе начинает новый.
    Segment& CurrentSegment(const Texture* image);

    std::vector<UIVertex> m_vertices; // все квады кадра (прямоугольники, картинки, глифы)
    std::vector<Segment> m_segments;
    std::vector<glm::vec4> m_clipStack; // текущие маски (уже пересечённые)
    size_t m_quadCount = 0;

    std::unique_ptr<sage::rhi::Geometry> m_geometry;
    size_t m_indexCapacity = 0;
    Shader m_shader;
    std::unique_ptr<Font> m_font;   // TrueType-шрифт (nullptr → stb_easy_font)
    // Множитель scale→пиксели для TrueType: старый API оперировал масштабами
    // ~1.5–2.5 (глифы stb_easy_font ~7px). Чтобы вёрстка игр не поехала,
    // отображаем scale в пиксельную высоту так же по порядку величины.
    float m_scaleToPixels = 8.0f;
    int m_screenWidth = 0, m_screenHeight = 0;
};
