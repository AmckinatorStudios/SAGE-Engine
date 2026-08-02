// Тесты EngineConfig: пресеты качества (значения и инварианты), env-пресет
// SAGE_QUALITY, сохранение/загрузка sage.cfg с пресетными значениями.
#include "TestFramework.h"

#include <cstdio>
#include <cstdlib>

#include <filesystem>
#include <fstream>

#include "sage/core/Config.h"
#include "sage/core/Paths.h"
#include "sage/core/Systems.h"

using sage::EngineConfig;
using sage::QualityPreset;

// Ресурсы движка ищутся рядом с БИНАРНИКОМ, а не в текущей папке. Это тот самый
// перекос, из-за которого SAGE Player нельзя было запустить откуда угодно:
// шейдеры лежали рядом с ним, а искал он их от CWD — и падал насмерть, стоило
// сменить папку (а он её меняет сам, уходя в проект).
TEST(Paths_engine_assets_are_found_next_to_the_binary_not_in_cwd) {
    namespace fs = std::filesystem;
    const fs::path exeDir = sage::ExecutableDir();
    CHECK_FALSE(exeDir.empty());
    CHECK_TRUE(fs::exists(exeDir));

    // Кладём файл рядом с бинарником и уходим в заведомо другую папку.
    const fs::path marker = exeDir / "sage_paths_probe.txt";
    { std::ofstream f(marker); f << "probe"; }
    std::error_code ec;
    const fs::path saved = fs::current_path(ec);
    const fs::path elsewhere = fs::temp_directory_path(ec);
    fs::current_path(elsewhere, ec);

    const std::string resolved = sage::EngineAssetPath("sage_paths_probe.txt");
    const bool found = fs::exists(resolved, ec);

    fs::current_path(saved, ec);
    fs::remove(marker, ec);

    CHECK_TRUE(found);
    // Именно РАЗРЕШИЛСЯ в абсолютный путь, а не «вернулся как был»: иначе тест
    // прошёл бы и на сломанной реализации, если запускать его из папки сборки.
    CHECK_TRUE(fs::path(resolved).is_absolute());

    // Чего рядом с бинарником нет — возвращается как есть, чтобы прежние
    // раскладки (ассеты в текущей папке) продолжали работать.
    CHECK_TRUE(sage::EngineAssetPath("no/such/engine/asset.bin") == "no/such/engine/asset.bin");
}

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

// --- Сохранения игры: прогресс игрока, а не сцена ------------------------------
//
// Сериализовалась только сцена — редакторный формат. Сохранять прогресс было
// нечем, и игре оставалось либо писать файлы в обход движка, либо не
// сохраняться вовсе.
#include "sage/core/SaveGame.h"

#include <cstdlib>
#include <filesystem>
#include <vector>

TEST(SaveGame_round_trips_progress_and_lists_slots) {
    // Уводим каталог сохранений во временный: тест не должен трогать реальный
    // прогресс того, кто его запускает.
    const std::string sandbox =
        (std::filesystem::temp_directory_path() / "sage_save_test").string();
    std::filesystem::remove_all(sandbox);
#ifdef _WIN32
    _putenv_s("APPDATA", sandbox.c_str());
#else
    setenv("XDG_DATA_HOME", sandbox.c_str(), 1);
#endif
    sage::save::SetGameName("TestGame");

    CHECK_FALSE(sage::save::Exists("slot1"));
    CHECK_TRUE(sage::save::Write("slot1", R"({"day":12,"hp":80})", 3));
    CHECK_TRUE(sage::save::Exists("slot1"));

    std::string payload;
    int version = 0;
    CHECK_TRUE(sage::save::Read("slot1", payload, &version));
    CHECK_EQ(version, 3);
    CHECK_TRUE(payload.find("\"day\"") != std::string::npos);

    // Слот виден в списке — по нему строится меню «Продолжить».
    std::vector<sage::save::SlotInfo> slots = sage::save::Slots();
    CHECK_EQ((int)slots.size(), 1);
    if (!slots.empty()) {
        CHECK_TRUE(slots[0].Name == "slot1");
        CHECK_EQ(slots[0].Version, 3);
        CHECK_TRUE(slots[0].SavedAtUnix > 0);
    }

    // Перезапись слота не плодит второй файл и обновляет содержимое.
    CHECK_TRUE(sage::save::Write("slot1", R"({"day":13})", 3));
    CHECK_EQ((int)sage::save::Slots().size(), 1);
    CHECK_TRUE(sage::save::Read("slot1", payload, nullptr));
    CHECK_TRUE(payload.find("13") != std::string::npos);

    CHECK_TRUE(sage::save::Delete("slot1"));
    CHECK_FALSE(sage::save::Exists("slot1"));
    std::filesystem::remove_all(sandbox);
}

// Имя слота приходит из игры и может прийти откуда угодно — из поля ввода, из
// имени персонажа, из сети. Путь наружу («../../.bashrc») означал бы запись за
// пределы папки сохранений, то есть порчу чужих файлов именем персонажа.
TEST(SaveGame_refuses_to_escape_its_directory) {
    const std::string sandbox =
        (std::filesystem::temp_directory_path() / "sage_save_escape").string();
    std::filesystem::remove_all(sandbox);
#ifdef _WIN32
    _putenv_s("APPDATA", sandbox.c_str());
#else
    setenv("XDG_DATA_HOME", sandbox.c_str(), 1);
#endif
    sage::save::SetGameName("TestGame");

    CHECK_TRUE(sage::save::Write("../../pwned", R"({"x":1})"));
    // Файл обязан лежать ВНУТРИ каталога сохранений, как бы его ни назвали.
    const std::filesystem::path dir = sage::save::Directory();
    int found = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() == ".sagesave") ++found;
    }
    CHECK_EQ(found, 1);
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(sandbox) / "pwned.sagesave"));
    std::filesystem::remove_all(sandbox);
}
