#pragma once
#include <string>

// ---------------------------------------------------------------------------
// EngineConfig — единый гибкий конфиг движка: окно, дисплей и качество графики.
// Заменяет разрозненное чтение env-переменных по всему коду одним источником
// правды. Приоритет применения (от слабого к сильному):
//   значения по умолчанию  ->  файл настроек (JSON)  ->  env-переменные.
// Так у игрока есть и файл настроек (settings-меню редактора его сохраняет), и
// возможность переопределить любой параметр из окружения (для CI/отладки).
//
// Использование в точке входа игры/рантайма:
//   sage::EngineConfig cfg;
//   cfg.LoadFile("sage.cfg");     // молча пропускается, если файла нет
//   cfg.ApplyEnvOverrides();      // SAGE_* поверх файла
//   sage::EngineConfig::Set(cfg); // делает конфиг глобально видимым
//   ... затем окно строится из cfg, а слои читают EngineConfig::Get().
// ---------------------------------------------------------------------------
namespace sage {

// Режим окна.
enum class WindowMode { Windowed, Borderless, Fullscreen };

// Фиксация соотношения сторон кадра (letterbox/pillarbox чёрными полосами).
// Free — кадр занимает всё окно (соотношение = размер окна).
enum class AspectMode { Free, R16x9, R16x10, R4x3, R21x9 };

// Готовые пресеты качества графики — один клик/переменная окружения вместо
// ручной настройки десятка флагов. Меняют ТОЛЬКО дисплей/графику/пост-процесс;
// оконные параметры (размер/режим/vsync) не трогают.
//   Low    — слабые/старые ПК: без теней и пост-процесса, рендер в 75% разрешения;
//   Medium — тени 1024 + базовый пост (тон-маппинг), без Bloom/SSAO;
//   High   — всё включено, тени 2048 (значения по умолчанию движка);
//   Ultra  — тени 4096 + MSAA 4x.
enum class QualityPreset { Low, Medium, High, Ultra };

struct EngineConfig {
    // --- Окно ---
    int Width = 1280;
    int Height = 720;
    std::string Title = "SAGE Engine";
    WindowMode Mode = WindowMode::Windowed;
    bool Resizable = true;
    bool VSync = true;
    int FrameCap = 0;     // максимум кадров/с (0 — без ограничения; при VSync обычно 0)
    int Msaa = 0;         // сглаживание экранного буфера: 0/2/4/8 (GLFW_SAMPLES)

    // --- Дисплей ---
    AspectMode Aspect = AspectMode::Free;
    float RenderScale = 1.0f; // масштаб внутреннего разрешения рендера (0.25..2.0)

    // --- Качество графики (флаги отключения тяжёлых проходов) ---
    bool Shadows = true;
    int ShadowResolution = 2048; // сторона карты теней (512/1024/2048/4096)
    bool PostProcessing = true;  // HDR + тон-маппинг/эффекты
    bool Fog = true;             // линейный туман сцены
    bool Skybox = true;          // процедурный скайбокс

    // --- Параметры пост-процесса (действуют при PostProcessing=true) ---
    float Exposure = 1.0f;
    float Gamma = 2.2f;
    float Saturation = 1.0f;
    float Contrast = 1.0f;
    float Vignette = 0.35f;
    bool Bloom = true;             // свечение ярких участков
    float BloomThreshold = 1.0f;   // яркость, выше которой пиксель светится
    float BloomIntensity = 0.55f;  // сила добавляемого свечения
    bool AmbientOcclusion = true;  // SSAO — затемнение щелей/контактов
    float AOStrength = 1.0f;       // сила SSAO (0 — выкл)
    float AORadius = 0.5f;         // радиус выборки SSAO (мировые единицы)

    // --- Многопоточность ---
    // Фоновые воркеры JobSystem (отсечение/подготовка кадра). 0 — авто
    // (аппаратные потоки − 1). Игровые скрипты и выдача GL остаются на главном
    // потоке по устройству движка (см. JobSystem.h / README).
    int WorkerThreads = 0;
    bool MultithreadedRender = true; // параллельные отсечение/сбор батчей кадра

    // Соотношение сторон для Aspect (0 — Free, кадр по размеру окна).
    float AspectRatio() const;

    // Итоговое внутреннее разрешение по ширине/высоте окна с учётом RenderScale
    // (кламп 0.25..2.0). Возвращает как минимум 1x1.
    void ScaledResolution(int windowW, int windowH, int& outW, int& outH) const;

    // Viewport с фиксированным соотношением сторон по центру окна (letterbox/
    // pillarbox чёрными полосами). Aspect=Free -> весь экран (0,0,winW,winH).
    void LetterboxViewport(int winW, int winH, int& x, int& y, int& w, int& h) const;

    // Применяет пресет качества (см. QualityPreset): выставляет display/graphics/
    // post-поля разом. Оконные параметры не трогаются. Дальше поля можно
    // подстраивать вручную — пресет лишь стартовая точка, не режим.
    void ApplyPreset(QualityPreset preset);

    // --- Загрузка/сохранение/переопределения ---
    // Читает JSON-файл настроек. false, если файла нет или он битый (значения не
    // трогаются частично — при ошибке остаётся как было).
    bool LoadFile(const std::string& path);
    // Пишет JSON-файл настроек. false при ошибке записи.
    bool SaveFile(const std::string& path) const;
    // Накладывает env-переменные поверх текущих значений (см. .cpp — полный список).
    void ApplyEnvOverrides();

    // --- Глобальный текущий конфиг ---
    static EngineConfig& Get();
    static void Set(const EngineConfig& cfg);
};

} // namespace sage
