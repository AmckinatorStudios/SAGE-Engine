#include "sage/ui/sageui/UIRegistry.h"

namespace sage::ui::sui {

UIElementRegistry& UIElementRegistry::Instance() {
    static UIElementRegistry instance;
    return instance;
}

void UIElementRegistry::Register(UIElementType type) {
    for (UIElementType& t : m_types) {
        if (t.Name == type.Name) {
            t = std::move(type);
            return;
        }
    }
    m_types.push_back(std::move(type));
}

const UIElementType* UIElementRegistry::Find(const std::string& name) const {
    for (const UIElementType& t : m_types)
        if (t.Name == name) return &t;
    return nullptr;
}

std::unique_ptr<UIElement> UIElementRegistry::Make(const std::string& name) const {
    const UIElementType* t = Find(name);
    return t && t->Make ? t->Make() : nullptr;
}

} // namespace sage::ui::sui
