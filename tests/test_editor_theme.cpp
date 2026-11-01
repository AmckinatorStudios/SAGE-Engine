// Темы оформления редактора и окно «Настройки»: в поставке ровно три темы,
// выключенный переключатель виден в каждой из них, Premium Gold — тёплая и
// читаемая. Плюс правило «туман и небо из настроек качества» движка.
#include "TestFramework.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include <imgui.h>

#include "EditorTheme.h"
#include "panels/settings/SettingsUi.h"
#include "sage/core/Config.h"
#include "sage/render/SkyDraw.h"
#include "sage/scene/Light.h"

namespace {

struct ImGuiScope {
    ImGuiContext* Ctx = ImGui::CreateContext();
    ImGuiScope() {
        ImGui::GetIO().IniFilename = nullptr;
        EditorTheme::Init();
    }
    ~ImGuiScope() { ImGui::DestroyContext(Ctx); }
};

// Относительная яркость и контраст по WCAG — тот же счёт, каким проверяют
// читаемость интерфейсов.
double Lum(const ImVec4& c) {
    auto ch = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * ch(c.x) + 0.7152 * ch(c.y) + 0.0722 * ch(c.z);
}
double Contrast(const ImVec4& a, const ImVec4& b) {
    const double la = Lum(a), lb = Lum(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

} // namespace

TEST(editor_ships_dark_light_and_premium_gold_only) {
    ImGuiScope scope;
    std::set<std::string> ids;
    for (const EditorTheme::Theme& t : EditorTheme::Themes()) ids.insert(t.Id);
    // Внешние темы из themes/ рядом с тестом не лежат — список ровно встроенный.
    CHECK_TRUE(ids.count("modern-dark") && ids.count("modern-light") && ids.count("premium-gold"));
    CHECK_FALSE(ids.count("midnight") || ids.count("classic"));
    CHECK_EQ(ids.size(), (size_t)3);
}

TEST(settings_toggle_track_is_visible_on_every_theme) {
    ImGuiScope scope;
    namespace ui = sage::editor::settingsui;
    for (const EditorTheme::Theme& t : EditorTheme::Themes()) {
        // Карточка группы — SurfaceAlt. Дорожка выключенного переключателя
        // рисовалась цветом поля ввода и сливалась с ней (контраст ~1.0):
        // на экране оставалась одинокая белая точка.
        const ImVec4 card = t.Colors[(int)EditorTheme::Role::SurfaceAlt];
        const ImVec4 off = t.Colors[(int)ui::ToggleTrackRole(false)];
        const ImVec4 on = t.Colors[(int)ui::ToggleTrackRole(true)];
        CHECK_TRUE(Contrast(off, card) >= 1.4);
        CHECK_TRUE(Contrast(on, card) >= 1.8);
    }
}

TEST(premium_gold_is_warm_and_readable) {
    ImGuiScope scope;
    const EditorTheme::Theme* gold = nullptr;
    for (const EditorTheme::Theme& t : EditorTheme::Themes())
        if (t.Id == "premium-gold") gold = &t;
    CHECK_TRUE(gold != nullptr);
    if (!gold) return;
    using R = EditorTheme::Role;
    const ImVec4 accent = gold->Colors[(int)R::Accent];
    const ImVec4 bg = gold->Colors[(int)R::Surface];
    // Золото — красный > зелёный > синий заметно; фон тёплый (красный не ниже синего).
    CHECK_TRUE(accent.x > accent.y && accent.y > accent.z + 0.3f);
    CHECK_TRUE(bg.x >= bg.z);
    // Читаемость: текст на фоне, второстепенный текст и подпись на золоте.
    CHECK_TRUE(Contrast(gold->Colors[(int)R::Text], bg) >= 12.0);
    CHECK_TRUE(Contrast(gold->Colors[(int)R::TextDim], bg) >= 4.5);
    CHECK_TRUE(Contrast(gold->Colors[(int)R::TextOnAccent], accent) >= 4.5);
}

TEST(quality_flags_turn_off_fog_and_sky_but_keep_sky_light) {
    sage::EngineConfig cfg;
    LightingEnvironment env;
    env.Fog.Enabled = true;
    env.Skybox.Enabled = true;
    glm::vec3 skyBefore, groundBefore;
    env.ResolveAmbient(skyBefore, groundBefore);

    sage::render::ApplyQualityToEnvironment(cfg, env);   // всё включено — ничего не трогает
    CHECK_TRUE(env.Fog.Enabled && env.Skybox.Enabled);

    cfg.Fog = false;
    cfg.Skybox = false;
    sage::render::ApplyQualityToEnvironment(cfg, env);
    CHECK_FALSE(env.Fog.Enabled);
    CHECK_FALSE(env.Skybox.Enabled);
    // Небо не рисуется, но светит как прежде: «пресет полегче — мир погас»
    // было бы поломкой, а не оптимизацией.
    glm::vec3 sky, ground;
    env.ResolveAmbient(sky, ground);
    CHECK_NEAR(sky.x, skyBefore.x, 1e-4f);
    CHECK_NEAR(ground.y, groundBefore.y, 1e-4f);
}
