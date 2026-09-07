// ---------------------------------------------------------------------------
// Звук как компонент сцены: состояние, система, файл сцены.
//
// ЧТО ЗДЕСЬ МОЖНО ПРОВЕРИТЬ, А ЧТО НЕТ. Услышать звук тест не может — на
// машине сборки нет звукового устройства, и движок честно работает в немом
// режиме. Но «слышно ли» — не то, что ломается: ломается связь между сценой и
// звуком. Поэтому проверяется ровно она: компонент переживает запись и чтение,
// система выполняет команды и приводит состояние в порядок, остановка сцены не
// оставляет звучащих источников, а отсутствие звуковой карты не превращается
// ни в падение, ни в «источник считает, что играет».
//
// Немой режим тут не помеха, а ВТОРАЯ ПОЛОВИНА проверки: игра, собранная на
// машине со звуком, обязана одинаково работать на машине без него — и лучше
// узнать об этом здесь, чем от игрока с ноутбуком без звуковой карты.
// ---------------------------------------------------------------------------
#include "TestFramework.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "sage/audio/AudioComponents.h"
#include "sage/audio/AudioEngine.h"
#include "sage/audio/AudioSystem.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"

namespace {

// Сцена с одним звучащим объектом.
std::unique_ptr<Scene> MakeAudioScene(GameObject& out) {
    auto scene = std::make_unique<Scene>("AudioTest");
    out = scene->CreateObject("Waterfall");
    out.GetTransform().Position = {3.0f, 0.0f, -2.0f};
    AudioSourceComponent& au = scene->Registry().emplace<AudioSourceComponent>(out.Entity());
    au.Clip = "assets/audio/water.wav";
    au.Volume = 0.6f;
    au.Pitch = 0.9f;
    au.Loop = true;
    au.AutoPlay = true;
    au.Spatial = true;
    au.MinDistance = 2.0f;
    au.MaxDistance = 55.0f;
    au.Rolloff = 1.5f;
    au.Category = AudioCategory::Ambient;
    return scene;
}

TEST(Audio_component_survives_save_and_load) {
    GameObject src;
    std::unique_ptr<Scene> scene = MakeAudioScene(src);
    const std::string text = SceneSerializer::SaveToString(*scene);

    std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(text);
    CHECK_TRUE(loaded != nullptr);
    GameObject obj = loaded->FindByName("Waterfall");
    CHECK_TRUE(obj.Valid());
    const AudioSourceComponent* au =
        loaded->Registry().try_get<AudioSourceComponent>(obj.Entity());
    CHECK_TRUE(au != nullptr);
    CHECK_EQ(au->Clip, std::string("assets/audio/water.wav"));
    CHECK_NEAR(au->Volume, 0.6f, 1e-4f);
    CHECK_NEAR(au->Pitch, 0.9f, 1e-4f);
    CHECK_TRUE(au->Loop);
    CHECK_TRUE(au->AutoPlay);
    CHECK_TRUE(au->Spatial);
    CHECK_NEAR(au->MinDistance, 2.0f, 1e-4f);
    CHECK_NEAR(au->MaxDistance, 55.0f, 1e-4f);
    CHECK_NEAR(au->Rolloff, 1.5f, 1e-4f);
    CHECK_TRUE(au->Category == AudioCategory::Ambient);
}

// Рантайм-состояние в файл не попадает: сохранённое «сейчас звучит» означало бы
// сцену, которая открывается с играющим звуком, которого никто не запускал.
TEST(Audio_runtime_state_is_not_serialized) {
    GameObject src;
    std::unique_ptr<Scene> scene = MakeAudioScene(src);
    AudioSourceComponent& au = scene->Registry().get<AudioSourceComponent>(src.Entity());
    au.Handle = 42;
    au.Playing = true;
    au.Request = AudioRequest::Play;

    std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(SceneSerializer::SaveToString(*scene));
    CHECK_TRUE(loaded != nullptr);
    GameObject obj = loaded->FindByName("Waterfall");
    const AudioSourceComponent& back = loaded->Registry().get<AudioSourceComponent>(obj.Entity());
    CHECK_EQ((int)back.Handle, 0);
    CHECK_TRUE(!back.Playing);
    CHECK_TRUE(back.Request == AudioRequest::None);
}

// Система обязана работать на машине БЕЗ звуковой карты: не падать, не считать
// звук играющим и не копить дескрипторы. Это ровно тот случай, что у игрока с
// отключённым звуком, и до появления компонента его проверить было негде.
TEST(Audio_system_survives_mute_device) {
    GameObject src;
    std::unique_ptr<Scene> scene = MakeAudioScene(src);
    AudioEngine engine;

    const int started = sage::audio::StartScene(*scene, engine);
    const AudioSourceComponent& au = scene->Registry().get<AudioSourceComponent>(src.Entity());
    if (!engine.IsAvailable()) {
        // Немой режим: запусков ноль, источник честно молчит.
        CHECK_EQ(started, 0);
        CHECK_TRUE(!au.Playing);
        CHECK_EQ((int)au.Handle, 0);
    }

    // Кадры обслуживания не должны ничего ломать ни в том, ни в другом случае.
    for (int i = 0; i < 8; ++i) sage::audio::Update(*scene, engine);
    sage::audio::StopScene(*scene, engine);
    CHECK_TRUE(!au.Playing);
    CHECK_EQ((int)au.Handle, 0);
}

// Команда — это состояние компонента, а выполняет её система. Проверяем сам
// протокол: команда снимается ровно одним кадром и не выполняется дважды.
TEST(Audio_request_is_consumed_by_one_frame) {
    GameObject src;
    std::unique_ptr<Scene> scene = MakeAudioScene(src);
    AudioEngine engine;
    AudioSourceComponent& au = scene->Registry().get<AudioSourceComponent>(src.Entity());

    au.Play();
    CHECK_TRUE(au.Request == AudioRequest::Play);
    sage::audio::Update(*scene, engine);
    CHECK_TRUE(au.Request == AudioRequest::None);

    au.Stop();
    sage::audio::Update(*scene, engine);
    CHECK_TRUE(au.Request == AudioRequest::None);
    CHECK_TRUE(!au.Playing);
    CHECK_EQ((int)au.Handle, 0);
}

// Старт сцены запускает только те источники, у которых стоит «играть сразу».
// Иначе реплика по событию (скрип двери) зазвучала бы при загрузке уровня —
// сразу у всех дверей карты.
TEST(Audio_autoplay_flag_decides_what_starts) {
    auto scene = std::make_unique<Scene>("AudioAutoplay");
    GameObject ambient = scene->CreateObject("Ambient");
    AudioSourceComponent& a = scene->Registry().emplace<AudioSourceComponent>(ambient.Entity());
    a.Clip = "assets/audio/hum.wav";
    a.AutoPlay = true;

    GameObject onEvent = scene->CreateObject("DoorCreak");
    AudioSourceComponent& b = scene->Registry().emplace<AudioSourceComponent>(onEvent.Entity());
    b.Clip = "assets/audio/creak.wav";
    b.AutoPlay = false;

    AudioEngine engine;
    sage::audio::StartScene(*scene, engine);
    // Без устройства ни один не «играет», но КОМАНДА второму не ставилась —
    // проверяем именно решение системы, а не звук.
    CHECK_TRUE(b.Request == AudioRequest::None);
    CHECK_TRUE(!b.Playing);
}

// Источник без файла — не ошибка и не повод шуметь в лог: компонент только что
// добавили мышью, файл выберут следующим действием.
TEST(Audio_empty_clip_is_silent_and_harmless) {
    auto scene = std::make_unique<Scene>("AudioEmpty");
    GameObject obj = scene->CreateObject("Silent");
    AudioSourceComponent& au = scene->Registry().emplace<AudioSourceComponent>(obj.Entity());
    au.AutoPlay = true;
    CHECK_TRUE(au.Clip.empty());

    AudioEngine engine;
    CHECK_EQ(sage::audio::StartScene(*scene, engine), 0);
    au.Play();
    sage::audio::Update(*scene, engine);
    CHECK_TRUE(!au.Playing);
    CHECK_EQ((int)au.Handle, 0);
}


// --- Настоящий звук, если устройство есть ----------------------------------
//
// Все проверки выше говорят про состояние компонента, и этого мало: они прошли
// бы и на движке, который звук вообще не запускает. Здесь на диск кладётся
// НАСТОЯЩИЙ короткий WAV, и проверяется, что источник сцены действительно
// зазвучал, поехал за объектом и замолчал по команде.
//
// Если звукового устройства нет (сервер сборки без звуковой карты), проверка
// честно пропускается: требовать звук там, где его физически некому издать, —
// значит получить тест, который «иногда падает» и которому перестают верить.

// Секунда тишины в 8-битном моно: файл маленький, а формат — самый простой из
// тех, что понимает декодер, поэтому проверка не зависит от кодеков.
bool WriteTestWav(const std::filesystem::path& file) {
    const int rate = 8000;
    const int samples = rate / 4; // четверть секунды
    std::vector<unsigned char> data((size_t)samples, 128);
    // Тихая синусоида: полностью пустой файл некоторые декодеры считают
    // ошибкой, а нам нужен нормальный звук.
    for (int i = 0; i < samples; ++i)
        data[(size_t)i] = (unsigned char)(128 + (int)(20.0 * std::sin(i * 0.05)));

    std::ofstream out(file, std::ios::binary);
    if (!out) return false;
    auto u32 = [&out](unsigned int v) { out.write((const char*)&v, 4); };
    auto u16 = [&out](unsigned short v) { out.write((const char*)&v, 2); };
    const unsigned int dataBytes = (unsigned int)data.size();
    out.write("RIFF", 4); u32(36 + dataBytes); out.write("WAVE", 4);
    out.write("fmt ", 4); u32(16); u16(1); u16(1);
    u32((unsigned int)rate); u32((unsigned int)rate); u16(1); u16(8);
    out.write("data", 4); u32(dataBytes);
    out.write((const char*)data.data(), (std::streamsize)dataBytes);
    return (bool)out;
}

TEST(Audio_source_really_plays_and_stops) {
    AudioEngine engine;
    if (!engine.IsAvailable()) {
        std::printf("       звукового устройства нет — проверка пропущена\n");
        return;
    }

    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "sage_audio_test";
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path wav = dir / "beep.wav";
    CHECK_TRUE(WriteTestWav(wav));

    auto scene = std::make_unique<Scene>("AudioReal");
    GameObject obj = scene->CreateObject("Beeper");
    AudioSourceComponent& au = scene->Registry().emplace<AudioSourceComponent>(obj.Entity());
    au.Clip = wav.string();
    au.Loop = true;      // зациклен, чтобы не доиграл посреди проверки
    au.AutoPlay = true;
    au.Spatial = true;

    // 1. Старт сцены зажигает источник.
    CHECK_EQ(sage::audio::StartScene(*scene, engine), 1);
    CHECK_TRUE(au.Playing);
    CHECK_TRUE(au.Handle != 0);

    // 2. Кадры обслуживания его не гасят и не плодят новых дескрипторов.
    const std::uint32_t handle = au.Handle;
    for (int i = 0; i < 5; ++i) {
        obj.GetTransform().Position.x += 1.0f; // источник едет — система тянет звук за ним
        CHECK_EQ(sage::audio::Update(*scene, engine), 1);
    }
    CHECK_EQ((int)au.Handle, (int)handle);

    // 3. Команда «стоп» глушит его в том же кадре и отпускает дескриптор.
    au.Stop();
    sage::audio::Update(*scene, engine);
    CHECK_TRUE(!au.Playing);
    CHECK_EQ((int)au.Handle, 0);

    // 4. И запускается снова — по команде, без AutoPlay.
    au.Play();
    sage::audio::Update(*scene, engine);
    CHECK_TRUE(au.Playing);

    sage::audio::StopScene(*scene, engine);
    CHECK_TRUE(!au.Playing);
    std::filesystem::remove_all(dir, ec);
}

} // namespace
