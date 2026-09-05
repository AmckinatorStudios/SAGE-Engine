#include "sage/ui/core/UIRegistry.h"

#include <algorithm>

#include "sage/ui/core/UIDocument.h"
#include "sage/ui/effects/UIEffect.h"
#include "sage/ui/input/UIInteraction.h"
#include "sage/ui/layout/UILayout.h"
#include "sage/ui/layout/UITransform.h"
#include "sage/ui/mask/UIMask.h"
#include "sage/ui/serialization/UIPrefab.h"
#include "sage/ui/serialization/UISerializer.h"
#include "sage/ui/style/UIStyle.h"
#include "sage/ui/visual/UIBorder.h"
#include "sage/ui/visual/UIFill.h"
#include "sage/ui/visual/UIIcon.h"
#include "sage/ui/visual/UIImage.h"
#include "sage/ui/visual/UIShape.h"
#include "sage/ui/visual/UIText.h"
#include "sage/ui/widgets/UIWidgets.h"

namespace sage::ui {

const char* const* UIComponentCategoryNames() {
    static const char* names[] = {"Transform", "Layout",  "Appearance", "Text",    "Image",
                                  "Mask",      "Effects", "Interaction", "Advanced"};
    return names;
}
int UIComponentCategoryCount() { return 9; }

UIComponentRegistry& UIComponentRegistry::Instance() {
    static UIComponentRegistry r;
    return r;
}

void UIComponentRegistry::Register(const UIComponentType& type) {
    for (auto& t : m_types) {
        if (t->Id == type.Id) {
            // Замена, а не отказ: так игра подменяет встроенный компонент своим,
            // не трогая движок.
            t = &type;
            m_sorted.clear();
            return;
        }
    }
    m_types.push_back(&type);
    m_sorted.clear();
}

const UIComponentType* UIComponentRegistry::Find(std::string_view id) const {
    EnsureBuiltins();
    for (const UIComponentType* t : m_types)
        if (t->Id == id) return t;
    return nullptr;
}

const std::vector<const UIComponentType*>& UIComponentRegistry::All() const {
    EnsureBuiltins();
    if (m_sorted.empty() && !m_types.empty()) {
        m_sorted = m_types;
        std::stable_sort(m_sorted.begin(), m_sorted.end(),
                         [](const UIComponentType* a, const UIComponentType* b) {
                             return a->Order < b->Order;
                         });
    }
    return m_sorted;
}

std::unique_ptr<UIComponent> UIComponentRegistry::Create(std::string_view id) const {
    const UIComponentType* t = Find(id);
    if (!t || !t->Create) return nullptr;
    return t->Create();
}

void UIComponentRegistry::EnsureBuiltins() const {
    if (m_builtinsDone) return;
    m_builtinsDone = true;
    RegisterBuiltinUIComponents();
}

void RegisterBuiltinUIComponents() {
    static bool done = false;
    if (done) return;
    done = true;
    UIComponentRegistry& r = UIComponentRegistry::Instance();
    // ЯДРО. Ни одного виджета: ядро о них не знает и не должно (§141).
    r.Register(UITransform::StaticType());
    r.Register(UILayout::StaticType());
    r.Register(UIFill::StaticType());
    r.Register(UIBorder::StaticType());
    r.Register(UIShape::StaticType());
    r.Register(UIImage::StaticType());
    r.Register(UIIcon::StaticType());
    r.Register(UIText::StaticType());
    r.Register(UIMask::StaticType());
    r.Register(UIEffects::StaticType());
    r.Register(UIInteraction::StaticType());
    r.Register(UIStyled::StaticType());
    r.Register(UIPrefabInstance::StaticType());
    r.Register(UIUnknownComponent::StaticType());
    // Компоненты поведения виджетов — надстройка, но зарегистрировать их надо
    // здесь же: без этого документ с ползунком не прочитается в сборке, которая
    // виджеты не создаёт. Ядро от них не ЗАВИСИТ — оно про них ничего не знает,
    // кроме одной строки регистрации.
    RegisterBuiltinUIWidgetComponents();
}

// --- Виджеты ----------------------------------------------------------------

UIWidgetRegistry& UIWidgetRegistry::Instance() {
    static UIWidgetRegistry r;
    return r;
}

void UIWidgetRegistry::Register(UIWidgetType type) {
    for (auto& t : m_types) {
        if (t.Id == type.Id) { t = std::move(type); return; }
    }
    m_types.push_back(std::move(type));
}

const UIWidgetType* UIWidgetRegistry::Find(std::string_view id) const {
    EnsureBuiltins();
    for (const auto& t : m_types)
        if (t.Id == id) return &t;
    return nullptr;
}

const std::vector<UIWidgetType>& UIWidgetRegistry::All() const {
    EnsureBuiltins();
    return m_types;
}

UINodeId UIWidgetRegistry::Build(std::string_view id, UIDocument& doc, UINodeId parent) const {
    const UIWidgetType* t = Find(id);
    // Неизвестное имя — пустой ответ, а не пустой узел: «кнопка создалась и не
    // работает» отлаживается втрое дольше, чем «кнопка не создалась».
    if (!t || !t->Build) return kUIInvalidNode;
    return t->Build(doc, parent);
}

void UIWidgetRegistry::EnsureBuiltins() const {
    if (m_builtinsDone) return;
    m_builtinsDone = true;
    RegisterBuiltinUIWidgets();
}

} // namespace sage::ui
