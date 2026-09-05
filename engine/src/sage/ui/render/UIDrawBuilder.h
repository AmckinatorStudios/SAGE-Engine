#pragma once
#include "sage/ui/core/UIContext.h"
#include "sage/ui/core/UIDocument.h"
#include "sage/ui/layout/UILayoutSolver.h"
#include "sage/ui/render/UIRenderList.h"

// ---------------------------------------------------------------------------
// ПОДГОТОВКА КАДРА (§82) — превращение посчитанной раскладки в список команд.
//
// Здесь и только здесь компоненты узнают, как они выглядят. Ни узел, ни
// компонент не рисуют себя через GPU (§4): компонент описывает СВОЙСТВА, а этот
// проход превращает их в фигуры.
//
// ПОЧЕМУ НЕ «КАЖДЫЙ КОМПОНЕНТ РИСУЕТ СЕБЯ САМ». Потому что порядок,
// объединение в батчи, маски, эффекты и промежуточные цели — свойства КАДРА, а
// не отдельного компонента. Компонент, рисующий себя напрямую, обязан знать всё
// это, и в итоге каждый новый компонент повторяет чужую логику эффектов (§145).
//
// Расширение: свой компонент добавляет свою эмиссию через UIDrawRegistry —
// таблицу «тип компонента → как из него получаются команды». Ядро при этом не
// меняется вовсе.
// ---------------------------------------------------------------------------
namespace sage::ui {

class UIComponent;
class UINode;

// Что эмиттер получает.
struct UIDrawContext {
    const UIContext* Ctx = nullptr;
    const UINode* Node = nullptr;
    const UIResolvedNode* Resolved = nullptr;
    UIRenderList* List = nullptr;
    UIRect Rect{};        // прямоугольник узла в экранных пикселях
    float Scale = 1.0f;   // масштаб холста
    float Opacity = 1.0f; // итоговая прозрачность
    UIClipState Clip;
    UIBlendMode Blend = UIBlendMode::Normal;
    uint64_t SortKey = 0;
    // Множитель цвета от эффектов уровня Modulate — уже перемноженный.
    UIColor Modulate{1.0f, 1.0f, 1.0f, 1.0f};

    // Готовая команда с уже заполненным общим состоянием: пусть эмиттер
    // заполняет только своё. Иначе каждый повторяет восемь присваиваний и
    // однажды забывает маску.
    UIRenderCommand& Begin(UIPrimitive kind) const;
};

using UIDrawEmitter = void (*)(const UIDrawContext&, const UIComponent&);

// Таблица «тип компонента → эмиттер». Отдельно от UIComponentType намеренно:
// компонент может существовать в сборке без рисования вовсе (инструмент,
// тесты, серверная проверка вёрстки), и тянуть туда рисование незачем.
class UIDrawRegistry {
public:
    static UIDrawRegistry& Instance();
    void Register(std::string_view componentId, UIDrawEmitter emitter);
    UIDrawEmitter Find(std::string_view componentId) const;

private:
    UIDrawRegistry() = default;
    void EnsureBuiltins() const;
    struct Entry { std::string Id; UIDrawEmitter Fn; };
    mutable std::vector<Entry> m_entries;
    mutable bool m_builtinsDone = false;
};

void RegisterBuiltinUIEmitters();

// Собрать команды всего документа.
void UIBuildDrawList(UIDocument& doc, const UILayoutSolver& layout, const UIContext& ctx,
                     UIRenderList& out);

} // namespace sage::ui
