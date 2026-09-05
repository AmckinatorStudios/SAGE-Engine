#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sage/ui/core/UIComponent.h"
#include "sage/ui/core/UITypes.h"

// ---------------------------------------------------------------------------
// УЗЕЛ ИНТЕРФЕЙСА (§6 ТЗ) — фундаментальный объект новой системы.
//
// Узел не «кнопка», не «картинка» и не «текст». Узел — это МЕСТО В ДЕРЕВЕ со
// своими собственными свойствами (виден ли, включён ли, насколько прозрачен, в
// каком слое, в каком порядке среди соседей) плюс набор компонентов, которые и
// определяют, чем он на самом деле является.
//
// Почему собственные свойства узла — именно эти и только эти: каждое из них
// НАСЛЕДУЕТСЯ поддеревом или управляет им (§8). Прозрачность умножается,
// видимость и включённость передаются вниз, слой и порядок задают, что поверх
// чего. Всё остальное — свойство компонента, а не узла: цвет принадлежит
// заливке, шрифт — тексту, радиус — форме.
// ---------------------------------------------------------------------------
namespace sage::ui {

using UINodeId = uint32_t;
constexpr UINodeId kUIInvalidNode = 0;

class UIDocument;

class UINode {
public:
    // --- Личность (§92) -----------------------------------------------------
    //
    // Два разных идентификатора, и оба нужны. Id — номер внутри документа,
    // быстрый и удобный для рантайма; он не переживает пересборку документа.
    // Guid — строка, которая переживает сохранение, копирование и слияние: по
    // ней ссылаются префабы, анимации и внешние привязки.
    UINodeId Id = kUIInvalidNode;
    std::string Guid;
    std::string Name;

    // --- Наследуемое состояние ----------------------------------------------
    bool Enabled = true;  // выключенный не участвует ни в чём, вместе с детьми
    bool Visible = true;  // невидимый не рисуется (но может ловить мышь, если
                          // это явно включено в UIInteraction)
    float Opacity = 1.0f; // умножается на прозрачность предков (§28)
    UIBlendMode Blend = UIBlendMode::Normal;

    // --- Порядок (§25, §26) -------------------------------------------------
    //
    // Слой — грубая ступень (фон, HUD, меню, подсказки), порядок — точная
    // расстановка внутри слоя, а при равенстве обоих решает порядок в дереве.
    // Три ступени вместо одного «z» ровно потому, что иначе всплывающая
    // подсказка и элемент HUD конкурируют одним числом, и любое добавление
    // элемента ломает чужую расстановку.
    int Layer = 0;
    int Order = 0;

    // --- Редакторские пометки -----------------------------------------------
    //
    // Живут в документе, а не в редакторе: замок и «спрятать» — свойства
    // содержимого, и человек ждёт их на месте после перезапуска.
    bool Locked = false;
    bool EditorHidden = false;

    // --- Связи --------------------------------------------------------------
    UINodeId Parent = kUIInvalidNode;
    std::vector<UINodeId> Children;

    // Из какого префаба пришёл узел (§61–63). Пусто — обычный узел.
    std::string PrefabSource;
    // Корень вставленного экземпляра префаба: его дети «принадлежат» источнику.
    bool PrefabRoot = false;

    // --- Компоненты ---------------------------------------------------------
    std::vector<std::unique_ptr<UIComponent>> Components;

    template <class T> T* Get() {
        for (auto& c : Components)
            if (&c->Type() == &T::StaticType()) return static_cast<T*>(c.get());
        return nullptr;
    }
    template <class T> const T* Get() const {
        return const_cast<UINode*>(this)->Get<T>();
    }
    template <class T> bool Has() const { return Get<T>() != nullptr; }

    // Добавить компонент, если его ещё нет, и вернуть ссылку. Основной способ
    // собирать интерфейс кодом: node.Ensure<UIFill>().Color = ...
    template <class T> T& Ensure() {
        if (T* existing = Get<T>()) return *existing;
        Components.push_back(std::make_unique<T>());
        return *static_cast<T*>(Components.back().get());
    }
    template <class T> void Remove() {
        for (size_t i = 0; i < Components.size(); ++i) {
            if (&Components[i]->Type() == &T::StaticType()) {
                if (Components[i]->Type().Essential) return; // §135: не оставлять узел без прямоугольника
                Components.erase(Components.begin() + (long)i);
                return;
            }
        }
    }

    UIComponent* Find(std::string_view id);
    const UIComponent* Find(std::string_view id) const;
    UIComponent* Add(std::string_view id);
    bool RemoveById(std::string_view id);

    // Компоненты в порядке рисования (по UIComponentType::Order).
    std::vector<UIComponent*> DrawOrder() const;
};

} // namespace sage::ui
