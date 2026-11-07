// Интерфейс в стиле SAGE (ScriptApi_UIKit.cpp): ui.screen{}, ui.button{}…
//
// Один путь вместо четырёх: строитель на вид элемента, свойства
// присваиванием, вид состояний кнопки таблицей, нажатие — функцией.
#include "TestFramework.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scripting/ScriptEngine.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIInteraction.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/components/Interact.h"
#include "sage/ui/components/Layout.h"
#include "sage/ui/components/Visual.h"

namespace {

entt::entity Named(Scene& scene, const std::string& name) {
    for (auto e : scene.Registry().view<NameComponent>())
        if (scene.Registry().get<NameComponent>(e).Name == name) return e;
    return entt::null;
}

void Click(Scene& scene, glm::vec2 at) {
    sage::ui::UIInputState down;
    down.Mouse = at;
    down.MouseDown = true;
    down.MousePressed = true;
    sage::ui::UpdateSceneUI(scene, down, 800, 600);
    sage::ui::UIInputState up;
    up.Mouse = at;
    up.MouseReleased = true;
    sage::ui::UpdateSceneUI(scene, up, 800, 600);
}

bool Throws(ScriptEngine& se, const char* code, const char* needle) {
    sol::protected_function_result r = se.Lua().safe_script(code, sol::script_pass_on_error);
    if (r.valid()) return false;
    sol::error e = r;
    return std::string(e.what()).find(needle) != std::string::npos;
}

} // namespace

TEST(UIKit_builders_make_elements_with_their_parts) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    se.Lua().script(R"(
        local s = ui.screen{name = "Menu", order = 5}
        local p = ui.panel{parent = s, name = "Back", x = 10, y = 20, w = 300, h = 200, color = {0, 0, 0, 0.5}}
        local t = ui.text{parent = p, name = "Title", text = "Hello", size = 16, align = "center"}
        t.text = "Options"
        t.x = 4
        p.anchor = "center"
        p.visible = false
        ui.group{parent = s, name = "List", layout = "column", gap = 4}
    )");
    auto& reg = scene.Registry();
    const entt::entity screen = Named(scene, "Menu"), back = Named(scene, "Back");
    const entt::entity title = Named(scene, "Title"), list = Named(scene, "List");
    CHECK_TRUE(reg.get<sage::ui::Element>(screen).Mode == sage::ui::Element::Stretch::Both);
    CHECK_EQ(reg.get<sage::ui::Canvas>(screen).SortOrder, 5);
    CHECK_NEAR(reg.get<sage::ui::Element>(back).Size.x, 300.0f, 1e-4f);
    CHECK_TRUE(reg.get<sage::ui::Element>(back).Anchor == UIAnchor::Center);
    CHECK_FALSE(reg.get<sage::ui::Element>(back).Active);
    CHECK_NEAR(reg.get<sage::ui::Fill>(back).Color.a, 0.5f, 1e-4f);
    CHECK_TRUE(reg.get<sage::ui::Label>(title).Text == "Options");
    CHECK_NEAR(reg.get<sage::ui::Label>(title).Scale, 2.0f, 1e-4f);   // строка 16 пикселей
    CHECK_NEAR(reg.get<sage::ui::Element>(title).Position.x, 4.0f, 1e-4f);
    CHECK_TRUE(reg.get<sage::ui::Stack>(list).Direction == sage::ui::Stack::Flow::Vertical);
}

TEST(UIKit_typos_are_errors_not_silence) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    CHECK_TRUE(Throws(se, "ui.panel{colour = {1, 0, 0, 1}}", "неизвестный ключ 'colour'"));
    CHECK_TRUE(Throws(se, "local p = ui.panel{} p.colr = 1", "нет свойства 'colr'"));
    CHECK_TRUE(Throws(se, "local p = ui.panel{} p.anchor = 'middle'", "anchor 'middle'"));
    CHECK_TRUE(Throws(se, "ui.panel{fit = 'zoom', image = 'a.png'}", "fit 'zoom'"));
}

TEST(UIKit_button_states_come_from_the_sheet_and_click_calls_back) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "sage_uikit_sheet";
    fs::create_directories(dir);
    const std::string image = (dir / "widgets.png").generic_string();
    {
        std::ofstream f(image + ".sagesheet");
        f << R"({"sprites": [
            {"name": "button", "rect": [0, 0, 200, 20], "slice": [2, 2, 2, 3]},
            {"name": "button_hover", "rect": [0, 20, 200, 20], "slice": [2, 2, 2, 3]}]})";
    }
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    se.Lua()["IMAGE"] = image;
    se.Lua().script(R"(
        clicks = 0
        local b = ui.button{name = "Play", w = 200, h = 40, text = "Play",
            image = IMAGE, sprite = "button", pixel = 2, hover = {sprite = "button_hover"},
            hover_text_color = {1, 1, 0.6, 1},
            on_click = function(btn) clicks = clicks + 1; btn.text = "Again" end}
    )");
    auto& reg = scene.Registry();
    const entt::entity b = Named(scene, "Play");
    const auto& fill = reg.get<sage::ui::Fill>(b);
    CHECK_TRUE(fill.Texture == image);
    CHECK_NEAR(fill.Sprite.y, 0.0f, 1e-4f);
    CHECK_EQ(fill.Fit, 1);                       // кусок с девятиной ложится девятиной
    CHECK_NEAR(fill.SliceBorder.w, 3.0f, 1e-4f);
    CHECK_TRUE(fill.Sharp());                    // наборы — ближайшим соседом
    const auto& act = reg.get<sage::ui::Interactable>(b);
    CHECK_TRUE(act.UseHoverLook);
    CHECK_NEAR(act.HoverLook.Sprite.y, 20.0f, 1e-4f);
    CHECK_NEAR(act.HoverLook.PixelScale, 2.0f, 1e-4f);   // состояние наследует обычный вид

    Click(scene, {100.0f, 20.0f});
    const int clicks = se.Lua()["clicks"];
    CHECK_EQ(clicks, 1);
    CHECK_TRUE(reg.get<sage::ui::Label>(Named(scene, "Play Text")).Text == "Again");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(UIKit_slider_reports_value_in_game_units) {
    ScriptEngine se;
    Scene scene("S");
    se.BindScene(scene);
    se.Lua().script(R"(
        s = ui.slider{name = "Volume", w = 200, h = 20, min = 0, max = 100, value = 40, step = 5,
            knob = {color = {1, 1, 1, 1}}}
        s.value = 72
    )");
    const float v = se.Lua().script("return s.value");
    CHECK_NEAR(v, 72.0f, 1e-3f);
    const auto& range = scene.Registry().get<sage::ui::Range>(Named(scene, "Volume"));
    CHECK_NEAR(range.Step, 5.0f, 1e-4f);
    CHECK_NEAR(range.Knob.Color.r, 1.0f, 1e-4f);
}
