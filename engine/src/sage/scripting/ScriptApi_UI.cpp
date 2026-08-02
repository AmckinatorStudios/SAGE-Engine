#include "ScriptEngine.h"

#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/ui/UIShowcase.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/UIIcons.h"

// ---------------------------------------------------------------------------
// Интерфейс сцены: UIElementComponent и sage.ui.*
//
// Часть Lua-API движка. Раньше ВСЕ привязки жили в одном ScriptEngine.cpp на
// 1800 строк: 126 функций, восемнадцать областей, и чтобы дописать одну
// строчку про анимацию, приходилось листать интерфейс, физику и таймеры.
// Определения разъехались по файлам ScriptApi_*.cpp — по файлу на область;
// объявления методов остались в ScriptEngine.h, поэтому порядок регистрации
// по-прежнему записан в одном месте (RegisterEngineApi) и не зависит от того,
// в каком файле лежит тело.
// ---------------------------------------------------------------------------

void ScriptEngine::RegisterUIApi() {
    // --- UI: интерфейс сцены (см. UIElementComponent в Components.h).
    // Скрипты правят текст/значение/видимость прямо в компоненте — так худ
    // (здоровье, счёт, таймеры) обновляется из игровой логики. ---
    m_lua.new_enum<UIElementComponent::Kind>("UIKind", {
        {"Panel", UIElementComponent::Kind::Panel},
        {"Icon",  UIElementComponent::Kind::Icon},
        {"Input", UIElementComponent::Kind::Input},
        {"Checkbox", UIElementComponent::Kind::Checkbox},
        {"Slider", UIElementComponent::Kind::Slider},
        {"Label", UIElementComponent::Kind::Label},
        {"Image", UIElementComponent::Kind::Image},
        {"Bar",   UIElementComponent::Kind::Bar},
    });
    m_lua.new_enum<UIAnchor>("UIAnchor", {
        {"TopLeft", UIAnchor::TopLeft},       {"TopCenter", UIAnchor::TopCenter},
        {"TopRight", UIAnchor::TopRight},     {"CenterLeft", UIAnchor::CenterLeft},
        {"Center", UIAnchor::Center},         {"CenterRight", UIAnchor::CenterRight},
        {"BottomLeft", UIAnchor::BottomLeft}, {"BottomCenter", UIAnchor::BottomCenter},
        {"BottomRight", UIAnchor::BottomRight},
    });
    m_lua.new_usertype<UIElementComponent>("UIElementComponent",
        "Type", &UIElementComponent::Type,
        "Anchor", &UIElementComponent::Anchor,
        "Offset", &UIElementComponent::Offset,
        "Size", &UIElementComponent::Size,
        "Layer", &UIElementComponent::Layer,
        "Visible", &UIElementComponent::Visible,
        "ClipChildren", &UIElementComponent::ClipChildren,
        "Color", &UIElementComponent::Color,
        "Rounding", &UIElementComponent::Rounding,
        "BorderThickness", &UIElementComponent::BorderThickness,
        "BorderColor", &UIElementComponent::BorderColor,
        "Text", &UIElementComponent::Text,
        "TextScale", &UIElementComponent::TextScale,
        "TextColor", &UIElementComponent::TextColor,
        "TextCentered", &UIElementComponent::TextCentered,
        "WrapText", &UIElementComponent::WrapText,
        "Value", &UIElementComponent::Value,
        "BarFillColor", &UIElementComponent::BarFillColor,
        "Icon", &UIElementComponent::Icon,
        "IconColor", &UIElementComponent::IconColor,
        "GradientColor", &UIElementComponent::GradientColor,
        "ShadowSize", &UIElementComponent::ShadowSize,
        "Sprite", &UIElementComponent::Sprite,
        "SpriteHover", &UIElementComponent::SpriteHover,
        "SpritePressed", &UIElementComponent::SpritePressed,
        "Interactive", &UIElementComponent::Interactive,
        "Enabled", &UIElementComponent::Enabled,
        "Placeholder", &UIElementComponent::Placeholder,
        "MaxLength", &UIElementComponent::MaxLength,
        "Password", &UIElementComponent::Password,
        "MinValue", &UIElementComponent::MinValue,
        "MaxValue", &UIElementComponent::MaxValue,
        "Hovered", sol::readonly(&UIElementComponent::Hovered),
        "Pressed", sol::readonly(&UIElementComponent::Pressed),
        "Focused", &UIElementComponent::Focused,
        "Clicked", sol::readonly(&UIElementComponent::Clicked),
        "Changed", sol::readonly(&UIElementComponent::Changed),
        "SliceBorder", &UIElementComponent::SliceBorder,
        "PixelScale", &UIElementComponent::PixelScale,
        "PixelArt", &UIElementComponent::PixelArt,
        "TexturePath", &UIElementComponent::TexturePath,
        "PadX", &UIElementComponent::PadX,
        "AutoWidth", &UIElementComponent::AutoWidth,
        "LayoutSize", sol::readonly(&UIElementComponent::LayoutSize)
    );

    // Картинка UI-элемента: путь + признак пиксель-арта. Отдельной функцией, а
    // не полем TexturePath, потому что путь без ЗАГРУЗКИ ничего не рисует, а
    // загрузить надо с правильной фильтрацией — из скрипта об этом помнить
    // незачем.
    Bind("ui", "SetImage", "SetUIImage", [](GameObject& obj, const std::string& path,
                                        sol::optional<bool> pixelArt) {
        if (!obj.Valid()) return;
        auto* ui = obj.Registry()->try_get<UIElementComponent>(obj.Entity());
        if (!ui) return;
        ui->TexturePath = path;
        ui->PixelArt = pixelArt.value_or(false);
        ui->Tex = path.empty() ? nullptr
                  : ui->PixelArt
                      ? ResourceManager::Instance().GetTexture(path, TextureFilter::Nearest, false)
                      : ResourceManager::Instance().GetTexture(path);
    });

    // Спрайт из листа: кусок в ПИКСЕЛЯХ исходника. Так набор интерфейса
    // описывается ровно теми числами, что напечатаны в его документации.
    Bind("ui", "SetSprite", "SetUISprite", [](GameObject& obj, float x, float y, float w, float h) {
        if (!obj.Valid()) return;
        if (auto* ui = obj.Registry()->try_get<UIElementComponent>(obj.Entity()))
            ui->Sprite = glm::vec4(x, y, w, h);
    });

    // Девятина: неподвижные углы в пикселях исходника (лево, верх, право, низ).
    Bind("ui", "SetSlice", "SetUISlice", [](GameObject& obj, float l, float t, float r, float b,
                                        sol::optional<float> scale) {
        if (!obj.Valid()) return;
        if (auto* ui = obj.Registry()->try_get<UIElementComponent>(obj.Entity())) {
            ui->SliceBorder = glm::vec4(l, t, r, b);
            ui->PixelScale = scale.value_or(0.0f);
        }
    });

    // Значение ползунка/галки в ЕДИНИЦАХ ИГРЫ. Внутри Value всегда 0..1, а
    // игре нужно 0..100 или -180..180: перевод обязан быть в одном месте,
    // иначе он расползётся по вызывающим и где-то разойдётся.
    Bind("ui", "GetValue", "GetUIValue", [](GameObject& obj) -> float {
        if (!obj.Valid()) return 0.0f;
        const auto* ui = obj.Registry()->try_get<UIElementComponent>(obj.Entity());
        if (!ui) return 0.0f;
        return ui->MinValue + (ui->MaxValue - ui->MinValue) * ui->Value;
    });
    Bind("ui", "SetValue", "SetUIValue", [](GameObject& obj, float value) {
        if (!obj.Valid()) return;
        auto* ui = obj.Registry()->try_get<UIElementComponent>(obj.Entity());
        if (!ui) return;
        const float span = ui->MaxValue - ui->MinValue;
        ui->Value = std::abs(span) < 1e-6f ? 0.0f
                                           : glm::clamp((value - ui->MinValue) / span, 0.0f, 1.0f);
    });

    // Размер картинки в пикселях — по нему скрипт нарезает лист на спрайты, не
    // зная его размера заранее.
    Bind("ui", "ImageSize", "ImageSize", [](const std::string& path) {
        std::shared_ptr<Texture> tex = ResourceManager::Instance().GetTexture(path);
        if (!tex) return std::make_tuple(0, 0);
        return std::make_tuple(tex->Width(), tex->Height());
    });

    // Список доступных векторных иконок — чтобы игра могла проверить имя, а не
    // узнавать об опечатке по пустому месту в интерфейсе.
    Bind("ui", "HasIcon", "HasIcon", [](const std::string& name) {
        return sage::ui::HasIcon(name);
    });
    Bind("ui", "IconNames", "IconNames", [this]() -> sol::table {
        sol::table list = m_lua.create_table();
        int i = 1;
        for (const std::string& n : sage::ui::IconNames()) list[i++] = n;
        return list;
    });

    // Собирает боевой интерфейс (инвентарь + дерево навыков) из UIElementComponent
    // в сцену — плотный реальный экран одним вызовом. Возвращает id корня.
    Bind("ui", "SpawnShowcase", "SpawnUIShowcase", [this]() -> int {
        if (!m_scene) throw std::runtime_error("SpawnUIShowcase: сцена не привязана");
        return sage::ui::BuildShowcase(*m_scene);
    });

}

