#include "ScriptEngine.h"

#include "sage/core/Log.h"
#include "sage/render/ParticlePresets.h"

// ---------------------------------------------------------------------------
// Частицы, билборды и звук: sage.fx.*, sage.audio.*
//
// Часть Lua-API движка. Раньше ВСЕ привязки жили в одном ScriptEngine.cpp на
// 1800 строк: 126 функций, восемнадцать областей, и чтобы дописать одну
// строчку про анимацию, приходилось листать интерфейс, физику и таймеры.
// Определения разъехались по файлам ScriptApi_*.cpp — по файлу на область;
// объявления методов остались в ScriptEngine.h, поэтому порядок регистрации
// по-прежнему записан в одном месте (RegisterEngineApi) и не зависит от того,
// в каком файле лежит тело.
// ---------------------------------------------------------------------------

void ScriptEngine::RegisterParticleApi() {
    // --- Частицы: доступно после BindParticles. ParticleConfig — те же поля,
    // что и ParticleEmitterConfig в C++ (см. render/Particle.h), плюс готовые
    // пресеты ParticlePresets.* (готовые визуальные рецепты — всплеск/дым/осколки) —
    // Lua-скрипт может взять пресет как основу и подправить пару полей,
    // вместо того чтобы описывать весь конфиг с нуля. ---
    m_lua.new_usertype<ParticleEmitterConfig>("ParticleConfig",
        sol::constructors<ParticleEmitterConfig()>(),
        "DirectionMin", &ParticleEmitterConfig::DirectionMin,
        "DirectionMax", &ParticleEmitterConfig::DirectionMax,
        "SpeedMin", &ParticleEmitterConfig::SpeedMin,
        "SpeedMax", &ParticleEmitterConfig::SpeedMax,
        "Gravity", &ParticleEmitterConfig::Gravity,
        "LifetimeMin", &ParticleEmitterConfig::LifetimeMin,
        "LifetimeMax", &ParticleEmitterConfig::LifetimeMax,
        "StartSizeMin", &ParticleEmitterConfig::StartSizeMin,
        "StartSizeMax", &ParticleEmitterConfig::StartSizeMax,
        "EndSizeMin", &ParticleEmitterConfig::EndSizeMin,
        "EndSizeMax", &ParticleEmitterConfig::EndSizeMax,
        "StartColor", &ParticleEmitterConfig::StartColor,
        "EndColor", &ParticleEmitterConfig::EndColor,
        "AngularVelocityMax", &ParticleEmitterConfig::AngularVelocityMax,
        "EmissionRate", &ParticleEmitterConfig::EmissionRate
    );

    sol::table presets = m_lua.create_table();
    presets.set_function("WaterSplash", &ParticlePresets::WaterSplash);
    presets.set_function("Smoke", &ParticlePresets::Smoke);
    presets.set_function("BlockBreak", &ParticlePresets::BlockBreak);
    presets.set_function("StoveEmbers", &ParticlePresets::StoveEmbers);
    m_lua["ParticlePresets"] = presets;

    // Разовый залп частиц в мировой точке — см. ParticleSystem::Burst
    Bind("fx", "Emit", "EmitParticles", [this](const ParticleEmitterConfig& config, glm::vec3 pos, int count) {
        if (!m_particles) throw std::runtime_error("EmitParticles: система частиц не привязана (ScriptEngine::BindParticles не вызван)");
        m_particles->Burst(config, pos, count);
    });
    // Непрерывная струя (дым, искры) — создаётся выключенной, включай
    // SetParticleStreamActive(id, true) отдельно (см. ParticleSystem::CreateStream)
    Bind("fx", "CreateStream", "CreateParticleStream", [this](const std::string& id, const ParticleEmitterConfig& config, glm::vec3 pos) {
        if (!m_particles) throw std::runtime_error("CreateParticleStream: система частиц не привязана (ScriptEngine::BindParticles не вызван)");
        m_particles->CreateStream(id, config, pos);
    });
    Bind("fx", "SetStreamActive", "SetParticleStreamActive", [this](const std::string& id, bool active) {
        if (!m_particles) throw std::runtime_error("SetParticleStreamActive: система частиц не привязана (ScriptEngine::BindParticles не вызван)");
        m_particles->SetStreamActive(id, active);
    });
    Bind("fx", "SetStreamPosition", "SetParticleStreamPosition", [this](const std::string& id, glm::vec3 pos) {
        if (!m_particles) throw std::runtime_error("SetParticleStreamPosition: система частиц не привязана (ScriptEngine::BindParticles не вызван)");
        m_particles->SetStreamPosition(id, pos);
    });
    Bind("fx", "RemoveStream", "RemoveParticleStream", [this](const std::string& id) {
        if (!m_particles) throw std::runtime_error("RemoveParticleStream: система частиц не привязана (ScriptEngine::BindParticles не вызван)");
        m_particles->RemoveStream(id);
    });
}

void ScriptEngine::RegisterBillboardApi() {
    // --- Билборды: доступно после BindBillboards. Спрайт без texturePath
    // рисуется сплошным цветом Tint (см. BillboardSprite) — удобно для
    // маркеров/индикаторов, для которых не хочется готовить текстуру. ---
    Bind("fx", "AddBillboard", "AddBillboard", [this](glm::vec3 pos, glm::vec2 size, sol::optional<std::string> texturePath) -> int {
        if (!m_billboards) throw std::runtime_error("AddBillboard: система билбордов не привязана (ScriptEngine::BindBillboards не вызван)");
        BillboardSprite sprite;
        sprite.WorldPos = pos;
        sprite.Size = size;
        if (texturePath) sprite.SpriteTexture = GetOrLoadBillboardTexture(*texturePath);
        return m_billboards->Add(sprite);
    });
    Bind("fx", "RemoveBillboard", "RemoveBillboard", [this](int id) {
        if (!m_billboards) throw std::runtime_error("RemoveBillboard: система билбордов не привязана (ScriptEngine::BindBillboards не вызван)");
        m_billboards->Remove(id);
    });
    Bind("fx", "SetBillboardPosition", "SetBillboardPosition", [this](int id, glm::vec3 pos) {
        if (!m_billboards) throw std::runtime_error("SetBillboardPosition: система билбордов не привязана (ScriptEngine::BindBillboards не вызван)");
        m_billboards->SetPosition(id, pos);
    });
    Bind("fx", "SetBillboardVisible", "SetBillboardVisible", [this](int id, bool visible) {
        if (!m_billboards) throw std::runtime_error("SetBillboardVisible: система билбордов не привязана (ScriptEngine::BindBillboards не вызван)");
        m_billboards->SetVisible(id, visible);
    });
    Bind("fx", "SetBillboardTint", "SetBillboardTint", [this](int id, glm::vec4 tint) {
        if (!m_billboards) throw std::runtime_error("SetBillboardTint: система билбордов не привязана (ScriptEngine::BindBillboards не вызван)");
        m_billboards->SetTint(id, tint);
    });
}

void ScriptEngine::RegisterAudioApi() {
    // --- Звук: доступно после BindAudio. Скрипты (катсцены, события уровня)
    // могут проигрывать эффекты/музыку и рулить громкостью. Если аудио-
    // устройство недоступно (headless), вызовы безопасно ничего не делают. ---
    Bind("audio", "PlaySound", "PlaySound", [this](const std::string& path, sol::optional<float> volume) {
        if (!m_audio) throw std::runtime_error("PlaySound: аудио не привязано (ScriptEngine::BindAudio не вызван)");
        m_audio->PlaySound2D(path, volume.value_or(1.0f));
    });
    Bind("audio", "PlaySound3D", "PlaySound3D", [this](const std::string& path, glm::vec3 pos, sol::optional<float> volume) {
        if (!m_audio) throw std::runtime_error("PlaySound3D: аудио не привязано (ScriptEngine::BindAudio не вызван)");
        m_audio->PlaySound3D(path, pos, volume.value_or(1.0f));
    });
    Bind("audio", "PlayMusic", "PlayMusic", [this](const std::string& path, sol::optional<float> volume) {
        if (!m_audio) throw std::runtime_error("PlayMusic: аудио не привязано (ScriptEngine::BindAudio не вызван)");
        m_audio->PlayMusic(path, volume.value_or(1.0f), true);
    });
    Bind("audio", "StopMusic", "StopMusic", [this]() {
        if (!m_audio) throw std::runtime_error("StopMusic: аудио не привязано (ScriptEngine::BindAudio не вызван)");
        m_audio->StopMusic();
    });
    Bind("audio", "SetMasterVolume", "SetMasterVolume", [this](float volume) {
        if (!m_audio) throw std::runtime_error("SetMasterVolume: аудио не привязано (ScriptEngine::BindAudio не вызван)");
        m_audio->SetMasterVolume(volume);
    });
}

