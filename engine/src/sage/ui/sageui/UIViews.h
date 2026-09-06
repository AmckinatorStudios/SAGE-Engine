#pragma once
#include <functional>
#include <string>
#include <vector>

#include "sage/ui/sageui/UIControls.h"

// ---------------------------------------------------------------------------
// СПИСКИ И ТАБЛИЦЫ СВОЙСТВ (§7, §11, §12 ТЗ редактора).
//
// Tree — иерархия сцены и дерево каталогов. PropertyGrid — инспектор.
// AssetView — карточки ассетов.
//
// ОБЩЕЕ У ВСЕХ ТРЁХ И ГЛАВНОЕ В НИХ: содержимое задаётся ДАННЫМИ, а строки
// переиспользуются. Дерево сцены перестраивается на каждое изменение выделения,
// а изменений выделения — десятки в минуту; пересоздавать полторы сотни узлов
// на каждое значило бы терять прокрутку, раскрытые ветки и подсветку под
// курсором. Поэтому Tree::SetItems принимает ПЛОСКИЙ список с глубиной, а
// элементы под него подгоняются.
//
// Плоский список, а не дерево объектов, намеренно: раскрытие и свёртка — это
// решение того, кто список отдаёт (он знает про свои данные), а не виджета.
// Виджет, хранящий своё представление дерева, обязан его синхронизировать — и
// однажды разойдётся с настоящим.
// ---------------------------------------------------------------------------
namespace sage::ui::sui {

// --- Дерево ------------------------------------------------------------------

struct TreeItem {
    std::string Id;        // устойчивый идентификатор строки
    std::string Text;
    std::string Icon;      // значок типа объекта; пусто — без значка
    int Depth = 0;         // уровень вложенности
    bool HasChildren = false;
    bool Expanded = false;
    bool Selected = false;
    bool Visible = true;   // «глазок» справа; сам виджет его только показывает
    UIColor Tint{1.0f, 1.0f, 1.0f, 1.0f};
};

class Tree : public UIElement {
public:
    const char* TypeName() const override { return "Tree"; }
    void OnAttach() override;

    // Отдать содержимое. Зовётся столько раз, сколько надо: одинаковый список
    // не пересобирает ни одного узла.
    void SetItems(std::vector<TreeItem> items);
    const std::vector<TreeItem>& Items() const { return m_items; }

    // Щелчок по строке. additive — с Ctrl/Shift: множественное выделение
    // решает вызывающий, потому что только он знает, что такое «диапазон».
    Tree* OnSelect(std::function<void(const std::string&, bool additive)> fn);
    Tree* OnToggle(std::function<void(const std::string&)> fn);      // раскрыть/свернуть
    Tree* OnVisibility(std::function<void(const std::string&)> fn);  // «глазок»
    Tree* OnActivate(std::function<void(const std::string&)> fn);    // двойной щелчок

    UIElement* Content() const;

private:
    struct RowUI {
        UIElement* Box = nullptr;
        UIElement* Arrow = nullptr;
        UIElement* Icon = nullptr;
        Label* Text = nullptr;
        IconButton* Eye = nullptr;
        std::string Id;
    };

    RowUI& EnsureRow(size_t index);
    void Apply(RowUI& row, const TreeItem& item);

    ScrollView* m_scroll = nullptr;
    std::vector<RowUI> m_rows;
    std::vector<TreeItem> m_items;
    std::function<void(const std::string&, bool)> m_onSelect;
    std::function<void(const std::string&)> m_onToggle, m_onVisibility, m_onActivate;
};

// --- Таблица свойств ---------------------------------------------------------
//
// Секция с заголовком, который сворачивается, и строки «подпись — редактор».
// Ширина колонки подписей одна на всю таблицу: пока каждая строка выбирала её
// сама, поля стояли лесенкой.
class PropertyGrid : public UIElement {
public:
    const char* TypeName() const override { return "PropertyGrid"; }
    void OnAttach() override;

    // Секция. Возвращает контейнер, в который кладут строки.
    UIElement* AddSection(const std::string& title, bool expanded = true);
    // Строка: подпись слева, редактор справа. Редактор создаёт вызывающий —
    // таблица не знает и не должна знать, чем правят значение.
    UIElement* AddRow(UIElement* section, const std::string& label);
    // Строка на всю ширину, без подписи: кнопка, разделитель, предупреждение.
    UIElement* AddWide(UIElement* section);

    PropertyGrid* SetLabelWidth(float width);
    float LabelWidth() const { return m_labelWidth; }

    UIElement* Content() const;

private:
    ScrollView* m_scroll = nullptr;
    float m_labelWidth = 88.0f;
};

// --- Карточки ассетов --------------------------------------------------------

struct AssetItem {
    std::string Id;
    std::string Name;
    std::string Kind;      // подпись под именем: «Static Mesh», «Material»
    std::string Icon;      // значок-заглушка, пока нет обложки
    bool Selected = false;
};

class AssetView : public UIElement {
public:
    const char* TypeName() const override { return "AssetView"; }
    void OnAttach() override;

    void SetItems(std::vector<AssetItem> items);
    AssetView* SetCardSize(glm::vec2 size);
    AssetView* OnSelect(std::function<void(const std::string&)> fn);
    AssetView* OnActivate(std::function<void(const std::string&)> fn);

    UIElement* Content() const;

private:
    struct CardUI {
        UIElement* Box = nullptr;
        UIElement* Cover = nullptr;
        UIElement* Icon = nullptr;
        Label* Name = nullptr;
        Label* Kind = nullptr;
        std::string Id;
    };

    CardUI& EnsureCard(size_t index);

    ScrollView* m_scroll = nullptr;
    std::vector<CardUI> m_cards;
    std::vector<AssetItem> m_items;
    // Высота считается от ширины: обложка квадратная, под ней две строки.
    // Задавать её отдельным числом значило бы, что при смене ширины карточки
    // подписи вылезают наружу — ровно это и происходило.
    glm::vec2 m_cardSize{88.0f, 122.0f};
    std::function<void(const std::string&)> m_onSelect, m_onActivate;
};

} // namespace sage::ui::sui
