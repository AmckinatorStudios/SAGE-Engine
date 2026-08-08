#include "sage/ui/UIPresets.h"

#include "sage/ui/UIComponents.h"

namespace sage::ui {

namespace {

using Kind = UIElementComponent::Kind;

// Таблица заготовок. Одна строка — один узнаваемый элемент интерфейса.
//
// Значения подобраны так, чтобы созданный элемент БЫЛО ВИДНО и он делал то, что
// обещает имя: у кнопки включено Interactive (без него это просто
// прямоугольник), у полосы Value = 0.6 (пустая полоса неотличима от панели), у
// поля ввода текст прижат влево (по центру набирать неудобно и непривычно).
struct Preset {
    const char* Name;
    void (*Apply)(UIElementComponent&);
};

const Preset kPresets[] = {
    {"Panel", [](UIElementComponent& u) {
         u.Type = Kind::Panel;
         u.Size = {260.0f, 140.0f};
         u.Text.clear();
     }},
    {"Button", [](UIElementComponent& u) {
         u.Type = Kind::Panel;
         u.Size = {200.0f, 52.0f};
         u.Text = "Button";
         u.Interactive = true; // без этого «кнопка» — просто прямоугольник
         u.Color = {0.16f, 0.22f, 0.34f, 0.95f};
         u.BorderThickness = 1.0f;
     }},
    {"Label", [](UIElementComponent& u) {
         u.Type = Kind::Label;
         u.Size = {220.0f, 40.0f};
         u.Text = "Text";
         u.AutoWidth = true;
     }},
    {"Image", [](UIElementComponent& u) {
         u.Type = Kind::Image;
         u.Size = {160.0f, 160.0f};
         u.Color = {1.0f, 1.0f, 1.0f, 1.0f};
     }},
    {"Bar", [](UIElementComponent& u) {
         u.Type = Kind::Bar;
         u.Size = {240.0f, 26.0f};
         u.Value = 0.6f; // наполовину: пустая полоса неотличима от панели
         u.Rounding = 6.0f;
     }},
    {"Checkbox", [](UIElementComponent& u) {
         u.Type = Kind::Checkbox;
         u.Size = {200.0f, 36.0f};
         u.Text = "Checkbox";
         u.Interactive = true;
     }},
    {"Slider", [](UIElementComponent& u) {
         u.Type = Kind::Slider;
         u.Size = {240.0f, 30.0f};
         u.Interactive = true;
         u.MinValue = 0.0f;
         u.MaxValue = 1.0f;
         u.Value = 0.5f;
     }},
    {"Input", [](UIElementComponent& u) {
         u.Type = Kind::Input;
         u.Size = {260.0f, 40.0f};
         u.Interactive = true;
         u.Placeholder = "Enter text";
         u.TextCentered = false;
     }},
};

} // namespace

const std::vector<std::string>& PresetNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const Preset& p : kPresets) out.emplace_back(p.Name);
        return out;
    }();
    return names;
}

bool ApplyPreset(UIElementComponent& element, const std::string& preset) {
    for (const Preset& p : kPresets) {
        if (preset == p.Name) {
            p.Apply(element);
            return true;
        }
    }
    return false;
}

} // namespace sage::ui
