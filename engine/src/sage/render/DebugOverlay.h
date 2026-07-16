#pragma once
#include "Shader.h"
#include "sage/rhi/Resources.h"
#include <memory>
#include <glm/glm.hpp>
#include <string>
#include <vector>

// Одна строка отладочного HUD'а: текст + цвет (белый по умолчанию,
// но можно подсветить, например, FPS красным при просадке).
struct DebugLine {
    std::string Text;
    glm::vec3 Color{1.0f, 1.0f, 1.0f};

    DebugLine(std::string text, glm::vec3 color = {1.0f, 1.0f, 1.0f})
        : Text(std::move(text)), Color(color) {}
};

// Отладочный HUD в духе Minecraft-F3: список строк "ключ: значение" поверх
// кадра. Текст рисуется векторно через stb_easy_font (без шрифтовых
// текстур — геометрия генерируется на лету), поэтому шрифт нарочно
// простой и техничный — не для игрового UI, а для служебной инфы.
class DebugOverlay {
public:
    DebugOverlay();

    DebugOverlay(const DebugOverlay&) = delete;
    DebugOverlay& operator=(const DebugOverlay&) = delete;

    void Draw(const std::vector<DebugLine>& lines, int screenWidth, int screenHeight);

private:
    std::unique_ptr<sage::rhi::Geometry> m_geometry;
    size_t m_indexCapacity = 0; // сколько индексов уже загружено в индексный буфер
    Shader m_shader;
};
