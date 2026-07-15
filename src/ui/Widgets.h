#pragma once
#include "UIElement.h"
#include <string>
#include <functional>

// ---------------------------------------------------------------------
// Готовые виджеты UI-системы. Из них собираются худы:
//
//   UIPanel       — прямоугольная подложка (с опциональной рамкой)
//   UILabel       — текст (можно динамический — через функцию-источник)
//   UIProgressBar — полоска значения 0..1 с подписью (здоровье, прогресс...)
//
// Виджеты с динамическим содержимым принимают std::function-"источники":
// каждый кадр виджет сам спрашивает актуальное значение — игровому коду
// не нужно вручную "проталкивать" данные в интерфейс.
// ---------------------------------------------------------------------

class UIPanel : public UIElement {
public:
    glm::vec3 Color{0.06f, 0.07f, 0.1f};
    float Alpha = 0.6f;
    float OutlineThickness = 0.0f; // 0 — без рамки
    glm::vec3 OutlineColor{0.8f, 0.75f, 0.6f};
    float OutlineAlpha = 0.9f;

    void Draw(UIRenderer& ui) override {
        glm::vec2 p = ResolvePosition((float)ui.ScreenWidth(), (float)ui.ScreenHeight());
        ui.Rect(p.x, p.y, Size.x, Size.y, Color, Alpha);
        if (OutlineThickness > 0.0f) {
            ui.RectOutline(p.x, p.y, Size.x, Size.y, OutlineThickness, OutlineColor, OutlineAlpha);
        }
    }
};

class UILabel : public UIElement {
public:
    std::string Text;
    float Scale = 2.0f;
    glm::vec3 Color{1.0f};
    bool CenterInSize = false; // центрировать текст внутри Size (для заголовков панелей)

    // Необязательный динамический источник текста: если задан, каждый кадр
    // текст берётся из него (например, [&]{ return dayNight.ClockString(); })
    std::function<std::string()> TextSource;

    void Draw(UIRenderer& ui) override {
        const std::string& text = TextSource ? (m_cache = TextSource()) : Text;
        if (text.empty()) return;

        // Если Size не задан — считаем размером сам текст (для якорей справа/снизу)
        glm::vec2 effSize = Size;
        if (effSize.x <= 0.0f) effSize.x = UIRenderer::MeasureText(text, Scale);
        if (effSize.y <= 0.0f) effSize.y = 8.0f * Scale;

        glm::vec2 saved = Size;
        const_cast<UILabel*>(this)->Size = effSize;
        glm::vec2 p = ResolvePosition((float)ui.ScreenWidth(), (float)ui.ScreenHeight());
        const_cast<UILabel*>(this)->Size = saved;

        if (CenterInSize) {
            ui.TextCentered(p.x + effSize.x * 0.5f, p.y, Scale, Color, text);
        } else {
            ui.Text(p.x, p.y, Scale, Color, text);
        }
    }

private:
    std::string m_cache;
};

class UIProgressBar : public UIElement {
public:
    std::string Label;            // короткая подпись внутри полоски ("HP")
    glm::vec3 FillColor{0.4f, 0.8f, 0.4f};
    glm::vec3 BackColor{0.0f, 0.0f, 0.0f};
    float BackAlpha = 0.55f;
    float LabelScale = 1.4f;

    // Источник значения 0..1 — каждый кадр бар сам берёт актуальное значение
    std::function<float()> ValueSource;

    void Draw(UIRenderer& ui) override {
        float value = ValueSource ? glm::clamp(ValueSource(), 0.0f, 1.0f) : 0.0f;
        glm::vec2 p = ResolvePosition((float)ui.ScreenWidth(), (float)ui.ScreenHeight());

        ui.Rect(p.x - 2, p.y - 2, Size.x + 4, Size.y + 4, BackColor, BackAlpha);
        ui.Rect(p.x, p.y, Size.x * value, Size.y, FillColor, 0.95f);
        if (!Label.empty()) {
            ui.Text(p.x + 4, p.y + (Size.y - 8.0f * LabelScale) * 0.5f + 1.0f, LabelScale, {1, 1, 1}, Label);
        }
    }
};
