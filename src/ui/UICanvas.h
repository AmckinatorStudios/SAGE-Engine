#pragma once
#include "UIElement.h"
#include <memory>
#include <vector>
#include <utility>
#include <algorithm>

// ---------------------------------------------------------------------
// UICanvas — контейнер виджетов, из которых собирается худ (или меню).
// Элементы рисуются в порядке добавления (последний — поверх).
//
// Использование:
//
//   UICanvas hud;
//   auto* hp = hud.Add<UIProgressBar>();
//   hp->Anchor = UIAnchor::TopLeft;
//   hp->Offset = {16, 16};
//   hp->Size = {180, 16};
//   hp->Label = "HP";
//   hp->FillColor = {0.85f, 0.25f, 0.25f};
//   hp->ValueSource = [&]{ return stats.Health / 100.0f; };
//   ...
//   // в игровом цикле:
//   ui.Begin(w, h);
//   hud.Draw(ui);          // весь худ одним вызовом
//   ...прочий immediate-mode UI при желании...
//   ui.End();
//
// Канвас можно целиком прятать/показывать (Visible) — удобно для
// переключаемых экранов: худ, меню крафта, экран паузы и т.д.
// ---------------------------------------------------------------------
class UICanvas {
public:
    bool Visible = true;

    // Референсное (проектное) разрешение. Если задано (не ноль), интерфейс
    // масштабируется под текущее окно с сохранением пропорций — чтобы при
    // любом соотношении сторон элементы были одного визуального размера
    // относительно экрана, а не "мельчали" на 4K и не "распухали" на 720p.
    // Ноль (по умолчанию) — попиксельная вёрстка без масштабирования.
    glm::vec2 ReferenceSize{0.0f};

    // Создаёт виджет типа T, добавляет в канвас и возвращает указатель
    // для настройки. Канвас владеет виджетом (unique_ptr).
    template <typename T, typename... Args>
    T* Add(Args&&... args) {
        auto widget = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = widget.get();
        m_elements.push_back(std::move(widget));
        return raw;
    }

    // Прогоняет ввод по интерактивным виджетам (кнопки/переключатели). Вызывать
    // ПЕРЕД Draw, раз в кадр, когда канвас активен и принимает клики.
    void Update(const UIInputState& input, float screenW, float screenH) {
        if (!Visible) return;
        float scale = ComputeScale(screenW, screenH);
        for (auto& element : m_elements) {
            element->LayoutScale = scale;
            if (element->Visible) element->Update(input, screenW, screenH);
        }
    }

    void Draw(UIRenderer& ui) {
        if (!Visible) return;
        float scale = ComputeScale((float)ui.ScreenWidth(), (float)ui.ScreenHeight());
        for (auto& element : m_elements) {
            element->LayoutScale = scale;
            if (element->Visible) element->Draw(ui);
        }
    }

    void Clear() { m_elements.clear(); }
    size_t Count() const { return m_elements.size(); }

private:
    // Коэффициент масштаба вёрстки: вписываем референс в экран с сохранением
    // пропорций (min по осям — "letterbox", ничего не растягивается).
    float ComputeScale(float w, float h) const {
        if (ReferenceSize.x <= 0.0f || ReferenceSize.y <= 0.0f) return 1.0f;
        return std::min(w / ReferenceSize.x, h / ReferenceSize.y);
    }

    std::vector<std::unique_ptr<UIElement>> m_elements;
};
