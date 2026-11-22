// Реестр панелей редактора (editor/src/PanelRegistry.h): панель описывается
// один раз, а меню, команды, раскладка и сброс читают реестр.
#include "TestFramework.h"

#include "../editor/src/PanelRegistry.h"
#include "../editor/src/ui/Commands.h"

namespace pe = sage::editor;

namespace {
pe::PanelSpec Spec(const char* id, pe::PanelSection section) {
    pe::PanelSpec s;
    s.Id = id;
    s.Title = id;
    s.Section = section;
    return s;
}
} // namespace

TEST(PanelRegistry_menu_order_is_section_then_registration) {
    pe::PanelRegistry reg;
    reg.Register(Spec("Tools1", pe::PanelSection::Tools));
    reg.Register(Spec("SceneB", pe::PanelSection::Scene));
    reg.Register(Spec("SceneA", pe::PanelSection::Scene));
    const auto all = reg.All();
    CHECK_EQ((int)all.size(), 3);
    CHECK_EQ(all[0]->Id, std::string("SceneB"));   // раздел «Сцена» первым,
    CHECK_EQ(all[1]->Id, std::string("SceneA"));   // внутри — как регистрировали
    CHECK_EQ(all[2]->Id, std::string("Tools1"));
}

TEST(PanelRegistry_new_panel_gets_a_command_and_can_be_removed) {
    Sage::UI::CommandRegistry commands;
    pe::PanelRegistry reg;
    reg.AttachCommands(commands, "Window");
    reg.Register(Spec("Notes", pe::PanelSection::Tools));   // уже после AttachCommands
    CHECK_TRUE(commands.Find("panel.notes") != nullptr);
    CHECK_FALSE(reg.IsVisible("Notes"));
    CHECK_TRUE(commands.Run("panel.notes"));
    CHECK_TRUE(reg.IsVisible("Notes"));
    CHECK_TRUE(reg.Unregister("Notes"));
    CHECK_TRUE(commands.Find("panel.notes") == nullptr);   // в палитре не остаётся мёртвой строки
    CHECK_TRUE(reg.Find("Notes") == nullptr);
}

TEST(PanelRegistry_restore_opens_only_layout_panels_and_keeps_external_flags) {
    bool builtin = false;
    pe::PanelRegistry reg;
    pe::PanelSpec a = Spec("Hierarchy", pe::PanelSection::Scene);
    a.InLayout = true;
    a.Visible = &builtin;   // флаг живёт снаружи (EditorPanelVisibility)
    reg.Register(a);
    reg.Register(Spec("Profiler", pe::PanelSection::Tools));
    CHECK_FALSE(reg.AnyVisible());
    reg.Restore();
    CHECK_TRUE(builtin);                      // реестр правит ТОТ ЖЕ флаг
    CHECK_FALSE(reg.IsVisible("Profiler"));   // инструмент раскладкой не открывается
    CHECK_TRUE(reg.AnyVisible());
    reg.CloseAll();
    CHECK_FALSE(builtin);
}

TEST(PanelRegistry_replacing_a_panel_keeps_its_visibility_and_menu_place) {
    pe::PanelRegistry reg;
    reg.Register(Spec("First", pe::PanelSection::Plugins));
    reg.Register(Spec("Second", pe::PanelSection::Plugins));
    reg.SetVisible("First", true);
    pe::PanelSpec again = Spec("First", pe::PanelSection::Plugins);
    again.Title = "First v2";   // плагин перезагрузился
    reg.Register(again);
    CHECK_EQ((int)reg.Count(), 2);
    CHECK_TRUE(reg.IsVisible("First"));
    CHECK_EQ(reg.All()[0]->Title, std::string("First v2"));
}

TEST(PanelRegistry_layout_docks_by_space_and_order) {
    pe::PanelRegistry reg;
    pe::PanelSpec game = Spec("Game", pe::PanelSection::Scene);
    game.Spaces = pe::panelspace::Scene;
    game.SceneDock = {pe::PanelDock::Center, 1};
    pe::PanelSpec view = Spec("Viewport", pe::PanelSection::Scene);
    view.Spaces = pe::panelspace::Scene;
    view.SceneDock = {pe::PanelDock::Center, 0};
    pe::PanelSpec assets = Spec("Assets", pe::PanelSection::Shared);
    assets.PerSpaceWindow = true;
    assets.SceneDock = {pe::PanelDock::Bottom, 0};
    assets.InterfaceDock = {pe::PanelDock::Bottom, 0};
    reg.Register(game);   // зарегистрирован раньше, но докается вторым
    reg.Register(view);
    reg.Register(assets);

    std::vector<std::string> scene;
    reg.ForEachDocked(EditorWorkspace::Scene, [&](const std::string& id, pe::PanelDock) { scene.push_back(id); });
    CHECK_EQ((int)scene.size(), 3);
    CHECK_EQ(scene[0], std::string("Viewport"));   // первая вкладка узла — выбранная
    CHECK_EQ(scene[1], std::string("Assets"));
    CHECK_EQ(scene[2], std::string("Game"));

    std::vector<std::string> ui;
    reg.ForEachDocked(EditorWorkspace::Interface, [&](const std::string& id, pe::PanelDock) { ui.push_back(id); });
    CHECK_EQ((int)ui.size(), 1);
    CHECK_EQ(ui[0], std::string("AssetsUI"));      // своё окно в пространстве интерфейса
}

// Пределы размера панели: умолчания защищают от сжатия в полоску, боковая
// панель не раздувается на пол-экрана, явные значения и 0 («без предела»)
// уважаются, масштаб интерфейса умножает всё.
TEST(PanelRegistry_limits_have_safe_defaults_and_follow_scale) {
    pe::PanelSpec side = Spec("Side", pe::PanelSection::Scene);
    side.SceneDock.Slot = pe::PanelDock::Right;
    pe::PanelSpec bottom = Spec("Bottom", pe::PanelSection::Scene);
    bottom.SceneDock.Slot = pe::PanelDock::Bottom;

    const auto s1 = pe::PanelRegistry::LimitsFor(side, EditorWorkspace::Scene, 1.0f);
    CHECK_NEAR(s1.Min[0], pe::kDefaultMinWidth, 1e-4f);
    CHECK_NEAR(s1.Min[1], pe::kDefaultMinHeight, 1e-4f);
    CHECK_NEAR(s1.Max[0], pe::kDefaultSideMaxWidth, 1e-4f);   // боковая — с потолком ширины
    CHECK_TRUE(s1.Max[1] >= 1e30f);
    const auto b1 = pe::PanelRegistry::LimitsFor(bottom, EditorWorkspace::Scene, 1.0f);
    CHECK_TRUE(b1.Max[0] >= 1e30f && b1.Max[1] >= 1e30f);      // нижняя — без потолка

    side.Limits.MinW = 300.0f;
    side.Limits.MaxW = 0.0f;   // явно «без предела»
    const auto s2 = pe::PanelRegistry::LimitsFor(side, EditorWorkspace::Scene, 2.0f);
    CHECK_NEAR(s2.Min[0], 600.0f, 1e-4f);
    CHECK_TRUE(s2.Max[0] >= 1e30f);

    // Максимум не бывает меньше минимума: противоречивая запись не ломает док.
    side.Limits.MinW = 400.0f;
    side.Limits.MaxW = 100.0f;
    const auto s3 = pe::PanelRegistry::LimitsFor(side, EditorWorkspace::Scene, 1.0f);
    CHECK_TRUE(s3.Max[0] >= s3.Min[0]);
}

// Пользовательские макеты хранят видимость панелей по Id: снимок возвращает
// ровно то, что было, а неизвестные реестру Id применение пропускает.
TEST(PanelRegistry_visibility_snapshot_roundtrip) {
    pe::PanelRegistry reg;
    reg.Register(Spec("A", pe::PanelSection::Scene));
    reg.Register(Spec("B", pe::PanelSection::Scene));
    reg.SetVisible("A", true);
    reg.SetVisible("B", false);
    const auto snap = reg.VisibilitySnapshot();
    reg.SetVisible("A", false);
    reg.SetVisible("B", true);
    auto withGhost = snap;
    withGhost["Removed"] = true;
    reg.ApplyVisibility(withGhost);
    CHECK_TRUE(reg.IsVisible("A"));
    CHECK_FALSE(reg.IsVisible("B"));
}

// Панель не попадает в систему отдельных окон по умолчанию: её Draw мог открыть
// окно иначе (инструмент со своим Begin/End, окно плагина).
TEST(PanelRegistry_detachable_is_opt_in) {
    pe::PanelSpec s;
    CHECK(!s.Detachable);
}
