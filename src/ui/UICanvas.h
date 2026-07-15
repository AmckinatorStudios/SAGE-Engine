#pragma once
#include "UIElement.h"
#include <memory>
#include <vector>
#include <utility>

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

    // Создаёт виджет типа T, добавляет в канвас и возвращает указатель
    // для настройки. Канвас владеет виджетом (unique_ptr).
    template <typename T, typename... Args>
    T* Add(Args&&... args) {
        auto widget = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = widget.get();
        m_elements.push_back(std::move(widget));
        return raw;
    }

    void Draw(UIRenderer& ui) {
        if (!Visible) return;
        for (auto& element : m_elements) {
            if (element->Visible) element->Draw(ui);
        }
    }

    void Clear() { m_elements.clear(); }
    size_t Count() const { return m_elements.size(); }

private:
    std::vector<std::unique_ptr<UIElement>> m_elements;
};
