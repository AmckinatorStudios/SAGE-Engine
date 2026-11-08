#pragma once
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>
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
// Сведения о звуковом файле (WAV, MP3, FLAC, OGG Vorbis) без проигрывания:
// длительность для инспектора и проверка «этот файл движок вообще откроет».
// Путь — как у клипа (относительно проекта, из пакета игры тоже).
struct AudioFileInfo {
    unsigned Channels = 0;
    unsigned SampleRate = 0;
    unsigned long long Frames = 0;
    double Seconds() const { return SampleRate ? (double)Frames / SampleRate : 0.0; }
};
bool ReadAudioInfo(const std::string& path, AudioFileInfo& out);

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
    // pitch — множитель частоты: 0.5 — октавой ниже (и вдвое дольше).
    // bus — именованная шина громкости (см. SetBusVolume); пусто — шина
    // категории ("sfx", "music", "ambient").
    void PlaySound2D(const std::string& path, float volume = 1.0f, Category category = Category::Sfx,
                     float pitch = 1.0f, const std::string& bus = {});
    // 3D — позиционный, микшируется относительно слушателя (звук в точке мира).
    void PlaySound3D(const std::string& path, const glm::vec3& position,
                     float volume = 1.0f, Category category = Category::Sfx, float pitch = 1.0f,
                     const std::string& bus = {});
    // Высоты звучащих сейчас разовых эффектов — для отладки («почему шаги
    // звучат одинаково») и проверок. Пусто — звука нет или он уже доиграл.
    std::vector<float> OneShotPitches() const;

    // --- Звук с полным набором параметров -----------------------------------
    //
    // ЗАЧЕМ ЕЩЁ ОДИН СПОСОБ ЗАПУСТИТЬ ЗВУК. PlaySound2D/3D и PlayLoop покрывают
    // случаи «выстрелил и забыл» и «фон играет всегда», и для игры, пишущей код
    // руками, этого хватало. Компоненту сцены — нет: он обязан уметь и то, и
    // другое, и обязан ПРОДОЛЖАТЬ УПРАВЛЯТЬ звуком после запуска (переставить
    // источник вместе с объектом, приглушить, остановить по команде скрипта), а
    // одноразовые звуки дескриптора не отдавали вовсе.
    //
    // Плюс параметры, которых у прежних вызовов не было и которые задаёт
    // именно автор сцены: высота тона (один и тот же удар звучит иначе у
    // деревянного ящика и у железного) и расстояния затухания (шёпот слышно за
    // метр, сирену — за сотню). Раньше расстояния были константами движка,
    // одинаковыми для всех звуков сразу.
    struct SoundParams {
        float Volume = 1.0f;
        // Множитель частоты: 1 — как в файле, 0.5 — октавой ниже. Меняет и
        // высоту, и длительность — как у настоящей записи, пущенной медленнее.
        float Pitch = 1.0f;
        bool Loop = false;
        // Позиционный ли звук. false — звучит одинаково отовсюду (музыка,
        // интерфейс, мысли героя).
        bool Spatial = false;
        glm::vec3 Position{0.0f};
        // Ближе MinDistance громкость не растёт, дальше MaxDistance — не
        // слышно. Rolloff — крутизна спада между ними.
        float MinDistance = 1.0f;
        float MaxDistance = 40.0f;
        float Rolloff = 1.0f;
        Category Cat = Category::Sfx;
    };

    // Запускает звук и ОТДАЁТ ДЕСКРИПТОР — в том числе одноразовый. Дескриптор
    // остаётся действительным, пока владелец не позовёт StopSound: доигравший
    // звук не исчезает сам, иначе владелец не смог бы отличить «доиграл» от
    // «был остановлен кем-то ещё» (см. IsSoundPlaying).
    SoundHandle Play(const std::string& path, const SoundParams& params);

    // --- Управляемые долгоживущие звуки ---
    // Зацикленный звук (эмбиент моря, гул). spatial=false — непозиционный фон;
    // spatial=true — привязан к позиции в мире. Возвращает дескриптор для
    // последующего управления (или InvalidHandle, если аудио недоступно/ошибка).
    SoundHandle PlayLoop(const std::string& path, float volume = 1.0f,
                         Category category = Category::Ambient,
                         bool spatial = false, const glm::vec3& position = glm::vec3(0.0f));

    // Потоковая музыка (декодируется на лету, не грузится в память целиком).
    // Заменяет текущую музыкальную дорожку, если та играет.
    // Декодирует звуковой файл в моно-сэмплы -1..1. Это НЕ проигрывание:
    // нужно инструментам, которые рисуют волну и режут по ней анимацию.
    //
    // Форматы берёт те же, что и проигрывание (miniaudio: WAV, MP3, FLAC), —
    // и это принципиально. Раньше инструмент нёс собственный разбор WAV, и
    // получалось, что MP3 играет, но волна для него не строится: файл как бы
    // поддержан и как бы нет. Один декодер на оба случая такого расхождения
    // не допускает.
    //
    // Каналы сводятся в моно усреднением: волна показывает громкость во
    // времени, и раздельные каналы в ней ничего не добавляют, а память едят
    // вдвое. outSampleRate — частота исходника (нужна, чтобы перевести
    // сэмплы в секунды). false — файл не открылся или формат не разобран.
    static bool DecodeToMono(const std::string& path, std::vector<float>& outSamples,
                             int& outSampleRate);

    // То же для файла, уже прочитанного в память (импорт из архива, буфер обмена).
    static bool DecodeToMono(const void* data, size_t bytes, std::vector<float>& outSamples,
                             int& outSampleRate);

    // Музыка — один поток за раз. fadeSeconds > 0 — нарастает из тишины.
    // loop = false — трек играет один раз: так устроены плейлисты с паузами
    // между треками (IsMusicPlaying скажет, когда пора следующий).
    void PlayMusic(const std::string& path, float volume = 1.0f, bool loop = true, float fadeSeconds = 0.0f,
                   const std::string& bus = {});
    void StopMusic();
    bool IsMusicPlaying() const;
    void SetMusicVolume(float volume);
    // Плавно увести и остановить (переход из меню в игру, смена сцены).
    void FadeOutMusic(float seconds);

    // --- Управление по дескриптору (безопасно для InvalidHandle/чужих id) ---
    void StopSound(SoundHandle handle);
    void SetSoundVolume(SoundHandle handle, float volume);
    void SetSoundPosition(SoundHandle handle, const glm::vec3& position);
    void SetSoundPitch(SoundHandle handle, float pitch);
    bool IsSoundPlaying(SoundHandle handle) const;
    // Живёт ли дескриптор вообще (звук ещё не остановлен владельцем). Отличать
    // это от IsSoundPlaying обязательно: доигравший одноразовый звук — живой
    // дескриптор, который больше не играет, и владельцу надо его освободить.
    bool IsSoundAlive(SoundHandle handle) const;

    // --- Где мы внутри звука (для проигрывателя и инструментов) --------------
    // Секунды от начала и полная длительность; 0 — дескриптор чужой или мёртвый.
    // Спрашиваются у звуковой подсистемы, а не считаются по времени кадра: звук
    // идёт своим потоком, и счётчик кадров с ним разъедется.
    float SoundPosition(SoundHandle handle) const;
    float SoundLength(SoundHandle handle) const;
    // Перемотка. Секунды зажимаются снизу нулём; за концом файла звук просто
    // доигрывает.
    void SeekSound(SoundHandle handle, float seconds);

    // Позиция и ориентация слушателя (обычно камера). Вызывать раз в кадр
    // ДО Play* этого кадра, чтобы 3D-панорама была актуальной.
    void SetListener(const glm::vec3& position, const glm::vec3& forward,
                     const glm::vec3& up = glm::vec3(0.0f, 1.0f, 0.0f));

    // Громкости, 0..1 (значения зажимаются в этот диапазон).
    // ПАУЗА ВСЕГО ЗВУКА, без потери позиции воспроизведения.
    //
    // Нужна паузе игры: остановленный кадр, из которого продолжает литься шум
    // водопада, паузой не выглядит. Глушить громкостью нельзя — это затёрло бы
    // настройку человека; остановка же запоминает курсор, и продолжение идёт с
    // того места, где встали.
    //
    // Запоминается ИМЕННО ТО, что звучало на момент паузы: продолжение не
    // оживляет доигравшие и заранее остановленные звуки. Возвращает, сколько
    // источников затронуто.
    // Пауза ОДНОГО управляемого звука (луп, музыка, источник сцены). Отдельно
    // от StopSound, потому что «продолжить с того же места» и «начать заново» —
    // разные вещи, и источник звука в сцене умеет и то, и другое.
    void SetSoundPaused(SoundHandle handle, bool paused);

    int SetAllPaused(bool paused);

    // ГРОМКОСТЬ — шинами. Шина — микс-группа со своим именем («block»,
    // «player», «weather» — какие нужны игре; у категорий — "sfx", "music",
    // "ambient"), создаётся при первом обращении. Итог звука = его громкость ×
    // громкость шины × общая (все — 0..1, больше 1 не усиливает).
    //
    // Общая громкость множится на КАЖДУЮ шину, а не ставится на конечный узел
    // графа: громкость выходной шины конечного узла miniaudio 0.11 никто не
    // читает (ma_engine_set_volume на ней не действовал), и регулятор общей
    // громкости в игре ничего не менял.
    void SetMasterVolume(float volume);
    void SetCategoryVolume(Category category, float volume);
    float MasterVolume() const;
    void SetBusVolume(const std::string& bus, float volume);
    float BusVolume(const std::string& bus) const;
    // То, что реально стоит на шине: громкость шины × общая.
    float EffectiveBusVolume(const std::string& bus) const;
    static const char* CategoryBus(Category category);

    // Раз в кадр: освобождает доигравшие одноразовые 3D/2D-звуки. Без вызова
    // они не текут (реапятся и в деструкторе), но копятся до конца партии.
    void Update();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl; // nullptr никогда — но при недоступном
                                  // устройстве Impl помечен как «немой»
};
