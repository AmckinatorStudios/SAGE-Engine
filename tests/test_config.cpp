// Тесты EngineConfig: пресеты качества (значения и инварианты), env-пресет
// SAGE_QUALITY, сохранение/загрузка sage.cfg с пресетными значениями.
#include "TestFramework.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

#include <filesystem>
#include <fstream>

#include "sage/core/Config.h"
#include "sage/core/Paths.h"
#include "sage/core/Systems.h"
#include "sage/render/PostFX.h"
#include "sage/render/PbrShader.h"
#include <cstring>

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

#if !defined(_WIN32)
// Быстрый доступ файлового диалога («Документы», «Загрузки») собирается не из
// «дом плюс английское имя»: на локализованной системе этих папок под такими
// именами НЕТ. Имена лежат в ~/.config/user-dirs.dirs, и читать надо их.
//
// Проверяем на подставном доме: сама ошибка «взяли английское имя» тихая —
// список просто оказывается наполовину пустым, и заметить это можно только
// на системе не с английской локалью.
TEST(Paths_user_folders_come_from_xdg_not_from_english_names) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path home = fs::temp_directory_path(ec) / "sage_xdg_home";
    fs::remove_all(home, ec);
    fs::create_directories(home / ".config", ec);
    fs::create_directories(home / "Документы", ec);
    fs::create_directories(home / "Загрузки", ec);
    // «Рабочий стол» НЕ создаём: он объявлен в user-dirs.dirs, но не существует —
    // такой пункт показывать нельзя, он ведёт в пустоту.
    {
        std::ofstream f(home / ".config" / "user-dirs.dirs");
        f << "# сгенерировано xdg-user-dirs\n"
          << "XDG_DESKTOP_DIR=\"$HOME/Рабочий стол\"\n"
          << "XDG_DOCUMENTS_DIR=\"$HOME/Документы\"\n"
          << "XDG_DOWNLOAD_DIR=\"$HOME/Загрузки\"\n";
    }

    const char* savedHome = std::getenv("HOME");
    const std::string restore = savedHome ? savedHome : "";
    setenv("HOME", home.string().c_str(), 1);

    const std::vector<sage::UserFolder> folders = sage::UserFolders();
    auto pathOf = [&](const char* label) -> fs::path {
        for (const sage::UserFolder& f : folders)
            if (std::strcmp(f.Label, label) == 0) return f.Path;
        return {};
    };
    const fs::path docs = pathOf("Документы");
    const fs::path downloads = pathOf("Загрузки");
    const fs::path desktop = pathOf("Рабочий стол");
    const fs::path projects = sage::DefaultProjectsDir();

    if (restore.empty()) unsetenv("HOME");
    else setenv("HOME", restore.c_str(), 1);
    fs::remove_all(home, ec);

    CHECK_TRUE(docs == home / "Документы");
    CHECK_TRUE(downloads == home / "Загрузки");
    CHECK_TRUE(desktop.empty());                 // объявлена, но не существует
    CHECK_TRUE(pathOf("Домой") == home);
    // Новые проекты — в «Документы», а не рядом с бинарником редактора.
    CHECK_TRUE(projects == home / "Документы" / "SAGE Projects");
}
#endif

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
#include <vector>
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

// --- Сглаживание: настройка обязана доезжать до буфера сцены ----------------
//
// `msaa` в конфиге читался, сохранялся и ставился пресетом Ultra в 4 — и не
// делал НИЧЕГО: сглаживание растеризатора работает на буфере, в котором
// геометрия растеризуется, а сцена рисуется в свой offscreen-буфер, который
// заводился всегда односэмпловым. Проверяем перевод настройки в число сэмплов
// и то, что FXAA при работающем MSAA выключается: два сглаживания подряд —
// это ровно та «мыльная картинка», от которой второе и не спасает.
TEST(Config_msaa_maps_to_scene_sample_count) {
    sage::EngineConfig cfg;
    cfg.Msaa = 0;  CHECK_EQ(sage::render::SceneSamples(cfg), 1);
    cfg.Msaa = 1;  CHECK_EQ(sage::render::SceneSamples(cfg), 1);
    cfg.Msaa = 2;  CHECK_EQ(sage::render::SceneSamples(cfg), 2);
    cfg.Msaa = 4;  CHECK_EQ(sage::render::SceneSamples(cfg), 4);
    cfg.Msaa = 8;  CHECK_EQ(sage::render::SceneSamples(cfg), 8);
    // Значение приходит от человека (файл, переменная окружения) — округляем
    // вниз до ступени растеризатора, а не притворяемся, что дали запрошенное.
    cfg.Msaa = 3;  CHECK_EQ(sage::render::SceneSamples(cfg), 2);
    cfg.Msaa = 64; CHECK_EQ(sage::render::SceneSamples(cfg), 8);
}

TEST(Config_fxaa_and_msaa_do_not_stack) {
    sage::EngineConfig cfg;
    cfg.Fxaa = true;

    cfg.Msaa = 0;
    CHECK_TRUE(sage::render::FxFromConfig(cfg).FxaaEnabled);

    cfg.Msaa = 4;
    CHECK_FALSE(sage::render::FxFromConfig(cfg).FxaaEnabled);

    // Выключенный руками FXAA не должен включаться сам от отсутствия MSAA.
    cfg.Fxaa = false;
    cfg.Msaa = 0;
    CHECK_FALSE(sage::render::FxFromConfig(cfg).FxaaEnabled);
}

// Пресет Ultra обещает MSAA — и обещание должно доходить до буфера сцены.
TEST(Config_ultra_preset_really_enables_msaa) {
    sage::EngineConfig cfg;
    cfg.ApplyPreset(sage::QualityPreset::Ultra);
    CHECK_TRUE(sage::render::SceneSamples(cfg) > 1);
    CHECK_TRUE(cfg.ShadowResolution >= 2048);
}

// --- Производные в общем блоке освещения запрещены -------------------------
//
// kPbrSharedGlsl встраивается в ШЕСТЬ разных шейдеров, и ни один из них не
// контролирует, из какого места вызовется освещение. Внутри него уже стоит
// ветвление по каскадам, а сам вызов приходит из тернарника по uniform-условию,
// который компилятор вправе собрать настоящей веткой, — и производные там
// спецификация объявляет неопределёнными.
//
// Это не теория. Ровно так и случилось: length(fwidth(worldPos)) в расчёте тени
// работал на программном растеризаторе (и в эталонных кадрах CI), а на
// настоящей видеокарте вернул мусор — NaN уехал в тень, оттуда в яркость, ACES
// зажал NaN в ноль, и ВСЯ освещённая геометрия стала чёрной. Небо и сетка
// рисовались как ни в чём не бывало: они идут мимо PBR. Ни один тест этого не
// заметил, потому что проверять картинку было не на чем.
//
// Проверка текстовая и потому ловит это ДО сборки шейдера, на любой машине.
TEST(PbrShader_has_no_screen_space_derivatives) {
    const std::string glsl = sage::render::kPbrSharedGlsl;
    for (const char* fn : {"dFdx", "dFdy", "fwidth"}) {
        const size_t at = glsl.find(fn);
        if (at != std::string::npos) {
            // Упоминание в комментарии — законно (там объясняется, почему их
            // тут нет). Ищем только вызовы: имя, за которым идёт скобка.
            size_t p = at;
            bool call = false;
            while (p != std::string::npos) {
                const size_t after = p + std::strlen(fn);
                if (after < glsl.size() && glsl[after] == '(') {
                    // Строка, в которой нашли, — комментарий?
                    const size_t lineStart = glsl.rfind('\n', p);
                    const std::string line =
                        glsl.substr(lineStart == std::string::npos ? 0 : lineStart + 1,
                                    p - (lineStart == std::string::npos ? 0 : lineStart + 1));
                    if (line.find("//") == std::string::npos) { call = true; break; }
                }
                p = glsl.find(fn, p + 1);
            }
            CHECK_FALSE(call);
        }
    }
}
