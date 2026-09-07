#include "ui/PropertyRows.h"

#include <cstdio>

#include "../Localization.h"

namespace sui = sage::ui::sui;
using sage::scene::Property;

namespace editor {
namespace {

// Числовое поле под одну компоненту вектора. Выделено, потому что таких полей в
// строке бывает три, и все три обязаны вести себя одинаково.
sui::NumericField* NumberField(sui::UIContext& ui, sui::UIElement* row, const Property& prop,
                               const DataFn& data, int component, const char* label,
                               const PropertyHooks& hooks) {
    float value = 0.0f;
    sage::scene::PropertyGetFloat(data(), prop, component, value);
    sui::NumericField* f = ui.CreateIn<sui::NumericField>(row, value);
    f->SetStretch(true, false);
    if (label) f->SetLabel(label);
    if (prop.HasRange()) f->SetRange(prop.Min, prop.Max);
    // Шаг перетаскивания — от размаха, а не «0.01 всегда»: у дальности отсечения
    // в двести метров сотая доля не двигает ничего, а у прозрачности 0..1
    // единица перепрыгивает весь диапазон.
    f->SetStep(prop.HasRange() ? (prop.Max - prop.Min) / 400.0f : 0.02f);
    f->OnChanged([&prop, data, component, hooks](float v) {
        void* ptr = data();
        if (!ptr) return;   // компонент убрали, пока строка ещё на экране
        if (hooks.BeginEdit) hooks.BeginEdit();
        sage::scene::PropertySetFloat(ptr, prop, component, v);
        if (hooks.Changed) hooks.Changed();
    });
    return f;
}

// Подпись строки: своя для каждого свойства и переведённая. Перевод именно
// здесь: движок хранит английский литерал и ничего не переводит.
const char* Caption(const Property& prop) { return T(prop.Label ? prop.Label : ""); }

} // namespace

void BuildPropertyRow(sui::UIContext& ui, sui::PropertyGrid* grid, sui::UIElement* section,
                      const Property& prop, DataFn data, const PropertyHooks& hooks) {
    if (!grid || !section || !data || !data()) return;
    const bool readOnly = prop.Editor == Property::Widget::ReadOnly;

    switch (prop.Type) {
        case Property::Kind::Bool: {
            sui::UIElement* row = grid->AddRow(section, Caption(prop));
            const bool on = sage::scene::FieldAs<bool>(data(), prop);
            sui::Checkbox* box = ui.CreateIn<sui::Checkbox>(row, std::string(), on);
            box->SetEnabled(!readOnly);
            box->OnToggle([&prop, data, hooks](bool v) {
                void* ptr = data();
                if (!ptr) return;
                if (hooks.BeginEdit) hooks.BeginEdit();
                sage::scene::FieldAs<bool>(ptr, prop) = v;
                if (hooks.Changed) hooks.Changed();
            });
            break;
        }
        case Property::Kind::Enum: {
            sui::UIElement* row = grid->AddRow(section, Caption(prop));
            std::vector<std::string> names;
            names.reserve((size_t)prop.EnumCount);
            for (int i = 0; i < prop.EnumCount; ++i) names.push_back(prop.EnumNames[i]);
            const int index = sage::scene::FieldAs<int>(data(), prop);
            sui::Dropdown* drop = ui.CreateIn<sui::Dropdown>(row, names, index);
            drop->SetStretch(true, false);
            drop->SetEnabled(!readOnly);
            drop->OnChanged([&prop, data, hooks](int v) {
                void* ptr = data();
                if (!ptr) return;
                if (hooks.BeginEdit) hooks.BeginEdit();
                sage::scene::FieldAs<int>(ptr, prop) = v;
                if (hooks.Changed) hooks.Changed();
            });
            break;
        }
        case Property::Kind::Float:
        case Property::Kind::Int: {
            sui::UIElement* row = grid->AddRow(section, Caption(prop));
            if (prop.Editor == Property::Widget::Slider && prop.HasRange()) {
                // Ползунок там, где границы ЕСТЬ и они осмысленны: у
                // прозрачности 0..1 тянуть удобнее, чем набирать.
                float value = 0.0f;
                sage::scene::PropertyGetFloat(data(), prop, 0, value);
                sui::Slider* s = ui.CreateIn<sui::Slider>(row, value, prop.Min, prop.Max);
                s->SetStretch(true, false);
                s->SetEnabled(!readOnly);
                s->OnValueChanged([&prop, data, hooks](float v) {
                    void* ptr = data();
                    if (!ptr) return;
                    if (hooks.BeginEdit) hooks.BeginEdit();
                    sage::scene::PropertySetFloat(ptr, prop, 0, v);
                    if (hooks.Changed) hooks.Changed();
                });
            } else {
                NumberField(ui, row, prop, data, 0, nullptr, hooks)->SetEnabled(!readOnly);
            }
            break;
        }
        case Property::Kind::Vec2:
        case Property::Kind::Vec3: {
            sui::UIElement* row = grid->AddRow(section, Caption(prop));
            row->Horizontal(4.0f);
            // Буквы осей — у полей, а не в подписи строки: «X Y Z» перед
            // числами читается сразу, а «Position (X, Y, Z)» приходится
            // сопоставлять глазами.
            static const char* kAxes[] = {"X", "Y", "Z"};
            const int n = prop.Type == Property::Kind::Vec2 ? 2 : 3;
            for (int i = 0; i < n; ++i)
                NumberField(ui, row, prop, data, i, kAxes[i], hooks)->SetEnabled(!readOnly);
            break;
        }
        case Property::Kind::Color: {
            sui::UIElement* row = grid->AddRow(section, Caption(prop));
            row->Horizontal(4.0f);
            // Образец цвета слева, числа справа. Образец — не украшение: по
            // трём числам 0..1 цвет не читается, а подобрать оттенок вслепую
            // нельзя.
            // Обычный элемент, а не Panel: у Panel есть СТИЛЬ, и тема
            // перекрашивает её заливку обратно на каждом кадре — образец
            // показывал бы цвет панели, а не цвет свойства.
            sui::UIElement* swatch = ui.CreateIn<sui::UIElement>(row);
            swatch->SetName("Swatch");
            swatch->SetSize({22.0f, 16.0f});
            const glm::vec3& c = sage::scene::FieldAs<glm::vec3>(data(), prop);
            sage::ui::UIFill& fill = swatch->Ensure<sage::ui::UIFill>();
            fill.Color = sage::ui::UIColor(c.r, c.g, c.b, 1.0f);
            fill.Radius = sage::ui::UICorners(3.0f);
            static const char* kRgb[] = {"R", "G", "B"};
            for (int i = 0; i < 3; ++i) {
                sui::NumericField* f = NumberField(ui, row, prop, data, i, kRgb[i], hooks);
                f->SetRange(0.0f, 1.0f);
                f->SetStep(0.004f);
                f->SetEnabled(!readOnly);
                // Образец обновляется вместе с числом: иначе он показывает
                // прежний цвет и врёт ровно тогда, когда на него смотрят.
                f->OnChanged([&prop, data, swatch](float) {
                    void* ptr = data();
                    if (!ptr) return;
                    const glm::vec3& v = sage::scene::FieldAs<glm::vec3>(ptr, prop);
                    if (sage::ui::UIFill* fl = swatch->Get<sage::ui::UIFill>()) {
                        fl->Color = sage::ui::UIColor(v.r, v.g, v.b, 1.0f);
                        swatch->Dirty(sage::ui::UIDirty_Visual);
                    }
                });
            }
            break;
        }
        case Property::Kind::String: {
            sui::UIElement* row = grid->AddRow(section, Caption(prop));
            row->Horizontal(4.0f);
            const std::string text = sage::scene::FieldAs<std::string>(data(), prop);
            sui::TextInput* input = ui.CreateIn<sui::TextInput>(row, text, std::string());
            input->SetStretch(true, false);
            input->SetEnabled(!readOnly);
            input->OnChanged([&prop, data, hooks](const std::string& v) {
                void* ptr = data();
                if (!ptr) return;
                if (hooks.BeginEdit) hooks.BeginEdit();
                sage::scene::FieldAs<std::string>(ptr, prop) = v;
                if (hooks.Changed) hooks.Changed();
            });
            // Путь к ассету — с кнопкой выбора. Набирать путь руками можно, но
            // человек так не работает: он выбирает файл.
            if (prop.Editor == Property::Widget::Asset && hooks.PickAsset && !readOnly) {
                sui::IconButton* pick = ui.CreateIn<sui::IconButton>(
                    row, sui::Icon::Folder, std::string(T("Choose a file")));
                pick->OnPress([&prop, data, input, hooks] {
                    void* ptr = data();
                    if (!ptr) return;
                    std::string& value = sage::scene::FieldAs<std::string>(ptr, prop);
                    const std::string was = value;
                    hooks.PickAsset(prop, value);
                    if (value == was) return;
                    if (hooks.BeginEdit) hooks.BeginEdit();
                    input->SetValue(value);
                    if (hooks.Changed) hooks.Changed();
                });
            }
            break;
        }
        default: break;
    }
}

} // namespace editor
