#pragma once
#include <functional>
#include <string>
#include <vector>

#include "sage/ui/sageui/UIElement.h"
#include "sage/ui/visual/UIText.h"
#include "sage/ui/widgets/UIWidgets.h"

// ---------------------------------------------------------------------------
// БАЗОВЫЕ ВИДЖЕТЫ ОБЪЕКТНОГО СЛОЯ (§14 ТЗ).
//
// Каждый из них — ТОНКИЙ класс поверх узла: он не рисует сам и не считает
// раскладку, он собирает узел из компонентов ядра и даёт этому имя, понятное
// человеку. Кнопка — это подложка + надпись + область попадания + команда; всё
// перечисленное уже умеет ядро, и виджет только договаривается, как именно.
//
// ПОЧЕМУ ЭТО ВАЖНО ИМЕННО ТАК. Виджет, который рисует себя сам, — это тупик, из
// которого выросла immediate-mode система: у него своя отрисовка, свои
// состояния, своё попадание мышью, и ничего из этого не переиспользуется. Здесь
// же новый виджет получает даром маски, эффекты, батчинг, фокус, навигацию,
// темы и сохранение в файл — потому что он их не изобретает.
// ---------------------------------------------------------------------------
namespace sage::ui::sui {

// --- Контейнер --------------------------------------------------------------
//
// Панель с подложкой. Самый частый строительный блок и, что важнее, ЕДИНСТВЕННЫЙ
// способ сгруппировать: группа без своего узла не может ни спрятаться целиком,
// ни получить прозрачность, ни обрезать содержимое.
class Panel : public UIElement {
public:
    const char* TypeName() const override { return "Panel"; }
    void OnAttach() override;

    Panel* SetColor(const UIColor& color);
    Panel* SetRadius(float radius);
    Panel* SetBorder(float width, const UIColor& color);
};

// Прозрачный контейнер: только раскладка, ничего не рисует. Нужен ровно затем,
// чтобы не платить подложкой за группировку (§130 «неиспользуемое не стоит»).
class Group : public UIElement {
public:
    const char* TypeName() const override { return "Group"; }
};

// --- Текст ------------------------------------------------------------------
class Label : public UIElement {
public:
    explicit Label(std::string text = {});
    const char* TypeName() const override { return "Label"; }
    void OnAttach() override;

    Label* SetText(const std::string& text);
    const std::string& Text() const;
    Label* SetFontSize(float size);
    Label* SetColor(const UIColor& color);
    Label* SetAlign(UITextAlign align, UITextVAlign vertical = UITextVAlign::Top);
    Label* SetWrap(bool wrap);
    // Многоточие вместо обрезки на полуслове: в списках и путях это разница
    // между «видно, что не влезло» и «непонятно, что написано».
    Label* SetEllipsis(bool on);

private:
    std::string m_initial;
};

// --- Кнопка -----------------------------------------------------------------
class Button : public UIElement {
public:
    explicit Button(std::string text = {}, std::string command = {});
    const char* TypeName() const override { return "Button"; }
    void OnAttach() override;

    Button* SetText(const std::string& text);
    // Команда (§102): строка наружу. Кнопка не знает, что она значит, и не
    // должна знать — иначе игровая логика просачивается в интерфейс.
    Button* SetCommand(const std::string& command);
    Label* Caption() const { return m_caption; }

private:
    std::string m_text, m_command;
    Label* m_caption = nullptr;
};

// --- Галка ------------------------------------------------------------------
class Checkbox : public UIElement {
public:
    explicit Checkbox(std::string text = {}, bool checked = false);
    const char* TypeName() const override { return "Checkbox"; }
    void OnAttach() override;

    bool Checked() const;
    Checkbox* SetChecked(bool on);
    Checkbox* OnToggle(std::function<void(bool)> fn);

private:
    std::string m_text;
    bool m_checked = false;
};

// --- Ползунок ---------------------------------------------------------------
class Slider : public UIElement {
public:
    Slider(float value = 0.5f, float min = 0.0f, float max = 1.0f);
    const char* TypeName() const override { return "Slider"; }
    void OnAttach() override;

    float Value() const;
    Slider* SetValue(float value);
    Slider* SetRange(float min, float max);
    Slider* SetStep(float step);

private:
    float m_value = 0.5f, m_min = 0.0f, m_max = 1.0f;
};

// --- Полоса выполнения ------------------------------------------------------
class ProgressBar : public UIElement {
public:
    explicit ProgressBar(float value = 0.0f);
    const char* TypeName() const override { return "ProgressBar"; }
    void OnAttach() override;

    ProgressBar* SetValue(float value);
    float Value() const;

private:
    float m_value = 0.0f;
};

// --- Поле ввода -------------------------------------------------------------
class TextInput : public UIElement {
public:
    explicit TextInput(std::string value = {}, std::string placeholder = {});
    const char* TypeName() const override { return "TextInput"; }
    void OnAttach() override;

    const std::string& Value() const;
    TextInput* SetValue(const std::string& value);
    TextInput* OnSubmit(std::function<void(const std::string&)> fn);
    TextInput* OnChanged(std::function<void(const std::string&)> fn);

private:
    std::string m_value, m_placeholder;
};

// --- Картинка ---------------------------------------------------------------
class Image : public UIElement {
public:
    explicit Image(std::string path = {});
    const char* TypeName() const override { return "Image"; }
    void OnAttach() override;

    Image* SetPath(const std::string& path);
    Image* SetTint(const UIColor& tint);

private:
    std::string m_path;
};

// --- Прокрутка --------------------------------------------------------------
//
// Содержимое кладётся в Content(), а не в сам ScrollView: снаружи — окно с
// маской, внутри — лента произвольной высоты. Смешать их в одном узле нельзя:
// маска обрезала бы саму ленту вместе с содержимым.
class ScrollView : public UIElement {
public:
    const char* TypeName() const override { return "ScrollView"; }
    void OnAttach() override;

    UIElement* Content() const { return m_content; }
    ScrollView* SetHorizontal(bool on);
    ScrollView* SetVertical(bool on);

private:
    UIElement* m_content = nullptr;
};

} // namespace sage::ui::sui
