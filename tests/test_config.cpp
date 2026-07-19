// Тесты EngineConfig: пресеты качества (значения и инварианты), env-пресет
// SAGE_QUALITY, сохранение/загрузка sage.cfg с пресетными значениями.
#include "TestFramework.h"

#include <cstdio>
#include <cstdlib>

#include "sage/core/Config.h"
#include "sage/core/Systems.h"

using sage::EngineConfig;
using sage::QualityPreset;

TEST(Systems_registry_all_v1_and_valid) {
    const auto& systems = sage::EngineSystems();
    CHECK_TRUE(systems.size() >= 15); // движок состоит из многих подсистем
    for (const sage::SystemVersion& s : systems) {
        CHECK_EQ(s.Major, 1);                 // пока все на v1
        CHECK_EQ(s.Tag(), std::string("v1"));
        CHECK_TRUE(s.Name != nullptr && s.Name[0] != '\0');
        CHECK_TRUE(s.Summary != nullptr && s.Summary[0] != '\0');
    }
}

TEST(Config_preset_low_disables_heavy_passes) {
    EngineConfig c;
    c.ApplyPreset(QualityPreset::Low);
    CHECK_FALSE(c.Shadows);
    CHECK_FALSE(c.PostProcessing);
    CHECK_FALSE(c.Bloom);
    CHECK_FALSE(c.AmbientOcclusion);
    CHECK_NEAR(c.RenderScale, 0.75f, 1e-5);
    CHECK_EQ(c.Msaa, 0);
}

TEST(Config_preset_ultra_enables_everything) {
    EngineConfig c;
    c.ApplyPreset(QualityPreset::Ultra);
    CHECK_TRUE(c.Shadows);
    CHECK_EQ(c.ShadowResolution, 4096);
    CHECK_TRUE(c.PostProcessing);
    CHECK_TRUE(c.Bloom);
    CHECK_TRUE(c.AmbientOcclusion);
    CHECK_EQ(c.Msaa, 4);
}

TEST(Config_preset_does_not_touch_window) {
    EngineConfig c;
    c.Width = 1920;
    c.Height = 1080;
    c.VSync = false;
    c.Mode = sage::WindowMode::Fullscreen;
    c.ApplyPreset(QualityPreset::Low);
    // Пресет качества не смеет трогать оконные параметры.
    CHECK_EQ(c.Width, 1920);
    CHECK_EQ(c.Height, 1080);
    CHECK_FALSE(c.VSync);
    CHECK_TRUE(c.Mode == sage::WindowMode::Fullscreen);
}

TEST(Config_presets_monotonic_shadow_resolution) {
    // Инвариант ряда пресетов: разрешение теней не убывает от Low к Ultra.
    EngineConfig lo, med, hi, ul;
    lo.ApplyPreset(QualityPreset::Low);
    med.ApplyPreset(QualityPreset::Medium);
    hi.ApplyPreset(QualityPreset::High);
    ul.ApplyPreset(QualityPreset::Ultra);
    CHECK_TRUE(lo.ShadowResolution <= med.ShadowResolution);
    CHECK_TRUE(med.ShadowResolution <= hi.ShadowResolution);
    CHECK_TRUE(hi.ShadowResolution <= ul.ShadowResolution);
}

TEST(Config_env_quality_preset_applied) {
#ifndef _WIN32
    setenv("SAGE_QUALITY", "LOW", 1); // регистронезависимо
    EngineConfig c;
    c.ApplyEnvOverrides();
    CHECK_FALSE(c.Shadows);
    CHECK_FALSE(c.PostProcessing);

    // Точечная env-переменная действует ПОВЕРХ пресета.
    setenv("SAGE_SHADOWS", "1", 1);
    EngineConfig c2;
    c2.ApplyEnvOverrides();
    CHECK_TRUE(c2.Shadows);

    unsetenv("SAGE_QUALITY");
    unsetenv("SAGE_SHADOWS");
#endif
}

TEST(Config_preset_roundtrip_through_file) {
    EngineConfig c;
    c.ApplyPreset(QualityPreset::Medium);
    const char* path = "sage_test_preset.cfg";
    CHECK_TRUE(c.SaveFile(path));

    EngineConfig loaded;
    CHECK_TRUE(loaded.LoadFile(path));
    CHECK_TRUE(loaded.Shadows);
    CHECK_EQ(loaded.ShadowResolution, 1024);
    CHECK_TRUE(loaded.PostProcessing);
    CHECK_FALSE(loaded.Bloom);
    CHECK_FALSE(loaded.AmbientOcclusion);

    std::remove(path);
}
