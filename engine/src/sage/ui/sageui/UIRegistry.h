#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sage/ui/sageui/UIElement.h"

// ---------------------------------------------------------------------------
// РЕЕСТР ТИПОВ ЭЛЕМЕНТОВ (§16, §40 ТЗ).
//
// ЗАЧЕМ. Новый элемент — кнопка-график, редактор кривой, дерево сцены — должен
// появляться в системе БЕЗ единой правки ядра. Иначе ядро обрастает списком
// «а ещё бывает такой», и каждый плагин требует своей строчки в чужом файле.
//
// Регистрация одна и даёт всё сразу: создание по имени (данные, .uidoc,
// плагин), название и раздел для палитры редактора, значок. Ровно тот же приём,
// что у компонентов узла (UIComponentRegistry) — и намеренно тот же: две разные
// схемы расширения в одной системе означают, что одну из них однажды забудут
// поддержать.
//
// ПОВТОРНАЯ РЕГИСТРАЦИЯ ЗАМЕНЯЕТ. Плагин перезагрузили — тип обновился, а не
// удвоился.
// ---------------------------------------------------------------------------
namespace sage::ui::sui {

struct UIElementType {
    std::string Name;                 // "Button" — по нему создают
    std::string Title;                // как показать человеку
    std::string Category = "General"; // раздел палитры
    std::string Icon;                 // значок движка, необязателен
    std::function<std::unique_ptr<UIElement>()> Make;
};

class UIElementRegistry {
public:
    static UIElementRegistry& Instance();

    void Register(UIElementType type);
    template <class T>
    void Register(const std::string& name, const std::string& title,
                  const std::string& category = "General", const std::string& icon = {}) {
        Register(UIElementType{name, title, category, icon,
                               [] { return std::unique_ptr<UIElement>(new T()); }});
    }

    const UIElementType* Find(const std::string& name) const;
    std::unique_ptr<UIElement> Make(const std::string& name) const;
    const std::vector<UIElementType>& All() const { return m_types; }

private:
    UIElementRegistry() = default;
    std::vector<UIElementType> m_types;
};

// Зарегистрировать встроенные типы. Идемпотентно — зовётся из UIContext.
void RegisterBuiltinUIElements();

} // namespace sage::ui::sui
