#pragma once
#include "UIElement.h"
#include <string>
#include <functional>
#include <algorithm>

// ---------------------------------------------------------------------
// Готовые виджеты UI-системы. Из них собираются худы и меню:
//
//   UIPanel       — прямоугольная подложка (с опциональной рамкой)
//   UILabel       — текст (можно динамический — через функцию-источник)
//   UIProgressBar — полоска значения 0..1 с подписью (здоровье, прогресс...)
//   UISprite      — текстурная иконка/картинка (с опц. сохранением пропорций)
//   UIButton      — кнопка: реагирует на наведение/нажатие, зовёт OnClick
//   UIToggle      — переключатель on/off (галочка/тумблер), зовёт OnChanged
//
// Виджеты с динамическим содержимым принимают std::function-"источники":
// каждый кадр виджет сам спрашивает актуальное значение — игровому коду не
// нужно вручную "проталкивать" данные в интерфейс. Интерактивные виджеты
// (кнопка/переключатель) обрабатывают ввод в Update() (см. UICanvas::Update).
//
// Все размеры/отступы/текст масштабируются LayoutScale (см. UIElement/
// UICanvas::ReferenceSize) — интерфейс одинаково выглядит при разном
// соотношении сторон и разрешении.
// ---------------------------------------------------------------------

class UIPanel : public UIElement {
public:
    glm::vec3 Color{0.06f, 0.07f, 0.1f};
    float Alpha = 0.6f;
    float OutlineThickness = 0.0f; // 0 — без рамки (в пикселях до масштаба)
    glm::vec3 OutlineColor{0.8f, 0.75f, 0.6f};
    float OutlineAlpha = 0.9f;

    void Draw(UIRenderer& ui) override {
        glm::vec2 p = ResolvePosition((float)ui.ScreenWidth(), (float)ui.ScreenHeight());
        glm::vec2 s = ScaledSize();
        ui.Rect(p.x, p.y, s.x, s.y, Color, Alpha);
        if (OutlineThickness > 0.0f) {
            ui.RectOutline(p.x, p.y, s.x, s.y, OutlineThickness * LayoutScale, OutlineColor, OutlineAlpha);
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

        float effScale = Scale * LayoutScale;
        // Если Size не задан — считаем размером сам текст (для якорей справа/снизу)
        glm::vec2 effSize = ScaledSize();
        if (effSize.x <= 0.0f) effSize.x = UIRenderer::MeasureText(text, effScale);
        if (effSize.y <= 0.0f) effSize.y = 8.0f * effScale;

        // Позиционируем по effSize: временно подменяем Size так, чтобы
        // ScaledSize() дал ровно effSize (LayoutScale > 0 всегда).
        glm::vec2 saved = Size;
        float ls = std::max(LayoutScale, 1e-6f);
        Size = effSize / ls;
        glm::vec2 p = ResolvePosition((float)ui.ScreenWidth(), (float)ui.ScreenHeight());
        Size = saved;

        if (CenterInSize) {
            ui.TextCentered(p.x + effSize.x * 0.5f, p.y, effScale, Color, text);
        } else {
            ui.Text(p.x, p.y, effScale, Color, text);
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
        glm::vec2 s = ScaledSize();
        float ls = LayoutScale;

        ui.Rect(p.x - 2 * ls, p.y - 2 * ls, s.x + 4 * ls, s.y + 4 * ls, BackColor, BackAlpha);
        ui.Rect(p.x, p.y, s.x * value, s.y, FillColor, 0.95f);
        if (!Label.empty()) {
            float ts = LabelScale * ls;
            ui.Text(p.x + 4 * ls, p.y + (s.y - 8.0f * ts) * 0.5f + ls, ts, {1, 1, 1}, Label);
        }
    }
};

// Текстурный спрайт (иконка/картинка). KeepAspect вписывает текстуру в Size,
// сохраняя её соотношение сторон (letterbox по центру) — картинка не тянется.
class UISprite : public UIElement {
public:
    const Texture* SpriteTexture = nullptr;
    glm::vec4 Tint{1.0f};
    glm::vec4 UV{0.0f, 0.0f, 1.0f, 1.0f};
    bool KeepAspect = false;

    void Draw(UIRenderer& ui) override {
        if (!SpriteTexture) return;
        glm::vec2 p = ResolvePosition((float)ui.ScreenWidth(), (float)ui.ScreenHeight());
        glm::vec2 s = ScaledSize();

        if (KeepAspect && SpriteTexture->Width() > 0 && SpriteTexture->Height() > 0 &&
            s.x > 0.0f && s.y > 0.0f) {
            float texAR = (float)SpriteTexture->Width() / (float)SpriteTexture->Height();
            float boxAR = s.x / s.y;
            glm::vec2 draw = s;
            if (texAR > boxAR) draw.y = s.x / texAR;   // текстура шире бокса — жмём по высоте
            else               draw.x = s.y * texAR;   // текстура уже бокса — жмём по ширине
            p += (s - draw) * 0.5f;                    // центрируем в отведённом Size
            s = draw;
        }
        ui.Sprite(p.x, p.y, s.x, s.y, *SpriteTexture, Tint, UV);
    }
};

// Кнопка: подсвечивается при наведении, темнеет при нажатии, зовёт OnClick
// при отпускании кнопки мыши внутри неё.
class UIButton : public UIElement {
public:
    std::string Label;
    glm::vec3 NormalColor{0.18f, 0.20f, 0.26f};
    glm::vec3 HoverColor{0.26f, 0.30f, 0.40f};
    glm::vec3 PressedColor{0.12f, 0.14f, 0.18f};
    float Alpha = 0.95f;
    glm::vec3 TextColor{1.0f};
    float TextScale = 1.6f;
    float OutlineThickness = 1.0f; // в пикселях до масштаба; 0 — без рамки
    glm::vec3 OutlineColor{0.8f, 0.75f, 0.6f};
    std::function<void()> OnClick;

    bool Hovered() const { return m_hovered; }

    void Update(const UIInputState& in, float w, float h) override {
        m_hovered = in.Mouse.x >= 0.0f && Contains(in.Mouse, w, h);
        if (in.MousePressed && m_hovered) m_pressedInside = true;
        if (in.MouseReleased) {
            if (m_pressedInside && m_hovered && OnClick) OnClick();
            m_pressedInside = false;
        }
        if (!in.MouseDown) m_pressedInside = false;
    }

    void Draw(UIRenderer& ui) override {
        glm::vec2 p = ResolvePosition((float)ui.ScreenWidth(), (float)ui.ScreenHeight());
        glm::vec2 s = ScaledSize();
        glm::vec3 c = m_pressedInside ? PressedColor : (m_hovered ? HoverColor : NormalColor);
        ui.Rect(p.x, p.y, s.x, s.y, c, Alpha);
        if (OutlineThickness > 0.0f) {
            ui.RectOutline(p.x, p.y, s.x, s.y, OutlineThickness * LayoutScale, OutlineColor, 0.9f);
        }
        if (!Label.empty()) {
            float ts = TextScale * LayoutScale;
            ui.TextCentered(p.x + s.x * 0.5f, p.y + (s.y - 8.0f * ts) * 0.5f, ts, TextColor, Label);
        }
    }

private:
    bool m_hovered = false;
    bool m_pressedInside = false;
};

// Переключатель on/off (тумблер). Size — размер дорожки; подпись рисуется
// справа от неё. Значение можно хранить внутри (Value) или тянуть/писать
// наружу через ValueSource/OnChanged (типовой паттерн привязки к настройке).
class UIToggle : public UIElement {
public:
    std::string Label;
    bool Value = false;
    glm::vec3 OnColor{0.28f, 0.62f, 0.34f};
    glm::vec3 OffColor{0.34f, 0.20f, 0.20f};
    glm::vec3 KnobColor{0.96f, 0.96f, 0.96f};
    glm::vec3 LabelColor{1.0f};
    float LabelScale = 1.4f;

    std::function<bool()> ValueSource;      // опционально: читать значение извне
    std::function<void(bool)> OnChanged;    // вызывается при переключении

    bool CurrentValue() const { return ValueSource ? ValueSource() : Value; }

    void Update(const UIInputState& in, float w, float h) override {
        bool hovered = in.Mouse.x >= 0.0f && Contains(in.Mouse, w, h);
        if (in.MousePressed && hovered) m_pressedInside = true;
        if (in.MouseReleased) {
            if (m_pressedInside && hovered) {
                Value = !CurrentValue();
                if (OnChanged) OnChanged(Value);
            }
            m_pressedInside = false;
        }
        if (!in.MouseDown) m_pressedInside = false;
    }

    void Draw(UIRenderer& ui) override {
        bool v = CurrentValue();
        glm::vec2 p = ResolvePosition((float)ui.ScreenWidth(), (float)ui.ScreenHeight());
        glm::vec2 s = ScaledSize();
        float ls = LayoutScale;

        // дорожка
        ui.Rect(p.x, p.y, s.x, s.y, v ? OnColor : OffColor, 0.95f);
        // ручка (квадрат чуть меньше высоты, слева при off / справа при on)
        float pad = 2.0f * ls;
        float knob = s.y - 2.0f * pad;
        float kx = v ? (p.x + s.x - knob - pad) : (p.x + pad);
        ui.Rect(kx, p.y + pad, knob, knob, KnobColor, 1.0f);
        // подпись справа от дорожки
        if (!Label.empty()) {
            float ts = LabelScale * ls;
            ui.Text(p.x + s.x + 8.0f * ls, p.y + (s.y - 8.0f * ts) * 0.5f, ts, LabelColor, Label);
        }
    }

private:
    bool m_pressedInside = false;
};
