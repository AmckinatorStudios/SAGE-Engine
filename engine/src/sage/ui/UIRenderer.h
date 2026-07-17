#pragma once
#include "sage/render/Shader.h"
#include "sage/render/Font.h"
#include "sage/rhi/Resources.h"
#include <memory>
#include <glm/glm.hpp>
#include <string>
#include <vector>

// ---------------------------------------------------------------------
// UIRenderer — простой immediate-mode UI слой движка: каждый кадр игра
// заново "описывает" интерфейс вызовами Rect/Text/TextCentered между
// Begin() и End(), а End() рисует всё одним-двумя draw call'ами поверх
// 3D-сцены (без depth-теста, с альфа-блендингом).
//
//   ui.Begin(width, height);
//   ui.Rect(10, 10, 200, 20, {0,0,0}, 0.5f);          // подложка
//   ui.Rect(12, 12, 196 * hp, 16, {0.8f,0.2f,0.2f});  // полоска здоровья
//   ui.Text(16, 13, 1.5f, {1,1,1}, "Health");
//   ui.End();
//
// Текст рендерится настоящим TrueType-шрифтом (класс Font, атлас глифов):
// поддерживается Unicode/UTF-8, включая кириллицу — игровые надписи можно
// писать по-русски. Конструктор пытается загрузить шрифт по умолчанию
// (assets/fonts/sage-default.ttf, затем системные); если ни один не найден,
// откатывается на встроенный векторный stb_easy_font (ASCII-only) — движок
// продолжает рисовать текст в любом случае. SetFont() задаёт свой шрифт.
// Координаты — экранные пиксели, (0,0) в левом верхнем углу.
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

    // Рамка толщиной t (четыре прямоугольника)
    void RectOutline(float x, float y, float w, float h, float t, glm::vec3 color, float alpha = 1.0f);

    // Текст с левым верхним углом в (x, y). scale 1.0 — "родные" ~7px глифы,
    // на практике для читаемости используем 1.5–2.5.
    void Text(float x, float y, float scale, glm::vec3 color, const std::string& text);

    // Текст, отцентрированный по горизонтали относительно centerX
    void TextCentered(float centerX, float y, float scale, glm::vec3 color, const std::string& text);

    // Ширина строки в экранных пикселях при данном масштабе (для вёрстки).
    // Учитывает текущий шрифт (пропорциональные метрики TrueType).
    float MeasureText(const std::string& text, float scale) const;

    void End();

    int ScreenWidth() const { return m_screenWidth; }
    int ScreenHeight() const { return m_screenHeight; }

private:
    // Вершина UI: позиция + цвет + UV атласа шрифта. Для сплошных
    // прямоугольников UV = (-1,-1) — сигнал шейдеру брать сплошной цвет
    // (без выборки из атласа); для глифов — реальные координаты в атласе.
    struct UIVertex {
        float x, y, z;
        unsigned char r, g, b, a;
        float u, v;
    };

    void PushQuad(float x, float y, float w, float h, glm::vec3 color, float alpha);
    void PushGlyphQuad(float x0, float y0, float x1, float y1,
                       glm::vec2 uv0, glm::vec2 uv1, glm::vec3 color);
    void TextEasyFont(float x, float y, float scale, glm::vec3 color, const std::string& text);
    void EnsureIndexCapacity(size_t quadCount);

    std::vector<UIVertex> m_vertices; // все квады кадра (и прямоугольники, и глифы текста)
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
