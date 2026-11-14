// ===========================================================================
//  ЭЛЕМЕНТЫ УПРАВЛЕНИЯ (этап 6 пространства «Интерфейс»): Dropdown, Toggle
//  (Switch), вертикальный Slider, Text Input (проверка, Enter, несколько
//  строк), навигация фокусом, звуки, содержимое кнопки.
//
//  Проверяется настоящим вводом через UpdateSceneUI, а не полями компонентов.
// ===========================================================================
#include "TestFramework.h"

#include "sage/scene/Scene.h"
#include "sage/scene/Signals.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIButton.h"
#include "sage/ui/UIControls.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISceneSystem.h"

using sage::ui::UIInputState;
using sage::ui::UIRect;

namespace {

GameObject Make(Scene& scene, const char* preset, glm::vec2 pos, glm::vec2 size) {
    GameObject o = scene.CreateObject(preset);
    sage::ui::ApplyPreset(scene, o.Entity(), preset);
    sage::ui::Element& el = scene.Registry().get<sage::ui::Element>(o.Entity());
    el.Anchor = UIAnchor::TopLeft;
    el.Position = pos;
    el.Size = size;
    return o;
}

sage::ui::UIInputResult Frame(Scene& scene, UIInputState in) { return sage::ui::UpdateSceneUI(scene, in, 800, 600); }

sage::ui::UIInputResult Press(Scene& scene, glm::vec2 p) {
    UIInputState in;
    in.Mouse = p;
    in.MouseDown = in.MousePressed = true;
    return Frame(scene, in);
}
sage::ui::UIInputResult Release(Scene& scene, glm::vec2 p) {
    UIInputState in;
    in.Mouse = p;
    in.MouseReleased = true;
    return Frame(scene, in);
}
sage::ui::UIInputResult Click(Scene& scene, glm::vec2 p) {
    Press(scene, p);
    return Release(scene, p);
}
sage::ui::UIInputResult Key(Scene& scene, bool UIInputState::*key, bool shift = false) {
    UIInputState in;
    in.*key = true;
    in.Shift = shift;
    return Frame(scene, in);
}

} // namespace

TEST(UIControls_presets_have_the_spec_names) {
    for (const char* name : {"Button", "Toggle", "Slider", "Text Input", "Dropdown"})
        CHECK_TRUE(sage::ui::FindPreset(name) != nullptr);
    CHECK_TRUE(sage::ui::FindPreset("Checkbox") == nullptr);
    CHECK_TRUE(sage::ui::FindPreset("Input Field") == nullptr);
    Scene scene("t");
    GameObject d = Make(scene, "Dropdown", {0, 0}, {200, 40});
    scene.Registry().get<sage::ui::Element>(d.Entity()).Type.clear();
    CHECK_TRUE(sage::ui::InferType(scene.Registry(), d.Entity()) == "Dropdown");
}

TEST(UIDropdown_click_opens_and_a_row_selects) {
    Scene scene("dd");
    GameObject d = Make(scene, "Dropdown", {100, 100}, {200, 40});
    auto& dd = scene.Registry().get<sage::ui::Dropdown>(d.Entity());
    dd.Options = "Low\nMedium\nHigh";
    int heard = -1;
    scene.Events.Connect(d.Id(), sage::signals::kValueChanged,
                         [&](const sage::events::Event& e) { heard = e.Arg.AsInt(); });
    Click(scene, {150, 120});
    CHECK_TRUE(dd.Open);
    // Список — под полем, строки по 40: вторая строка — y 180..220.
    Click(scene, {150, 200});
    CHECK_FALSE(dd.Open);
    CHECK_EQ(dd.Selected, 1);
    CHECK_EQ(heard, 1);
}

TEST(UIDropdown_opens_above_near_the_bottom_and_stays_on_screen) {
    sage::ui::Dropdown dd;
    dd.Options = "a\nb\nc\nd";
    sage::ui::DropdownLayout lay;
    CHECK_TRUE(sage::ui::DropdownListLayout(dd, {700, 540, 200, 40}, {800, 600}, lay));
    CHECK_NEAR(lay.List.y + lay.List.h, 540.0f, 0.01f);   // над полем
    CHECK_NEAR(lay.List.x, 600.0f, 0.01f);                // не за правым краем
    CHECK_EQ(lay.Visible, 4);
}

TEST(UIDropdown_disabled_option_is_not_selected) {
    Scene scene("dd");
    GameObject d = Make(scene, "Dropdown", {100, 100}, {200, 40});
    auto& dd = scene.Registry().get<sage::ui::Dropdown>(d.Entity());
    dd.Options = "Low\nMedium\nHigh";
    dd.Disabled = "Medium";
    Click(scene, {150, 120});
    Click(scene, {150, 200});
    CHECK_EQ(dd.Selected, -1);
    CHECK_TRUE(dd.Open);   // щелчок по недоступной строке ничего не делает
}

TEST(UIDropdown_open_list_takes_the_click_from_what_is_below) {
    Scene scene("dd");
    GameObject d = Make(scene, "Dropdown", {100, 100}, {200, 40});
    GameObject b = Make(scene, "Button", {100, 150}, {200, 40});
    scene.Registry().get<sage::ui::Dropdown>(d.Entity()).Options = "One\nTwo\nThree";
    Click(scene, {150, 120});
    const auto r = Click(scene, {150, 170});   // кнопка под раскрытым списком
    CHECK_TRUE(r.ClickedId != b.Id());
    CHECK_EQ(scene.Registry().get<sage::ui::Dropdown>(d.Entity()).Selected, 0);
}

TEST(UIDropdown_keyboard_chooses) {
    Scene scene("dd");
    GameObject d = Make(scene, "Dropdown", {100, 100}, {200, 40});
    auto& dd = scene.Registry().get<sage::ui::Dropdown>(d.Entity());
    dd.Options = "Low\nMedium\nHigh";
    Key(scene, &UIInputState::Down);    // фокус на список
    Key(scene, &UIInputState::Enter);   // раскрыть
    CHECK_TRUE(dd.Open);
    Key(scene, &UIInputState::Down);
    Key(scene, &UIInputState::Down);
    Key(scene, &UIInputState::Enter);
    CHECK_EQ(dd.Selected, 1);
    CHECK_FALSE(dd.Open);
}

TEST(UITextInput_validation_filters_typed_characters) {
    Scene scene("in");
    GameObject f = Make(scene, "Text Input", {0, 0}, {200, 40});
    scene.Registry().get<sage::ui::TextInput>(f.Entity()).Validation = sage::ui::TextInput::Rule::Integer;
    Click(scene, {20, 20});
    UIInputState in;
    in.TypedText = "12a-3";
    Frame(scene, in);
    CHECK_TRUE(scene.Registry().get<sage::ui::Label>(f.Entity()).Text == "123");
    CHECK_TRUE(sage::ui::InputAccepts(sage::ui::TextInput::Rule::Decimal, "", "-"));
    CHECK_FALSE(sage::ui::InputAccepts(sage::ui::TextInput::Rule::Decimal, "1.5", "."));
}

TEST(UITextInput_enter_submits_or_breaks_the_line) {
    Scene scene("in");
    GameObject f = Make(scene, "Text Input", {0, 0}, {200, 40});
    std::string submitted;
    scene.Events.Connect(f.Id(), sage::signals::kSubmitted,
                         [&](const sage::events::Event& e) { submitted = e.Arg.AsString(); });
    Click(scene, {20, 20});
    UIInputState in;
    in.TypedText = "Ann";
    Frame(scene, in);
    Key(scene, &UIInputState::Enter);
    CHECK_TRUE(submitted == "Ann");
    CHECK_FALSE(scene.Registry().get<sage::ui::Interactable>(f.Entity()).Runtime.Focused);

    scene.Registry().get<sage::ui::TextInput>(f.Entity()).Multiline = true;
    Click(scene, {20, 20});
    Key(scene, &UIInputState::Enter);
    in.TypedText = "B";
    Frame(scene, in);
    CHECK_TRUE(scene.Registry().get<sage::ui::Label>(f.Entity()).Text == "Ann\nB");
}

TEST(UIToggle_switch_knob_slides_over_time) {
    Scene scene("sw");
    GameObject t = Make(scene, "Toggle", {0, 0}, {200, 40});
    auto& r = scene.Registry().get<sage::ui::Range>(t.Entity());
    r.ToggleVariant = sage::ui::Range::Variant::Switch;
    r.AnimationTime = 0.2f;
    Frame(scene, UIInputState{});
    CHECK_NEAR(r.Shown, 0.0f, 0.001f);
    Click(scene, {20, 20});
    CHECK_NEAR(r.Value, 1.0f, 0.001f);
    UIInputState step;
    step.DeltaTime = 0.1f;
    Frame(scene, step);
    CHECK_TRUE(r.Shown > 0.2f && r.Shown < 0.8f);   // едет, а не прыгает
    Frame(scene, step);
    Frame(scene, step);
    CHECK_NEAR(r.Shown, 1.0f, 0.001f);
}

TEST(UISlider_vertical_runs_from_bottom_to_top) {
    Scene scene("sl");
    GameObject s = Make(scene, "Slider", {0, 0}, {30, 200});
    auto& r = scene.Registry().get<sage::ui::Range>(s.Entity());
    r.Vertical = true;
    Press(scene, {15, 199});
    CHECK_NEAR(r.Value, 0.0f, 0.02f);
    UIInputState hold;
    hold.Mouse = {15, 0};
    hold.MouseDown = true;
    Frame(scene, hold);
    CHECK_NEAR(r.Value, 1.0f, 0.02f);
}

TEST(UINavigation_arrows_move_focus_and_enter_clicks) {
    Scene scene("nav");
    GameObject a = Make(scene, "Button", {100, 100}, {200, 40});
    GameObject b = Make(scene, "Button", {100, 200}, {200, 40});
    GameObject c = Make(scene, "Button", {400, 100}, {200, 40});
    auto focused = [&](GameObject o) {
        return scene.Registry().get<sage::ui::Interactable>(o.Entity()).Runtime.Focused;
    };
    Key(scene, &UIInputState::Down);   // первый — показать фокус
    CHECK_TRUE(focused(a));
    Key(scene, &UIInputState::Down);
    CHECK_TRUE(focused(b));
    Key(scene, &UIInputState::Up);
    Key(scene, &UIInputState::Right);
    CHECK_TRUE(focused(c));
    const auto r = Key(scene, &UIInputState::Enter);
    CHECK_EQ(r.ClickedId, c.Id());
    Key(scene, &UIInputState::Tab, /*shift=*/true);
    CHECK_TRUE(focused(b));
}

TEST(UIButton_sounds_go_to_the_frame_owner) {
    Scene scene("snd");
    GameObject b = Make(scene, "Button", {0, 0}, {200, 40});
    auto& act = scene.Registry().get<sage::ui::Interactable>(b.Entity());
    act.HoverSound = "sounds/hover.wav";
    act.ClickSound = "sounds/click.wav";
    const auto pressed = Press(scene, {20, 20});
    CHECK_EQ((int)pressed.Sounds.size(), 1);   // навели
    const auto released = Release(scene, {20, 20});
    CHECK_EQ((int)released.Sounds.size(), 1);
    CHECK_TRUE(released.Sounds[0] == "sounds/click.wav");
}

TEST(UIButton_content_icon_and_label_in_a_row) {
    Scene scene("btn");
    GameObject b = Make(scene, "Button", {0, 0}, {200, 50});
    sage::ui::ButtonContent content;
    CHECK_TRUE(sage::ui::ReadButtonContent(scene, b.Entity(), content));
    CHECK_TRUE(content.Label == "Button");
    content.Label = "Play";
    content.Icon = "play";
    content.Alignment = sage::ui::Arrangement::Spread::Start;
    sage::ui::ApplyButtonContent(scene, b.Entity(), content);
    entt::registry& reg = scene.Registry();
    CHECK_TRUE(reg.all_of<sage::ui::Arrangement>(b.Entity()));
    sage::ui::ButtonContent back;
    sage::ui::ReadButtonContent(scene, b.Entity(), back);
    CHECK_TRUE(back.Icon == "play");
    CHECK_TRUE(back.Label == "Play");
    CHECK_TRUE(back.Alignment == sage::ui::Arrangement::Spread::Start);
    // Значок убран — ряд не нужен, подпись снова во всю кнопку.
    back.Icon.clear();
    sage::ui::ApplyButtonContent(scene, b.Entity(), back);
    CHECK_FALSE(reg.all_of<sage::ui::Arrangement>(b.Entity()));
    CHECK_EQ((int)scene.ChildrenOf(b.Entity()).size(), 1);
}
