#pragma once
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sage/ui/core/UIComponent.h"

// ---------------------------------------------------------------------------
// РЕЕСТРЫ (§96–98 ТЗ) — то, ради чего система расширяется без правки ядра.
//
// Ядро интерфейса не содержит НИ ОДНОГО списка «какие бывают компоненты»,
// «какие бывают эффекты», «какие бывают виджеты». Такие списки — это
// switch-case, разбросанный по сериализатору, инспектору и рисованию, и любое
// расширение начинается с их синхронного изменения.
//
// Здесь новый компонент/эффект/виджет добавляется ОДНОЙ регистрацией, в том
// числе из игры или плагина, и сразу получает всё: сохранение, чтение,
// инспектор, копирование, анимацию свойств.
//
// Повторная регистрация с тем же ключом ЗАМЕНЯЕТ прежнюю. Это не оплошность:
// так игра подменяет встроенный компонент своим, не трогая движок.
// ---------------------------------------------------------------------------
namespace sage::ui {

class UIComponentRegistry {
public:
    static UIComponentRegistry& Instance();

    // Хранится УКАЗАТЕЛЬ на канонический тип (статический объект внутри
    // StaticType() компонента), а не копия: сравнение «этот ли это тип»
    // должно быть сравнением адресов, иначе копия в реестре и оригинал у
    // компонента оказываются разными типами с одинаковым именем.
    void Register(const UIComponentType& type);
    const UIComponentType* Find(std::string_view id) const;
    // Все типы, отсортированные по Order. Первое обращение регистрирует
    // встроенные компоненты движка.
    const std::vector<const UIComponentType*>& All() const;

    std::unique_ptr<UIComponent> Create(std::string_view id) const;

private:
    UIComponentRegistry() = default;
    void EnsureBuiltins() const;

    mutable std::vector<const UIComponentType*> m_types;
    mutable std::vector<const UIComponentType*> m_sorted;
    mutable bool m_builtinsDone = false;
};

// Регистрация встроенных компонентов движка. Открыта наружу, потому что её
// зовёт и реестр (лениво), и тесты (детерминированно, до первого обращения).
void RegisterBuiltinUIComponents();

// --- Реестр виджетов (§60, §100) -------------------------------------------
//
// Виджет — это НЕ компонент, а сборка узлов: кнопка это подложка + надпись +
// взаимодействие. Поэтому и реестр отдельный: он хранит не тип данных, а
// функцию «собери мне такое поддерево».
class UIDocument;
using UINodeId = uint32_t;

struct UIWidgetType {
    std::string Id;     // "button", "slider", "scrollview"
    std::string Title;  // название для человека
    const char* Icon = nullptr;
    std::string Hint;
    std::string Category; // раздел меню создания: "Basic", "Input", "Containers"

    // Собирает виджет внутри parent и возвращает его корневой узел.
    std::function<UINodeId(UIDocument&, UINodeId parent)> Build;
};

class UIWidgetRegistry {
public:
    static UIWidgetRegistry& Instance();

    void Register(UIWidgetType type);
    const UIWidgetType* Find(std::string_view id) const;
    const std::vector<UIWidgetType>& All() const;

    // Создать виджет по имени. kUIInvalidNode — имени нет: молча отдать пустой
    // узел значило бы «кнопка создалась и не работает».
    UINodeId Build(std::string_view id, UIDocument& doc, UINodeId parent) const;

private:
    UIWidgetRegistry() = default;
    void EnsureBuiltins() const;

    mutable std::vector<UIWidgetType> m_types;
    mutable bool m_builtinsDone = false;
};

void RegisterBuiltinUIWidgets();

} // namespace sage::ui
