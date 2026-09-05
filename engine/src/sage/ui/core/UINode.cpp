#include "sage/ui/core/UINode.h"

#include <algorithm>

#include "sage/ui/core/UIRegistry.h"

namespace sage::ui {

UIComponent* UINode::Find(std::string_view id) {
    for (auto& c : Components)
        if (c->Type().Id == id) return c.get();
    return nullptr;
}

const UIComponent* UINode::Find(std::string_view id) const {
    return const_cast<UINode*>(this)->Find(id);
}

UIComponent* UINode::Add(std::string_view id) {
    const UIComponentType* type = UIComponentRegistry::Instance().Find(id);
    if (!type) return nullptr;
    // Уникальный компонент уже есть — отдаём его, а не заводим второй: две
    // раскладки на одном узле не значат ничего, кроме «одна из них не работает».
    if (type->Unique) {
        if (UIComponent* existing = Find(id)) return existing;
    }
    std::unique_ptr<UIComponent> made = UIComponentRegistry::Instance().Create(id);
    if (!made) return nullptr;
    Components.push_back(std::move(made));
    return Components.back().get();
}

bool UINode::RemoveById(std::string_view id) {
    for (size_t i = 0; i < Components.size(); ++i) {
        if (Components[i]->Type().Id == id) {
            if (Components[i]->Type().Essential) return false;
            Components.erase(Components.begin() + (long)i);
            return true;
        }
    }
    return false;
}

std::vector<UIComponent*> UINode::DrawOrder() const {
    std::vector<UIComponent*> out;
    out.reserve(Components.size());
    for (const auto& c : Components) out.push_back(c.get());
    // Стабильная сортировка: компоненты с одинаковым Order рисуются в том
    // порядке, в каком их добавили, — иначе перестановка была бы «случайной».
    std::stable_sort(out.begin(), out.end(), [](const UIComponent* a, const UIComponent* b) {
        return a->Type().Order < b->Type().Order;
    });
    return out;
}

} // namespace sage::ui
