# Звук

2D и 3D звук, музыка, эмбиент, компонент Audio.

## Аудио (2D/3D-звук, эмбиент, музыка)
Звуковая подсистема ядра (`engine/src/sage/audio/AudioEngine.*`) — часть
движка наравне с рендером и скриптингом. Бэкенд —
[miniaudio](https://github.com/mackron/miniaudio) (single-header, public
domain): сам выбирает системный аудио-API (ALSA/PulseAudio на Linux, WASAPI на
Windows, CoreAudio на macOS) и грузит его через `dlopen` в рантайме.

Что умеет: 2D-эффекты (`PlaySound2D`, непозиционные, для UI), 3D-эффекты
(`PlaySound3D`, микшируются относительно слушателя — обычно камера,
`SetListener` каждый кадр), зацикленный эмбиент (`PlayLoop`), потоковая музыка
(`PlayMusic`, декодируется на лету), микс-группы категорий (SFX/Music/Ambient)
с раздельной регулировкой громкости.

```cpp
AudioEngine audio;
audio.SetListener(cam.Position, cam.Front, cam.Up);      // каждый кадр
audio.PlaySound3D("assets/audio/splash.wav", worldPos);  // звук в точке мира
audio.PlayLoop("assets/audio/ocean.wav", 0.7f);          // эмбиент
audio.PlayMusic("assets/audio/music.wav", 0.5f);         // фоновая музыка
```

**Graceful degradation:** если аудио-устройство недоступно (headless-сервер,
CI, нет звуковой карты), конструктор не падает — движок переходит в «немой»
режим, и все `Play*` становятся no-op (проверка — `IsAvailable()`). Тот же
бинарник работает и со звуком, и без него. Из Lua доступны `PlaySound`/
`PlaySound3D`/`PlayMusic`/`StopMusic`/`SetMasterVolume` после
`ScriptEngine::BindAudio()`. `PlaySound(path, volume, pitch)` и
`PlaySound3D(path, pos, volume, pitch)`: высота (1 — как записано, 2 — на
октаву выше, и быстрее) — чтобы один и тот же шаг или удар не звучал
одинаково каждый раз.

**Громкость — шинами.** Шина — микс-группа с именем, которое выбирает игра
(«block», «player», «weather»…; у категорий движка — `sfx`, `music`,
`ambient`), заводится при первом обращении. Звук идёт в шину последним
необязательным параметром: `PlaySound(path, volume, pitch, bus)`,
`PlaySound3D(path, pos, volume, pitch, bus)`, `PlayMusic(path, volume, loop,
fade, bus)`; регулятор — `audio.set_bus_volume(bus, v)`, текущее —
`audio.bus_volume(bus)`. Итог звука = его громкость × шина × общая
(`SetMasterVolume`), всё 0..1: больше 1 не усиливает. Общая множится на
каждую шину, а не ставится на конечный узел графа: громкость выходной шины
конечного узла miniaudio 0.11 никто не читает, и прежде регулятор общей
громкости не действовал вовсе.

**Форматы:** WAV, MP3, FLAC и **OGG Vorbis**. Vorbis декодирует `stb_vorbis`
(подключён к miniaudio в `audio/miniaudio_impl.cpp`); файлы читаются и из
пакета игры. `ReadAudioInfo(path, info)` — длительность, каналы и частота без
проигрывания.

**Музыка плейлистом.** `PlayMusic(path, volume = 1, loop = true, fade = 0)`:
`loop = false` — трек один раз, `fade` — секунды нарастания из тишины.
`IsMusicPlaying()` говорит, что трек кончился (пора следующий),
`SetMusicVolume(v)` — громкость на ходу, `FadeOutMusic(seconds)` — плавно
увести и остановить. Порядок треков, паузы и когда играть — решает игра:
движок даёт инструменты, а не готовый плеер.

```lua
local tracks = {"assets/music/a.ogg", "assets/music/b.ogg"}
local wait = 0
function OnUpdate(entity, dt)
    if not IsMusicPlaying() then
        wait = wait - dt
        if wait <= 0 then
            PlayMusic(tracks[math.random(#tracks)], 0.6, false, 2)
            wait = 10 + math.random() * 20      -- пауза после следующего трека
        end
    end
end
```

**Декодирование без воспроизведения.** `AudioEngine::DecodeToMono` (статический,
работает и в «немом» режиме, и без единого созданного движка) читает файл или
кусок памяти и отдаёт сэмплы моно вместе с частотой дискретизации. Форматы —
все, что понимает движок: **WAV, MP3, FLAC, OGG Vorbis**. Нужно это тем, кому звук надо
не услышать, а ПОСМОТРЕТЬ: осциллограмма на монтажной дорожке, поиск пиков для
синхронизации анимации по удару, детект тишины. Director 3D рисует этим
звуковую дорожку таймлайна — до этого он разбирал заголовок WAV вручную и
спотыкался на любом другом формате.

**Перемотка в проигрывателе редактора.** Бегунок перематывал звук в каждом
кадре перетаскивания, а звук продолжал играть. Перемотка у miniaudio отложенная
(её исполняет звуковой поток), курсор в следующем кадре ещё старый — бегунок
прыгал назад, перематывал снова, и из динамиков шла каша обрывков, «будто
играет одновременно». Теперь как в любом проигрывателе
(`sage/audio/AudioScrub.h`): схватили — пауза, тянут — движется только бегунок,
отпустили — одна перемотка и продолжение. Бегунок у остановленного звука
задаёт, откуда начнётся «Играть». `SeekSound` считает кадр в частоте САМОГО
ФАЙЛА: частота устройства (48 кГц) на файле 44.1 кГц уводила перемотку на 9%
дальше. Проверка — `Audio_scrub_pauses_while_dragging_and_seeks_once`.
