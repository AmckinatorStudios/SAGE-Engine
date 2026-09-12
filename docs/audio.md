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
`ScriptEngine::BindAudio()`.

**Декодирование без воспроизведения.** `AudioEngine::DecodeToMono` (статический,
работает и в «немом» режиме, и без единого созданного движка) читает файл или
кусок памяти и отдаёт сэмплы моно вместе с частотой дискретизации. Форматы —
все, что понимает miniaudio: **WAV, MP3, FLAC**. Нужно это тем, кому звук надо
не услышать, а ПОСМОТРЕТЬ: осциллограмма на монтажной дорожке, поиск пиков для
синхронизации анимации по удару, детект тишины. Director 3D рисует этим
звуковую дорожку таймлайна — до этого он разбирал заголовок WAV вручную и
спотыкался на любом другом формате.
