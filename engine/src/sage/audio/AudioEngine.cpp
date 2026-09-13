#include "AudioEngine.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/Log.h"

// Только ОБЪЯВЛЕНИЯ miniaudio — реализация (MINIAUDIO_IMPLEMENTATION) собрана
// в отдельном TU (miniaudio_impl.cpp), чтобы её ~90k строк не перекомпилировались
// каждый раз, когда меняется логика AudioEngine.
#include "miniaudio.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
constexpr float kClamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Разумные значения затухания 3D-звука по умолчанию: слышен вблизи корабля,
// плавно гаснет к горизонту. Игра может не задумываться об этих числах.
constexpr float k3DMinDistance = 1.0f;   // ближе — громкость не растёт
constexpr float k3DMaxDistance = 40.0f;  // дальше — практически не слышно
constexpr float k3DRolloff = 1.0f;
} // namespace

// Всё «сырьё» miniaudio живёт здесь, за pImpl — публичный заголовок остаётся
// чистым (только glm + std).
struct AudioEngine::Impl {
    bool Available = false;
    ma_engine Engine{};

    // Микс-группы категорий — дети конечного узла движка. Громкость категории
    // множится на громкость каждого звука в ней, а сверху — мастер-громкость
    // (ma_engine_set_volume). Так регуляторы SFX/Music/Ambient независимы.
    ma_sound_group Groups[3]{}; // индексируется (int)Category
    bool GroupsInited[3] = {false, false, false};

    float MasterVolume = 1.0f;

    // Управляемые звуки (лупы/музыка) по дескриптору — их uninit'им явно
    // (StopSound/StopMusic/деструктор).
    std::unordered_map<SoundHandle, ma_sound*> Managed;
    SoundHandle NextHandle = 1;
    SoundHandle MusicHandle = InvalidHandle;

    // Что мы остановили паузой — чтобы продолжить ровно это (см. SetAllPaused).
    std::vector<SoundHandle> PausedByUs;
    // Одноразовые звуки дескрипторов не имеют — для них хватает одного флага:
    // на паузе они все остановлены, и продолжать надо все недоигравшие.
    bool OneShotsPaused = false;

    // Одноразовые звуки: живут до конца проигрывания, реапятся в Update().
    std::vector<ma_sound*> OneShots;

    // Пути, о неудачной загрузке которых уже предупредили — чтобы звук,
    // проигрываемый каждый кадр, не залил лог одинаковыми ошибками.
    std::unordered_set<std::string> WarnedPaths;

    ma_sound_group* GroupFor(Category cat) {
        int idx = (int)cat;
        return GroupsInited[idx] ? &Groups[idx] : nullptr;
    }

    // ПОЧЕМУ НЕ ЗАИГРАЛО — одной строкой и с указанием, что именно проверять.
    //
    // Раньше здесь был код miniaudio и слово «пропускаю». По коду 2 («файл не
    // найден») человек не может отличить опечатку в пути от того, что путь
    // относительный, а редактор запущен из другой папки, — а это два разных
    // действия. Поэтому в строке стоит и то, что просили, и то, во что это
    // превратилось, и человеческая причина.
    void WarnOnce(const std::string& ref, const std::string& resolved, ma_result result) {
        if (!WarnedPaths.insert(ref).second) return;
        const char* why = "";
        switch (result) {
            case MA_DOES_NOT_EXIST: why = "файла нет по этому пути"; break;
            case MA_ACCESS_DENIED:  why = "нет доступа к файлу"; break;
            case MA_INVALID_FILE:   why = "формат не разобран (нужны wav, mp3, flac или ogg)"; break;
            case MA_OUT_OF_MEMORY:  why = "не хватило памяти"; break;
            default:                why = "звуковая подсистема отказала"; break;
        }
        LOG_ERROR("Audio") << "Звук не запущен: '" << ref << "' -> '" << resolved << "' — " << why
                           << " (miniaudio " << (int)result << ")";
    }

    // Создаёт и запускает ma_sound из файла. streaming=true для музыки
    // (не грузить в память целиком). Возвращает nullptr при ошибке.
    ma_sound* StartSound(const std::string& path, Category cat, float volume,
                         bool spatial, const glm::vec3& pos, bool looping, bool streaming) {
        ma_uint32 flags = streaming ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
        if (!spatial) flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;

        // ССЫЛКА ПРОЕКТА -> ПУТЬ, КОТОРЫЙ ОТКРОЕТСЯ. Клип у компонента хранится
        // относительно проекта («assets/audio/creak.wav»), а miniaudio открывает
        // файл относительно каталога процесса — то есть папки с exe. Играя из
        // игры, собранной рядом с проектом, разницы не видно; в редакторе,
        // запущенном из Downloads, не звучало НИЧЕГО. Ровно так же ходят за
        // своими файлами модели, текстуры и материалы.
        const std::string resolved = sage::AssetDatabase::Instance().LocatePath(path);

        auto* sound = new ma_sound{};
        ma_result r = ma_sound_init_from_file(&Engine, resolved.c_str(), flags,
                                              GroupFor(cat), nullptr, sound);
        if (r != MA_SUCCESS) {
            delete sound;
            WarnOnce(path, resolved, r);
            return nullptr;
        }
        LOG_DEBUG("Audio") << "Звук запущен: '" << path << "'"
                           << (resolved == path ? std::string() : " -> '" + resolved + "'")
                           << ", громкость " << volume << (looping ? ", зациклен" : "")
                           << (spatial ? ", позиционный" : ", непозиционный")
                           << (streaming ? ", потоком" : "");

        ma_sound_set_volume(sound, kClamp01(volume));
        ma_sound_set_looping(sound, looping ? MA_TRUE : MA_FALSE);
        if (spatial) {
            ma_sound_set_position(sound, pos.x, pos.y, pos.z);
            ma_sound_set_min_distance(sound, k3DMinDistance);
            ma_sound_set_max_distance(sound, k3DMaxDistance);
            ma_sound_set_rolloff(sound, k3DRolloff);
        }
        ma_sound_start(sound);
        return sound;
    }

    void Destroy(ma_sound* sound) {
        ma_sound_uninit(sound);
        delete sound;
    }
};

AudioEngine::AudioEngine() : m_impl(std::make_unique<Impl>()) {
    ma_result r = ma_engine_init(nullptr, &m_impl->Engine);
    if (r != MA_SUCCESS) {
        // Нет устройства (headless/CI) — переходим в немой режим, но остаёмся
        // полностью рабочим объектом: все Play* просто ничего не делают.
        LOG_WARN("Audio") << "Аудио-устройство недоступно (код " << (int)r
                          << ") — звук отключён, игра продолжит работу без него";
        return;
    }

    for (int i = 0; i < 3; ++i) {
        if (ma_sound_group_init(&m_impl->Engine, 0, nullptr, &m_impl->Groups[i]) == MA_SUCCESS) {
            m_impl->GroupsInited[i] = true;
        }
    }
    m_impl->Available = true;
    LOG_INFO("Audio") << "Аудио инициализировано (" << ma_engine_get_channels(&m_impl->Engine)
                      << " канала, " << ma_engine_get_sample_rate(&m_impl->Engine) << " Гц)";
}

AudioEngine::~AudioEngine() {
    if (!m_impl->Available) return;
    for (auto* s : m_impl->OneShots) m_impl->Destroy(s);
    for (auto& [id, s] : m_impl->Managed) m_impl->Destroy(s);
    for (int i = 0; i < 3; ++i) {
        if (m_impl->GroupsInited[i]) ma_sound_group_uninit(&m_impl->Groups[i]);
    }
    ma_engine_uninit(&m_impl->Engine);
}

bool AudioEngine::IsAvailable() const { return m_impl->Available; }

void AudioEngine::PlaySound2D(const std::string& path, float volume, Category category) {
    if (!m_impl->Available) return;
    if (ma_sound* s = m_impl->StartSound(path, category, volume, /*spatial=*/false,
                                         glm::vec3(0.0f), /*looping=*/false, /*streaming=*/false)) {
        m_impl->OneShots.push_back(s);
    }
}

void AudioEngine::PlaySound3D(const std::string& path, const glm::vec3& position,
                              float volume, Category category) {
    if (!m_impl->Available) return;
    if (ma_sound* s = m_impl->StartSound(path, category, volume, /*spatial=*/true,
                                         position, /*looping=*/false, /*streaming=*/false)) {
        m_impl->OneShots.push_back(s);
    }
}

AudioEngine::SoundHandle AudioEngine::Play(const std::string& path, const SoundParams& p) {
    if (!m_impl->Available) return InvalidHandle;
    ma_sound* s = m_impl->StartSound(path, p.Cat, p.Volume, p.Spatial, p.Position, p.Loop,
                                     /*streaming=*/false);
    if (!s) return InvalidHandle;
    // Параметры, которых у StartSound нет: он делит их с прежними вызовами, а
    // те задают расстояния константами движка. Ставим поверх — так старое
    // поведение остаётся ровно прежним, а новое получает свои значения.
    ma_sound_set_pitch(s, p.Pitch > 0.01f ? p.Pitch : 0.01f);
    if (p.Spatial) {
        ma_sound_set_min_distance(s, std::max(p.MinDistance, 0.01f));
        ma_sound_set_max_distance(s, std::max(p.MaxDistance, p.MinDistance + 0.01f));
        ma_sound_set_rolloff(s, std::max(p.Rolloff, 0.0f));
    }
    SoundHandle h = m_impl->NextHandle++;
    m_impl->Managed[h] = s;
    return h;
}

AudioEngine::SoundHandle AudioEngine::PlayLoop(const std::string& path, float volume,
                                               Category category, bool spatial,
                                               const glm::vec3& position) {
    if (!m_impl->Available) return InvalidHandle;
    ma_sound* s = m_impl->StartSound(path, category, volume, spatial, position,
                                     /*looping=*/true, /*streaming=*/false);
    if (!s) return InvalidHandle;
    SoundHandle h = m_impl->NextHandle++;
    m_impl->Managed[h] = s;
    return h;
}

void AudioEngine::PlayMusic(const std::string& path, float volume, bool loop) {
    if (!m_impl->Available) return;
    StopMusic();
    ma_sound* s = m_impl->StartSound(path, Category::Music, volume, /*spatial=*/false,
                                     glm::vec3(0.0f), loop, /*streaming=*/true);
    if (!s) return;
    SoundHandle h = m_impl->NextHandle++;
    m_impl->Managed[h] = s;
    m_impl->MusicHandle = h;
}

void AudioEngine::StopMusic() {
    if (m_impl->MusicHandle != InvalidHandle) {
        StopSound(m_impl->MusicHandle);
        m_impl->MusicHandle = InvalidHandle;
    }
}

void AudioEngine::StopSound(SoundHandle handle) {
    auto it = m_impl->Managed.find(handle);
    if (it == m_impl->Managed.end()) return;
    m_impl->Destroy(it->second);
    m_impl->Managed.erase(it);
}

void AudioEngine::SetSoundVolume(SoundHandle handle, float volume) {
    auto it = m_impl->Managed.find(handle);
    if (it != m_impl->Managed.end()) ma_sound_set_volume(it->second, kClamp01(volume));
}

void AudioEngine::SetSoundPosition(SoundHandle handle, const glm::vec3& position) {
    auto it = m_impl->Managed.find(handle);
    if (it != m_impl->Managed.end()) ma_sound_set_position(it->second, position.x, position.y, position.z);
}

void AudioEngine::SetSoundPitch(SoundHandle handle, float pitch) {
    auto it = m_impl->Managed.find(handle);
    if (it != m_impl->Managed.end()) ma_sound_set_pitch(it->second, pitch > 0.01f ? pitch : 0.01f);
}

bool AudioEngine::IsSoundAlive(SoundHandle handle) const {
    return m_impl->Managed.find(handle) != m_impl->Managed.end();
}

bool AudioEngine::IsSoundPlaying(SoundHandle handle) const {
    auto it = m_impl->Managed.find(handle);
    return it != m_impl->Managed.end() && ma_sound_is_playing(it->second) == MA_TRUE;
}

// --- Где мы внутри звука ----------------------------------------------------
//
// Нужно проигрывателю: полоса с бегунком без длительности и позиции — просто
// кнопка «играть». Спрашивается у miniaudio, а не считается по времени кадра:
// звук идёт своим потоком со своей частотой, и счётчик кадров с ним разъедется
// тем сильнее, чем дольше играет.
float AudioEngine::SoundPosition(SoundHandle handle) const {
    auto it = m_impl->Managed.find(handle);
    if (it == m_impl->Managed.end()) return 0.0f;
    float seconds = 0.0f;
    if (ma_sound_get_cursor_in_seconds(it->second, &seconds) != MA_SUCCESS) return 0.0f;
    return seconds;
}

float AudioEngine::SoundLength(SoundHandle handle) const {
    auto it = m_impl->Managed.find(handle);
    if (it == m_impl->Managed.end()) return 0.0f;
    float seconds = 0.0f;
    if (ma_sound_get_length_in_seconds(it->second, &seconds) != MA_SUCCESS) return 0.0f;
    return seconds;
}

void AudioEngine::SeekSound(SoundHandle handle, float seconds) {
    auto it = m_impl->Managed.find(handle);
    if (it == m_impl->Managed.end()) return;
    const ma_uint32 rate = ma_engine_get_sample_rate(&m_impl->Engine);
    if (rate == 0) return;
    if (seconds < 0.0f) seconds = 0.0f;
    ma_sound_seek_to_pcm_frame(it->second, (ma_uint64)(seconds * (float)rate));
}

void AudioEngine::SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) {
    if (!m_impl->Available) return;
    ma_engine_listener_set_position(&m_impl->Engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&m_impl->Engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&m_impl->Engine, 0, up.x, up.y, up.z);
}

int AudioEngine::SetAllPaused(bool paused) {
    if (!m_impl->Available) return 0;
    int touched = 0;
    if (paused) {
        m_impl->PausedByUs.clear();
        for (auto& [id, sound] : m_impl->Managed) {
            if (ma_sound_is_playing(sound) != MA_TRUE) continue;
            // ma_sound_stop, а не uninit: курсор остаётся на месте, и старт
            // продолжает с него же.
            ma_sound_stop(sound);
            m_impl->PausedByUs.push_back(id);
            ++touched;
        }
        for (ma_sound* sound : m_impl->OneShots) {
            if (ma_sound_is_playing(sound) != MA_TRUE) continue;
            ma_sound_stop(sound);
            ++touched;
        }
        m_impl->OneShotsPaused = true;
    } else {
        for (SoundHandle id : m_impl->PausedByUs) {
            auto it = m_impl->Managed.find(id);
            // Звук мог исчезнуть за время паузы (сцену остановили, объект
            // удалили) — это не ошибка, просто продолжать нечего.
            if (it == m_impl->Managed.end()) continue;
            ma_sound_start(it->second);
            ++touched;
        }
        m_impl->PausedByUs.clear();
        if (m_impl->OneShotsPaused) {
            for (ma_sound* sound : m_impl->OneShots) {
                if (ma_sound_at_end(sound) == MA_TRUE) continue;
                ma_sound_start(sound);
                ++touched;
            }
            m_impl->OneShotsPaused = false;
        }
    }
    return touched;
}

void AudioEngine::SetMasterVolume(float volume) {
    m_impl->MasterVolume = kClamp01(volume);
    if (m_impl->Available) ma_engine_set_volume(&m_impl->Engine, m_impl->MasterVolume);
}

void AudioEngine::SetCategoryVolume(Category category, float volume) {
    if (ma_sound_group* g = m_impl->GroupFor(category)) ma_sound_group_set_volume(g, kClamp01(volume));
}

float AudioEngine::MasterVolume() const { return m_impl->MasterVolume; }

void AudioEngine::Update() {
    if (!m_impl->Available) return;
    auto& pool = m_impl->OneShots;
    for (size_t i = 0; i < pool.size();) {
        if (ma_sound_at_end(pool[i]) == MA_TRUE) {
            m_impl->Destroy(pool[i]);
            pool[i] = pool.back();
            pool.pop_back();
        } else {
            ++i;
        }
    }
}

// ============================================================================
//  Декодирование в моно — для волновой формы в инструментах
// ============================================================================

namespace {

// Общая часть обоих DecodeToMono: декодер уже открыт, осталось вычитать всё и
// свести каналы. Читаем ПОРЦИЯМИ: длина файла заранее известна не всегда
// (потоковые форматы), а держать в памяти два представления — исходное и
// моно — незачем.
bool DecodeAllToMono(ma_decoder& decoder, std::vector<float>& outSamples, int& outSampleRate) {
    const ma_uint32 channels = decoder.outputChannels;
    outSampleRate = (int)decoder.outputSampleRate;
    if (channels == 0 || outSampleRate <= 0) return false;

    outSamples.clear();
    // Оценка длины, если формат её знает: одна аллокация вместо десятков.
    ma_uint64 totalFrames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames) == MA_SUCCESS && totalFrames > 0) {
        outSamples.reserve((size_t)totalFrames);
    }

    constexpr ma_uint64 kChunkFrames = 4096;
    std::vector<float> chunk((size_t)kChunkFrames * channels);
    for (;;) {
        ma_uint64 read = 0;
        if (ma_decoder_read_pcm_frames(&decoder, chunk.data(), kChunkFrames, &read) != MA_SUCCESS) break;
        if (read == 0) break;
        for (ma_uint64 f = 0; f < read; ++f) {
            float sum = 0.0f;
            for (ma_uint32 c = 0; c < channels; ++c) sum += chunk[(size_t)(f * channels + c)];
            outSamples.push_back(sum / (float)channels);
        }
        if (read < kChunkFrames) break; // файл кончился
    }
    return !outSamples.empty();
}

} // namespace

bool AudioEngine::DecodeToMono(const std::string& path, std::vector<float>& outSamples,
                               int& outSampleRate) {
    // Просим float32: волне нужна амплитуда, а не исходная разрядность, и
    // конвертацию miniaudio делает сам — надёжнее, чем разбирать форматы руками.
    // Ссылка проекта разрешается так же, как при проигрывании: инструмент,
    // который РИСУЕТ волну по файлу, и движок, который его ИГРАЕТ, обязаны
    // понимать одну и ту же строку. Иначе звук слышно, а волны для него нет —
    // и выглядит это как «формат не поддержан».
    const std::string resolved = sage::AssetDatabase::Instance().LocatePath(path);
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder;
    if (ma_decoder_init_file(resolved.c_str(), &config, &decoder) != MA_SUCCESS) {
        LOG_WARN("Audio") << "Не удалось открыть для разбора: '" << path << "' -> '" << resolved
                          << "'";
        return false;
    }
    const bool ok = DecodeAllToMono(decoder, outSamples, outSampleRate);
    ma_decoder_uninit(&decoder);
    if (!ok) LOG_WARN("Audio") << "Файл открылся, но сэмплов не дал: " << resolved;
    return ok;
}

bool AudioEngine::DecodeToMono(const void* data, size_t bytes, std::vector<float>& outSamples,
                               int& outSampleRate) {
    if (!data || bytes == 0) return false;
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder;
    if (ma_decoder_init_memory(data, bytes, &config, &decoder) != MA_SUCCESS) return false;
    const bool ok = DecodeAllToMono(decoder, outSamples, outSampleRate);
    ma_decoder_uninit(&decoder);
    return ok;
}
