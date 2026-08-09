#pragma once
#include <memory>
#include <string>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "sage/render/Texture.h"
#include "sage/ui/UI.h"

// ---------------------------------------------------------------------------
// СТАРЫЙ ФОРМАТ элемента интерфейса — и перевод его в компоненты.
//
// Это НЕ компонент и не модель данных. Это описание того, как элемент
// интерфейса выглядел в файлах сцен и префабов до перехода на компоненты: одна
// структура на сорок с лишним полей и перечисление видов, где у надписи были
// скругление, девятина и предел длины поля ввода — поля, ничего для неё не
// значащие.
//
// Он оставлен ровно для одного: ПРОЧИТАТЬ такие файлы. Сцена, сделанная до
// перехода, обязана открываться, а не встречать человека пустым экраном без
// единого сообщения. Записывается всё уже компонентами (см. SceneSerializer),
// поэтому однажды открытая и сохранённая сцена сюда больше не возвращается.
//
// Ничего, кроме миграции, этой структурой пользоваться не должно: она описывает
// прошлое, и всякая новая возможность интерфейса (растяжение, раскладка, холст,
// группы, имена действий) в ней отсутствует по определению.
// ---------------------------------------------------------------------------
namespace sage::ui {

struct LegacyElement {
    enum class Kind { Panel, Label, Image, Bar, Icon, Input, Checkbox, Slider };

    Kind Type = Kind::Panel;
    UIAnchor Anchor = UIAnchor::TopLeft;
    glm::vec2 Offset{16.0f, 16.0f};
    glm::vec2 Size{200.0f, 56.0f};
    int Layer = 0;
    bool Visible = true;
    bool ClipChildren = false;

    glm::vec4 Color{0.09f, 0.10f, 0.14f, 0.85f};
    float Rounding = 8.0f;
    float BorderThickness = 0.0f;
    glm::vec4 BorderColor{0.85f, 0.80f, 0.65f, 0.9f};

    std::string Text;
    float TextScale = 2.0f;
    glm::vec4 TextColor{1.0f, 1.0f, 1.0f, 1.0f};
    bool TextCentered = true;
    bool WrapText = false;

    std::string TexturePath;
    glm::vec4 Sprite{0.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 SliceBorder{0.0f, 0.0f, 0.0f, 0.0f};
    float PixelScale = 0.0f;
    bool PixelArt = false;
    glm::vec4 SpriteHover{0.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 SpritePressed{0.0f, 0.0f, 0.0f, 0.0f};

    bool Interactive = false;
    bool Enabled = true;

    std::string Placeholder;
    int MaxLength = 0;
    bool Password = false;

    float MinValue = 0.0f;
    float MaxValue = 1.0f;
    float Value = 1.0f;
    glm::vec4 BarFillColor{0.36f, 0.75f, 0.42f, 1.0f};

    std::string Icon;
    glm::vec4 IconColor{1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 GradientColor{0.0f, 0.0f, 0.0f, 0.0f};
    float ShadowSize = 0.0f;
    float PadX = 8.0f;
    bool AutoWidth = false;

    std::shared_ptr<Texture> Tex; // рантайм: загруженная текстура картинки
};

// Раскладывает старое описание на компоненты. Существующие части
// перезаписываются, отсутствующие в описании — не создаются.
void Decompose(const LegacyElement& flat, entt::registry& reg, entt::entity e);

} // namespace sage::ui
