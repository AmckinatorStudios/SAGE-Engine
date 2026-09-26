#include "ScriptEngine.h"
#include "sage/audio/AudioComponents.h"

#include "sage/core/Log.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/render/ParticleEffectIO.h"

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

namespace {

// Перечисления эффекта — СЛОВАМИ, теми же, что в файле .sagefx: скрипт и файл
// говорят на одном языке, и номер из середины списка ни у кого не сломается.
template <typename E, size_t N>
sol::property_wrapper<std::function<std::string(const sage::fx::ParticleEffect&)>,
                      std::function<void(sage::fx::ParticleEffect&, const std::string&)>>
EnumProperty(E sage::fx::ParticleEffect::*field, const char* const (&names)[N]) {
    std::function<std::string(const sage::fx::ParticleEffect&)> get =
        [field, &names](const sage::fx::ParticleEffect& f) {
            const size_t i = (size_t)(f.*field);
            return std::string(i < N ? names[i] : names[0]);
        };
    std::function<void(sage::fx::ParticleEffect&, const std::string&)> set =
        [field, &names](sage::fx::ParticleEffect& f, const std::string& v) {
            for (size_t i = 0; i < N; ++i)
                if (v == names[i]) { f.*field = (E)i; return; }
            throw std::runtime_error("ParticleEffect: неизвестное значение «" + v + "»");
        };
    return sol::property(get, set);
}

const char* const kLuaShapes[] = {"point", "sphere", "hemisphere", "cone", "box", "circle", "edge"};
const char* const kLuaRenders[] = {"billboard", "stretched", "horizontal", "vertical", "mesh", "trail"};
const char* const kLuaBlends[] = {"alpha", "additive", "premultiplied"};
const char* const kLuaSpaces[] = {"world", "local"};
const char* const kLuaSprites[] = {"softCircle", "circle", "square"};
const char* const kLuaFlipbooks[] = {"overLifetime", "speed", "random", "fixed"};

// Кривая из таблицы {{t, v}, {t, v}, ...}.
sage::fx::Curve CurveFrom(const sol::table& t) {
    sage::fx::Curve c;
    for (auto& kv : t) {
        sol::table k = kv.second.as<sol::table>();
        c.Keys.push_back({k.get_or(1, 0.0f), k.get_or(2, 1.0f)});
    }
    c.Sort();
    return c;
}

// Градиент из таблицы {{t, r, g, b, a}, ...}: у каждого ключа и цвет, и альфа.
sage::fx::Gradient GradientFrom(const sol::table& t) {
    sage::fx::Gradient g;
    for (auto& kv : t) {
        sol::table k = kv.second.as<sol::table>();
        const float at = k.get_or(1, 0.0f);
        g.Colors.push_back({at, {k.get_or(2, 1.0f), k.get_or(3, 1.0f), k.get_or(4, 1.0f)}});
        g.Alphas.push_back({at, k.get_or(5, 1.0f)});
    }
    g.Sort();
    return g;
}

} // namespace

void ScriptEngine::RegisterParticleApi() {
    using sage::fx::ParticleEffect;
    // --- Частицы: доступно после BindParticles. ---------------------------
    //
    // ГОТОВЫХ ЭФФЕКТОВ В ДВИЖКЕ НЕТ. Эффект — данные: файл .sagefx из проекта
    // (fx.LoadEffect) или таблица полей, собранная скриптом. Прежние пресеты
    // «огонь», «дым», «брызги» были кодом движка и знали только то, что в них
    // зашили.
    m_lua.new_usertype<sage::fx::Range>("ParticleRange",
        sol::factories([] { return sage::fx::Range{}; },
                       [](float lo, float hi) { return sage::fx::Range{lo, hi}; }),
        "Min", &sage::fx::Range::Min,
        "Max", &sage::fx::Range::Max);

    m_lua.new_usertype<ParticleEffect>("ParticleEffect",
        sol::constructors<ParticleEffect()>(),
        "Duration", &ParticleEffect::Duration,
        "Loop", &ParticleEffect::Loop,
        "StartDelay", &ParticleEffect::StartDelay,
        "Prewarm", &ParticleEffect::Prewarm,
        "MaxParticles", &ParticleEffect::MaxParticles,
        "SimulationSpeed", &ParticleEffect::SimulationSpeed,
        "GravityScale", &ParticleEffect::GravityScale,
        "InheritVelocity", &ParticleEffect::InheritVelocity,
        "StartLifetime", &ParticleEffect::StartLifetime,
        "StartSpeed", &ParticleEffect::StartSpeed,
        "StartSize", &ParticleEffect::StartSize,
        "StartRotation", &ParticleEffect::StartRotation,
        "StartColorA", &ParticleEffect::StartColorA,
        "StartColorB", &ParticleEffect::StartColorB,
        "RateOverTime", &ParticleEffect::RateOverTime,
        "RateOverDistance", &ParticleEffect::RateOverDistance,
        "Radius", &ParticleEffect::Radius,
        "ConeAngle", &ParticleEffect::ConeAngle,
        "Arc", &ParticleEffect::Arc,
        "BoxSize", &ParticleEffect::BoxSize,
        "EdgeLength", &ParticleEffect::EdgeLength,
        "FromShell", &ParticleEffect::FromShell,
        "RandomizeDirection", &ParticleEffect::RandomizeDirection,
        "ShapeOffset", &ParticleEffect::ShapeOffset,
        "ShapeRotation", &ParticleEffect::ShapeRotation,
        "UseVelocity", &ParticleEffect::UseVelocity,
        "LinearVelocity", &ParticleEffect::LinearVelocity,
        "Orbital", &ParticleEffect::Orbital,
        "Radial", &ParticleEffect::Radial,
        "UseForces", &ParticleEffect::UseForces,
        "Force", &ParticleEffect::Force,
        "Wind", &ParticleEffect::Wind,
        "WindInfluence", &ParticleEffect::WindInfluence,
        "Gustiness", &ParticleEffect::Gustiness,
        "Drag", &ParticleEffect::Drag,
        "MaxSpeed", &ParticleEffect::MaxSpeed,
        "Attraction", &ParticleEffect::Attraction,
        "UseNoise", &ParticleEffect::UseNoise,
        "NoiseStrength", &ParticleEffect::NoiseStrength,
        "NoiseFrequency", &ParticleEffect::NoiseFrequency,
        "NoiseScroll", &ParticleEffect::NoiseScroll,
        "UseCollision", &ParticleEffect::UseCollision,
        "PlaneHeight", &ParticleEffect::PlaneHeight,
        "Bounce", &ParticleEffect::Bounce,
        "Friction", &ParticleEffect::Friction,
        "LifetimeLoss", &ParticleEffect::LifetimeLoss,
        "Texture", &ParticleEffect::Texture,
        "TilesX", &ParticleEffect::TilesX,
        "TilesY", &ParticleEffect::TilesY,
        "FrameRate", &ParticleEffect::FrameRate,
        "PixelArt", &ParticleEffect::PixelArt,
        "Intensity", &ParticleEffect::Intensity,
        "SortByDistance", &ParticleEffect::SortByDistance,
        "StretchLength", &ParticleEffect::StretchLength,
        "StretchBySpeed", &ParticleEffect::StretchBySpeed,
        "TrailLifetime", &ParticleEffect::TrailLifetime,
        "AlignToVelocity", &ParticleEffect::AlignToVelocity,
        "Shape", EnumProperty(&ParticleEffect::Shape, kLuaShapes),
        "Render", EnumProperty(&ParticleEffect::Render, kLuaRenders),
        "Blend", EnumProperty(&ParticleEffect::Blend, kLuaBlends),
        "Space", EnumProperty(&ParticleEffect::Space, kLuaSpaces),
        "Sprite", EnumProperty(&ParticleEffect::Sprite, kLuaSprites),
        "Flipbook", EnumProperty(&ParticleEffect::Flipbook, kLuaFlipbooks),
        // Кривые и градиенты — таблицами; вызов заодно включает свой модуль:
        // задать кривую и забыть галку — самая частая «почему не работает».
        "SetSizeOverLifetime", [](ParticleEffect& f, sol::table keys) {
            f.SizeOverLifetime = CurveFrom(keys);
            f.UseSizeOverLifetime = true;
        },
        "SetColorOverLifetime", [](ParticleEffect& f, sol::table keys) {
            f.ColorOverLifetime = GradientFrom(keys);
            f.UseColorOverLifetime = true;
        },
        "SetSpeedOverLifetime", [](ParticleEffect& f, sol::table keys) {
            f.SpeedOverLifetime = CurveFrom(keys);
            f.UseVelocity = true;
        },
        "SetFrames", [](ParticleEffect& f, sol::table frames) {
            f.Frames.clear();
            for (auto& kv : frames) f.Frames.push_back(kv.second.as<std::string>());
        },
        "AddBurst", [](ParticleEffect& f, float time, int count, sol::optional<int> cycles,
                       sol::optional<float> interval) {
            f.Bursts.push_back({time, count, count, cycles.value_or(1), interval.value_or(0.5f)});
        });

    // Эффект из файла проекта (.sagefx). Ошибка — исключение с причиной, а не
    // пустой эффект: «не нашёлся файл» и «эффект такой» различать обязательно.
    Bind("fx", "LoadEffect", "LoadParticleEffect", [](const std::string& path) {
        ParticleEffect fx;
        std::string err;
        const std::string real = sage::AssetDatabase::Instance().LocatePath(path);
        if (!sage::fx::LoadEffectFile(real, fx, &err))
            throw std::runtime_error("LoadEffect: " + err);
        return fx;
    });

    // Разовый залп частиц в мировой точке.
    Bind("fx", "Emit", "EmitParticles", [this](const ParticleEffect& fx, glm::vec3 pos, int count) {
        if (!m_particles) throw std::runtime_error("EmitParticles: система частиц не привязана (ScriptEngine::BindParticles не вызван)");
        m_particles->Burst(fx, pos, count);
    });
    // Непрерывная струя по имени — создаётся выключенной, включай SetStreamActive.
    Bind("fx", "CreateStream", "CreateParticleStream", [this](const std::string& id, const ParticleEffect& fx, glm::vec3 pos) {
        if (!m_particles) throw std::runtime_error("CreateParticleStream: система частиц не привязана (ScriptEngine::BindParticles не вызван)");
        m_particles->CreateStream(id, fx, pos);
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

    // --- Эмиттер ОБЪЕКТА --------------------------------------------------
    // Команда ставится компоненту, система исполнит её в своём шаге кадра —
    // так же, как у звука объекта.
    auto emitter = [](GameObject& obj, const char* who) -> ParticleEmitterComponent& {
        if (!obj.Valid()) throw std::runtime_error(std::string(who) + ": объект недействителен");
        ParticleEmitterComponent* pe = obj.Registry()->try_get<ParticleEmitterComponent>(obj.Entity());
        if (!pe) throw std::runtime_error(std::string(who) + ": у объекта нет эмиттера частиц");
        return *pe;
    };
    Bind("fx", "Play", "PlayParticles", [emitter](GameObject obj) {
        ParticleEmitterComponent& pe = emitter(obj, "fx.Play");
        pe.Playing = true;
        pe.RestartRequested = true;
    });
    Bind("fx", "Stop", "StopParticles", [emitter](GameObject obj, sol::optional<bool> clear) {
        ParticleEmitterComponent& pe = emitter(obj, "fx.Stop");
        pe.Playing = false;
        if (clear.value_or(false)) pe.ClearRequested = true;
    });
    Bind("fx", "EmitFrom", "EmitFromObject", [emitter](GameObject obj, int count) {
        emitter(obj, "fx.EmitFrom").PendingEmit += std::max(count, 0);
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

    // --- Звук ОБЪЕКТА (компонент Audio) ------------------------------------
    //
    // Отличие от PlaySound3D не в удобстве: там скрипт обязан знать и путь к
    // файлу, и координаты, и повторить их в каждом месте, откуда звук
    // запускается. Здесь всё это уже стоит в сцене мышью, а скрипт говорит
    // только КОГДА: `Audio.Play(door)`. Звук при этом едет за объектом сам —
    // ровно поэтому у скрипта нет способа «забыть обновить позицию».
    //
    // Команда ставится компоненту, а к устройству идёт система кадра: у
    // скрипта нет доступа к звуковому устройству, и заводить его ради одной
    // кнопки значило бы дать каждому скрипту право глушить чужие звуки.
    Bind("audio", "Play", "Play", [](GameObject obj) {
        if (!obj.Valid()) throw std::runtime_error("Audio.Play: объект недействителен");
        AudioSourceComponent* au = obj.Registry()->try_get<AudioSourceComponent>(obj.Entity());
        if (!au) throw std::runtime_error("Audio.Play: у объекта нет компонента Audio");
        au->Play();
    });
    Bind("audio", "Stop", "Stop", [](GameObject obj) {
        if (!obj.Valid()) throw std::runtime_error("Audio.Stop: объект недействителен");
        AudioSourceComponent* au = obj.Registry()->try_get<AudioSourceComponent>(obj.Entity());
        if (!au) throw std::runtime_error("Audio.Stop: у объекта нет компонента Audio");
        au->Stop();
    });
    Bind("audio", "IsPlaying", "IsPlaying", [](GameObject obj) {
        if (!obj.Valid()) return false;
        const AudioSourceComponent* au =
            obj.Registry()->try_get<AudioSourceComponent>(obj.Entity());
        return au && au->Playing;
    });
    // Громкость и высота — на живом звуке: система переносит их на звучащий
    // экземпляр в том же кадре, поэтому «затихающий вдали мотор» пишется одной
    // строкой в OnUpdate, а не перезапуском звука.
    Bind("audio", "SetVolume", "SetVolume", [](GameObject obj, float volume) {
        if (!obj.Valid()) return;
        if (AudioSourceComponent* au = obj.Registry()->try_get<AudioSourceComponent>(obj.Entity()))
            au->Volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
    });
    Bind("audio", "SetPitch", "SetPitch", [](GameObject obj, float pitch) {
        if (!obj.Valid()) return;
        if (AudioSourceComponent* au = obj.Registry()->try_get<AudioSourceComponent>(obj.Entity()))
            au->Pitch = pitch < 0.05f ? 0.05f : pitch;
    });
    // Сменить звук на лету: у двери «открыть» и «закрыть» — один источник и
    // два файла, и заводить ради этого два компонента незачем.
    Bind("audio", "SetClip", "SetClip", [](GameObject obj, const std::string& clip) {
        if (!obj.Valid()) return;
        if (AudioSourceComponent* au = obj.Registry()->try_get<AudioSourceComponent>(obj.Entity()))
            au->Clip = clip;
    });
}

