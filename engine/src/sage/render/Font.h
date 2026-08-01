#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "sage/rhi/Resources.h"

// ---------------------------------------------------------------------------
// Font — растровый шрифт на основе TrueType (.ttf/.otf через stb_truetype).
// Запекает глифы в один атлас-текстуру (R8: покрытие в красном канале) и
// хранит метрики каждого глифа. Поддерживает Unicode (строки — UTF-8): по
// умолчанию печёт латиницу (ASCII) и кириллицу, так что игровой текст можно
// писать по-русски — прямой ответ на «полная поддержка шрифтов».
//
// Рендерит не сам Font, а UIRenderer: он спрашивает у шрифта позиции/размеры
// глифов (BuildQuads) и рисует их своими текстурными квадами из атласа.
// Один Font можно переиспользовать в разных размерах — метрики масштабируются
// от базовой высоты запекания (PixelHeight()).
// ---------------------------------------------------------------------------
class Font {
public:
    // Один глиф в атласе: UV-прямоугольник + метрики (в пикселях базового
    // размера запекания). Origin — левый-верх пера; advance — шаг до след. глифа.
    struct Glyph {
        glm::vec2 uv0{0.0f};   // левый-верх в текстурных координатах [0..1]
        glm::vec2 uv1{0.0f};   // правый-низ
        glm::vec2 size{0.0f};  // размер квада в пикселях базового размера
        glm::vec2 offset{0.0f};// смещение от пера (xoff, yoff) в пикселях
        float advance = 0.0f;  // шаг пера по X в пикселях
    };

    // Готовый к отрисовке квад одного глифа в экранных пикселях.
    struct PositionedGlyph {
        float x0, y0, x1, y1;         // прямоугольник на экране
        glm::vec2 uv0, uv1;           // область в атласе
    };

    // Загружает шрифт из .ttf/.otf. pixelHeight — базовый размер запекания
    // (крупнее = чётче при увеличении, но больше атлас). Бросает при ошибке.
    // pixelArt — шрифт нарисован по пикселям (Minecraft, Sprout Lands и любой
    // другой из набора). Тогда атлас берётся Nearest и БЕЗ мип-уровней, а
    // масштаб на отрисовке округляется до целого: полупиксельное увеличение
    // растягивает одни штрихи буквы на два экранных пикселя, а соседние на
    // один, и ровный шрифт идёт волнами — ровно то, что видно как «артефакты».
    static std::unique_ptr<Font> Load(const std::string& path, float pixelHeight = 48.0f,
                                      bool pixelArt = false);

    bool IsPixelArt() const { return m_pixelArt; }

    // Раскладывает UTF-8 строку в квады, начиная с (startX, startY) —
    // startY — координата ВЕРХА строки. scale домножает базовый размер.
    // Возвращает перо в конце (для продолжения строки). out дополняется.
    float BuildQuads(const std::string& utf8, float startX, float startY, float scale,
                     std::vector<PositionedGlyph>& out) const;

    // Ширина строки в пикселях при данном масштабе.
    float MeasureWidth(const std::string& utf8, float scale) const;
    // Высота строки (для вертикальной вёрстки) при данном масштабе.
    float LineHeight(float scale) const { return m_lineHeight * scale; }
    // Высота над базовой линией (ascent) — для выравнивания по базовой линии.
    float Ascent(float scale) const { return m_ascent * scale; }

    float PixelHeight() const { return m_pixelHeight; }
    const sage::rhi::Texture2D& Atlas() const { return *m_atlas; }

    // Есть ли в атласе глиф для символа. Нужна не рисованию (оно и так
    // подставит «?»), а тому, кто ВЫБИРАЕТ шрифт: пиксельные наборы почти
    // никогда не несут кириллицы, и молча получить экран из вопросительных
    // знаков — худший из возможных ответов.
    // ПРЯМОЙ поиск по таблице, не через Find: тот подставляет '?' для всего
    // неизвестного и потому «находит» любой символ — на такой проверке
    // отсутствие кириллицы не отличить от её наличия.
    bool HasGlyph(uint32_t codepoint) const { return m_glyphs.count(codepoint) != 0; }

private:
    const Glyph* Find(uint32_t codepoint) const;

    std::unordered_map<uint32_t, Glyph> m_glyphs;
    bool m_pixelArt = false;
    std::unique_ptr<sage::rhi::Texture2D> m_atlas;
    float m_pixelHeight = 48.0f;
    float m_lineHeight = 0.0f; // ascent - descent + lineGap, в пикселях базового размера
    float m_ascent = 0.0f;
    float m_atlasW = 0.0f, m_atlasH = 0.0f;
};

// Декодирование UTF-8: возвращает следующий codepoint, продвигая i. Битые
// байты трактуются как U+FFFD. Вынесено в заголовок — пригодится и вне Font.
uint32_t Utf8Next(const std::string& s, size_t& i);
