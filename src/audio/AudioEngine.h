#pragma once
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <cstdint>

// ---------------------------------------------------------------------
// AudioEngine — звуковая подсистема движка: проигрывание 2D/3D-эффектов,
// зацикленного эмбиента, потоковой музыки, с позиционным (3D) микшированием
// относительно слушателя (обычно привязан к камере). Часть ЯДРА движка —
// как и рендер/скриптинг, ничего не знает о конкретной игре.
//
// Бэкенд — miniaudio (single-header, public domain), выбирает системный
// аудио-API сам (ALSA/PulseAudio на Linux, WASAPI на Windows, CoreAudio на
// macOS) и грузит его через dlopen в рантайме — поэтому на этапе линковки
// никаких аудио-библиотек не нужно.
//
// ВАЖНО — graceful degradation: если аудио-устройство недоступно (headless
// сервер, CI, отсутствие звуковой карты), конструктор НЕ бросает и НЕ падает —
// движок переходит в «немой» режим, и все методы Play* становятся no-op.
// Проверить можно через IsAvailable(). Это позволяет тем же коду/бинарнику
// работать и на машине разработчика со звуком, и в headless-тестах без него.
//
// miniaudio.h намеренно НЕ включён в этот заголовок (он ~90k строк) —
// реализация спрятана за pImpl, чтобы не раздувать время компиляции всем,
// кто просто хочет проиграть звук.
//
// Использование:
//   AudioEngine audio;
//   audio.SetListener(cam.Position, cam.Front, cam.Up); // каждый кадр
//   audio.PlaySound2D("assets/audio/click.wav");        // UI-щелчок
//   audio.PlaySound3D("assets/audio/splash.wav", pos);  // всплеск в точке мира
//   SoundHandle sea = audio.PlayLoop("assets/audio/ocean.wav", 0.5f);
//   audio.PlayMusic("assets/audio/calm.wav", 0.4f);     // фоновая музыка
//   ...
//   audio.Update(); // раз в кадр — освобождает доигравшие one-shot'ы
// ---------------------------------------------------------------------
class AudioEngine {
public:
    // Категории для раздельной регулировки громкости (микс-группы). Мастер-
    // громкость (SetMasterVolume) применяется поверх всех категорий.
    enum class Category { Sfx, Music, Ambient };

    // Дескриптор управляемого звука (луп/музыка/позиционный луп), который
    // вызывающий хочет продолжать контролировать после старта — остановить,
    // сдвинуть в пространстве, изменить громкость. 0 — невалидный/немой.
    using SoundHandle = uint32_t;
    static constexpr SoundHandle InvalidHandle = 0;

    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // false, если аудио-устройство не удалось открыть (headless/CI/нет карты) —
    // все Play* безопасно ничего не делают.
    bool IsAvailable() const;

    // --- Одноразовые эффекты (fire-and-forget) ---
    // 2D — непозиционный, всегда по центру (UI, оповещения). volume 0..1
    // относительно громкости категории.
    void PlaySound2D(const std::string& path, float volume = 1.0f, Category category = Category::Sfx);
    // 3D — позиционный, микшируется относительно слушателя (звук в точке мира).
    void PlaySound3D(const std::string& path, const glm::vec3& position,
                     float volume = 1.0f, Category category = Category::Sfx);

    // --- Управляемые долгоживущие звуки ---
    // Зацикленный звук (эмбиент моря, гул). spatial=false — непозиционный фон;
    // spatial=true — привязан к позиции в мире. Возвращает дескриптор для
    // последующего управления (или InvalidHandle, если аудио недоступно/ошибка).
    SoundHandle PlayLoop(const std::string& path, float volume = 1.0f,
                         Category category = Category::Ambient,
                         bool spatial = false, const glm::vec3& position = glm::vec3(0.0f));

    // Потоковая музыка (декодируется на лету, не грузится в память целиком).
    // Заменяет текущую музыкальную дорожку, если та играет.
    void PlayMusic(const std::string& path, float volume = 1.0f, bool loop = true);
    void StopMusic();

    // --- Управление по дескриптору (безопасно для InvalidHandle/чужих id) ---
    void StopSound(SoundHandle handle);
    void SetSoundVolume(SoundHandle handle, float volume);
    void SetSoundPosition(SoundHandle handle, const glm::vec3& position);
    bool IsSoundPlaying(SoundHandle handle) const;

    // Позиция и ориентация слушателя (обычно камера). Вызывать раз в кадр
    // ДО Play* этого кадра, чтобы 3D-панорама была актуальной.
    void SetListener(const glm::vec3& position, const glm::vec3& forward,
                     const glm::vec3& up = glm::vec3(0.0f, 1.0f, 0.0f));

    // Громкости, 0..1 (значения зажимаются в этот диапазон).
    void SetMasterVolume(float volume);
    void SetCategoryVolume(Category category, float volume);
    float MasterVolume() const;

    // Раз в кадр: освобождает доигравшие одноразовые 3D/2D-звуки. Без вызова
    // они не текут (реапятся и в деструкторе), но копятся до конца партии.
    void Update();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl; // nullptr никогда — но при недоступном
                                  // устройстве Impl помечен как «немой»
};
