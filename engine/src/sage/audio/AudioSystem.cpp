#include "sage/audio/AudioSystem.h"

#include <glm/glm.hpp>

#include "sage/audio/AudioComponents.h"
#include "sage/audio/AudioEngine.h"
#include "sage/core/Log.h"
#include "sage/scene/Scene.h"
#include "sage/scene/Transform.h"

namespace sage::audio {
namespace {

AudioEngine::Category ToEngineCategory(AudioCategory c) {
    switch (c) {
        case AudioCategory::Music:   return AudioEngine::Category::Music;
        case AudioCategory::Ambient: return AudioEngine::Category::Ambient;
        default:                     return AudioEngine::Category::Sfx;
    }
}

// Останавливает то, что звучит, и приводит компонент в состояние «молчит».
void Silence(AudioSourceComponent& src, AudioEngine& engine) {
    if (src.Handle != 0) engine.StopSound((AudioEngine::SoundHandle)src.Handle);
    src.Handle = 0;
    src.Playing = false;
}

// Запускает источник заново. Позиция берётся МИРОВАЯ: источник может висеть на
// ребёнке (колесо машины, факел в руке), и локальные координаты поставили бы
// звук в начало мира.
void Start(AudioSourceComponent& src, AudioEngine& engine, const glm::vec3& worldPos) {
    Silence(src, engine);
    if (src.Clip.empty()) return;

    AudioEngine::SoundParams p;
    p.Volume = src.Volume;
    p.Pitch = src.Pitch;
    p.Loop = src.Loop;
    p.Spatial = src.Spatial;
    p.Position = worldPos;
    p.MinDistance = src.MinDistance;
    p.MaxDistance = src.MaxDistance;
    p.Rolloff = src.Rolloff;
    p.Cat = ToEngineCategory(src.Category);

    src.Handle = (std::uint32_t)engine.Play(src.Clip, p);
    // Ноль означает и «нет звуковой карты», и «файл не открылся». Про второе
    // движок уже написал в лог один раз на путь — повторять здесь на каждый
    // кадр нельзя: источник с опечаткой в имени залил бы консоль.
    src.Playing = src.Handle != 0;
}

} // namespace

int StartScene(Scene& scene, AudioEngine& engine) {
    int started = 0;
    auto view = scene.Registry().view<AudioSourceComponent>();
    for (auto e : view) {
        AudioSourceComponent& src = view.get<AudioSourceComponent>(e);
        src.Request = AudioRequest::None; // старт сцены отменяет команды прошлой жизни
        if (!src.AutoPlay || src.Clip.empty()) continue;
        if (src.Playing) continue;
        Start(src, engine, glm::vec3(scene.WorldMatrix(e)[3]));
        if (src.Playing) ++started;
    }
    return started;
}

int Update(Scene& scene, AudioEngine& engine) {
    int playing = 0;
    auto view = scene.Registry().view<AudioSourceComponent>();
    for (auto e : view) {
        AudioSourceComponent& src = view.get<AudioSourceComponent>(e);

        // 1. Команды. Обрабатываются ДО всего остального: «стоп» в этом кадре
        //    должен замолчать в этом же кадре, а не после переноса позиции.
        const AudioRequest request = src.Request;
        src.Request = AudioRequest::None;
        if (request == AudioRequest::Stop) {
            Silence(src, engine);
            continue;
        }
        if (request == AudioRequest::Play) {
            Start(src, engine, glm::vec3(scene.WorldMatrix(e)[3]));
        }

        if (src.Handle == 0) continue;

        // 2. Доиграл ли одноразовый звук. Дескриптор при этом ещё жив — его
        //    освобождает владелец, то есть мы: иначе они копились бы по одному
        //    на каждый выстрел до конца партии.
        if (!engine.IsSoundPlaying((AudioEngine::SoundHandle)src.Handle)) {
            Silence(src, engine);
            continue;
        }

        // 3. Звук едет за объектом. Только позиционный: у непозиционного
        //    координаты ни на что не влияют, и трогать их — лишняя работа на
        //    каждый кадр и каждый источник.
        if (src.Spatial) {
            engine.SetSoundPosition((AudioEngine::SoundHandle)src.Handle,
                                    glm::vec3(scene.WorldMatrix(e)[3]));
        }
        // 4. Громкость и высота правятся в инспекторе на живом звуке — иначе
        //    подбирать их пришлось бы перезапуском.
        engine.SetSoundVolume((AudioEngine::SoundHandle)src.Handle, src.Volume);
        engine.SetSoundPitch((AudioEngine::SoundHandle)src.Handle, src.Pitch);
        src.Playing = true;
        ++playing;
    }
    return playing;
}

void StopScene(Scene& scene, AudioEngine& engine) {
    auto view = scene.Registry().view<AudioSourceComponent>();
    for (auto e : view) {
        AudioSourceComponent& src = view.get<AudioSourceComponent>(e);
        Silence(src, engine);
        src.Request = AudioRequest::None;
    }
}

} // namespace sage::audio
