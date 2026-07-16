#pragma once
#include "sage/render/Shader.h"
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
// Текст — векторный stb_easy_font (ASCII-only: кириллицу не умеет,
// поэтому все игровые надписи должны быть на английском/транслите).
// Координаты — экранные пиксели, (0,0) в левом верхнем углу.
// ---------------------------------------------------------------------
class UIRenderer {
public:
    UIRenderer();
    ~UIRenderer();

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

    // Ширина строки в экранных пикселях при данном масштабе (для вёрстки)
    static float MeasureText(const std::string& text, float scale);

    void End();

    int ScreenWidth() const { return m_screenWidth; }
    int ScreenHeight() const { return m_screenHeight; }

private:
    struct UIVertex {
        float x, y, z;
        unsigned char r, g, b, a;
    };

    void PushQuad(float x, float y, float w, float h, glm::vec3 color, float alpha);
    void EnsureIndexCapacity(size_t quadCount);

    std::vector<UIVertex> m_vertices; // все квады кадра (и прямоугольники, и глифы текста)
    size_t m_quadCount = 0;

    unsigned int m_vao = 0, m_vbo = 0, m_ebo = 0;
    size_t m_indexCapacity = 0;
    Shader m_shader;
    int m_screenWidth = 0, m_screenHeight = 0;
};
