// ============================================================================
// The Boat (альфа) на SAGE Engine.
//
// Концепт: бесконечный спокойный океан. Игрок — на небольшом корабле,
// который "плывёт вперёд" (корабль в мире статичен, океан и мусор движутся
// мимо — так проще и стабильнее для воксельной структуры). Корабль целиком
// построен из игровых блоков: его можно ломать, перестраивать и достраивать.
// Из воды вылавливается мусор, из мусора крафтятся блоки, удочка и пресная
// вода. Рыбу можно поймать, пожарить на печке и съесть. У игрока четыре
// стата: здоровье, энергия, голод, жажда. Время суток сменяется, ночью
// разгораются корабельные фонари. Игра про атмосферу и расслабленность.
//
// Управление:
//   WASD — ходьба, Shift — бег, Space — прыжок (в воде — грести вверх)
//   Мышь — взгляд; ЛКМ — сломать блок / подсечь рыбу
//   ПКМ — использовать предмет в руке (поставить блок / закинуть удочку /
//         пожарить рыбу на печке / съесть / выпить)
//   1..9, колесо — хотбар; F — подобрать мусор; Tab — меню крафта
//   V — noclip (режим полёта для стройки), F2 — скриншот, F3 — debug HUD
//   F4 — панель настроек (интерактивный UI: тумблеры/кнопки мышью)
//
// Устройство файла (после рефакторинга): main() отвечает только за
// инициализацию ресурсов и цикл "ввод -> симуляция -> рендер". Вся игровая
// логика живёт в src/game/:
//   GameState.h     — состояние партии (мир, игрок, инвентарь, системы)
//   PlayerActions.h — реакция на ввод (ломать/ставить/крафтить/есть)
//   GameHud.h        — отрисовка игрового интерфейса
// ============================================================================
#include <cstdlib>
#include <cstdio>
#include <string>
#include <iostream>
#include <algorithm>
#include <vector>
#include <future>
#include <thread>
#include <chrono>
#include <cstdint>
#include <filesystem>

#include "core/Version.h"
#include "core/Window.h"
#include "core/Log.h"
#include "core/Stats.h"
#include "core/InputSystem.h"
#include "core/JobSystem.h"
#include "core/MainThreadDispatcher.h"
#include "core/EngineRuntime.h"
#include "render/Shader.h"
#include "render/Camera.h"
#include "asset/AssetManager.h"
#include "render/Texture.h"
#include "render/Skybox.h"
#include "render/BillboardSystem.h"
#include "render/LightingUpload.h"
#include "render/Screenshot.h"
#include "render/DebugOverlay.h"
#include "render/Framebuffer.h"
#include "render/ShadowMap.h"
#include "render/PostProcess.h"
#include "ui/UIRenderer.h"
#include "ui/UICanvas.h"
#include "ui/Widgets.h"
#include "game/GameActions.h"
#include "game/GameState.h"
#include "game/PlayerActions.h"
#include "game/GameHud.h"

namespace {

constexpr float kNoclipFlySpeed = 10.0f; // м/с, свободный полёт в режиме noclip (см. UpdateNoclipFly)

// Полусторона ортобокса карты теней вокруг центра корабля — накрывает весь
// корабль плюс воду вокруг него с запасом (см. ShadowMap::SetLightMatrix).
constexpr float kShadowRadius = 24.0f;

// Читает позицию/угол камеры и время суток из переменных окружения —
// используется автотестами/CI для детерминированных скриншотов без
// реального ввода (см. SAGE_CAM_POS/YAW/PITCH, SAGE_TIME_OF_DAY, SAGE_NOCLIP).
void ApplyDebugEnvOverrides(GameState& game, Camera& camera) {
    if (const char* posEnv = std::getenv("SAGE_CAM_POS")) {
        float x, y, z;
        if (std::sscanf(posEnv, "%f,%f,%f", &x, &y, &z) == 3) game.Player.Position = {x, y, z};
    }
    if (const char* yawEnv = std::getenv("SAGE_CAM_YAW")) camera.Yaw = (float)std::atof(yawEnv);
    if (const char* pitchEnv = std::getenv("SAGE_CAM_PITCH")) camera.Pitch = (float)std::atof(pitchEnv);
    if (const char* t = std::getenv("SAGE_TIME_OF_DAY")) game.DayNight.SetTimeOfDay((float)std::atof(t));
    if (std::getenv("SAGE_NOCLIP")) game.Noclip = true;
    if (std::getenv("SAGE_FORCE_COOKING")) game.CookTimer = 999.0f; // для CI-скриншотов эффекта готовки/дыма
    if (std::getenv("SAGE_DEBUG_HUD")) game.DebugHudVisible = true;
    if (std::getenv("SAGE_FORCE_BITE")) {
        game.Fishing.Cast(camera.Position, camera.Front, GameConstants::SeaLevel);
        game.Fishing.DebugForceBite();
    }
    camera.ProcessMouse(0.0f, 0.0f); // пересчитать Front/Right/Up после ручной правки Yaw/Pitch
}

// Собирает худ (статбары + часы) из виджетов UI-системы движка. Виджеты
// сами тянут актуальные значения через ValueSource/TextSource каждый кадр —
// после сборки этот канвас не нужно больше трогать вручную.
UICanvas BuildStatsHud(GameState& game) {
    UICanvas hud;
    struct BarDef { const char* label; float PlayerStats::* field; glm::vec3 color; };
    // Порядок принципиален: здоровье, энергия, голод, жажда
    BarDef defs[] = {
        {"HP", &PlayerStats::Health, {0.85f, 0.25f, 0.25f}},
        {"EN", &PlayerStats::Energy, {0.95f, 0.85f, 0.30f}},
        {"FD", &PlayerStats::Hunger, {0.90f, 0.55f, 0.20f}},
        {"WT", &PlayerStats::Thirst, {0.30f, 0.60f, 0.95f}},
    };
    for (int i = 0; i < 4; ++i) {
        auto* bar = hud.Add<UIProgressBar>();
        bar->Anchor = UIAnchor::TopLeft;
        bar->Offset = {16.0f, 16.0f + i * 24.0f};
        bar->Size = {180.0f, 16.0f};
        bar->Label = defs[i].label;
        bar->FillColor = defs[i].color;
        float PlayerStats::* field = defs[i].field;
        bar->ValueSource = [&game, field] { return game.Stats.*field / 100.0f; };
    }

    auto* clockPanel = hud.Add<UIPanel>();
    clockPanel->Anchor = UIAnchor::TopRight;
    clockPanel->Offset = {12.0f, 12.0f};
    clockPanel->Size = {172.0f, 26.0f};
    clockPanel->Color = {0.0f, 0.0f, 0.0f};
    clockPanel->Alpha = 0.5f;

    auto* clockLabel = hud.Add<UILabel>();
    clockLabel->Anchor = UIAnchor::TopRight;
    clockLabel->Offset = {20.0f, 18.0f};
    clockLabel->Scale = 2.0f;
    clockLabel->TextSource = [&game] {
        return "Day " + std::to_string(game.DayCounter) + "  " + game.DayNight.ClockString();
    };
    return hud;
}

// Читает действия движения и превращает их в желаемое направление в
// горизонтальной плоскости (относительно текущего взгляда камеры)
glm::vec3 ReadMoveDirection(InputMap& actions, const Camera& camera) {
    glm::vec3 flatFront = glm::normalize(glm::vec3(camera.Front.x, 0.0f, camera.Front.z));
    glm::vec3 flatRight = glm::normalize(glm::vec3(camera.Right.x, 0.0f, camera.Right.z));
    glm::vec3 dir(0.0f);
    if (actions.IsDown(GameActions::MoveForward)) dir += flatFront;
    if (actions.IsDown(GameActions::MoveBackward)) dir -= flatFront;
    if (actions.IsDown(GameActions::MoveLeft)) dir -= flatRight;
    if (actions.IsDown(GameActions::MoveRight)) dir += flatRight;
    return dir;
}

// Свободный полёт (ToggleNoclip) для стройки/отладки — минует физику и статы целиком
void UpdateNoclipFly(GameState& game, InputMap& actions, const Camera& camera, float dt) {
    glm::vec3 fly(0.0f);
    if (actions.IsDown(GameActions::MoveForward)) fly += camera.Front;
    if (actions.IsDown(GameActions::MoveBackward)) fly -= camera.Front;
    if (actions.IsDown(GameActions::MoveLeft)) fly -= camera.Right;
    if (actions.IsDown(GameActions::MoveRight)) fly += camera.Right;
    if (actions.IsDown(GameActions::FlyUp)) fly += glm::vec3(0, 1, 0);
    if (actions.IsDown(GameActions::FlyDown)) fly -= glm::vec3(0, 1, 0);
    if (glm::length(fly) > 0.001f) {
        game.Player.Position += glm::normalize(fly) * kNoclipFlySpeed * dt;
    }
    game.Player.Velocity = glm::vec3(0.0f);
}

// Самопроверка асинхронной подсистемы (JobSystem + MainThreadDispatcher +
// асинхронная загрузка ресурсов) — гоняется по SAGE_TEST_ASYNC, в т.ч. в CI
// headless. Требует уже созданного GL-контекста (загрузка текстуры/меша идёт
// в главном потоке). Возвращает true, если всё прошло. НЕ входит в обычный
// игровой цикл — это отдельный детерминированный прогон с последующим выходом.
bool RunAsyncSelfTest() {
    using clock = std::chrono::steady_clock;
    bool ok = true;

    // 1) Пул задач: 64 независимые задачи, собираем результаты через future.
    std::vector<std::future<long>> futures;
    for (int i = 0; i < 64; ++i) {
        futures.push_back(JobSystem::Instance().Enqueue([i]() -> long {
            long s = 0; for (int k = 0; k < 5000; ++k) s += (i * k) % 13; return s;
        }));
    }
    long sum = 0; for (auto& f : futures) sum += f.get();
    LOG_INFO("AsyncTest") << "Пул: 64 задачи выполнены (" << JobSystem::Instance().WorkerCount()
                          << " воркеров), sum=" << sum;

    // 2) ParallelFor: параллельно заполняем массив, проверяем корректность.
    std::vector<int> arr(20000, -1);
    JobSystem::Instance().ParallelFor(arr.size(), [&arr](size_t i) { arr[i] = int(i % 1000); });
    for (size_t i = 0; i < arr.size(); ++i) {
        if (arr[i] != int(i % 1000)) { ok = false; break; }
    }
    LOG_INFO("AsyncTest") << "ParallelFor: " << (ok ? "OK" : "ОШИБКА");

    // 3) Система ассетов: async-загрузка текстуры и модели (CPU-декод в фоне,
    //    GL-загрузка в главном потоке через Drain). Крутим Drain до готовности.
    AssetManager& am = AssetManager::Instance();
    Asset<Texture> tex = am.LoadTextureAsync("textures/checker_demo.png");
    Asset<Mesh> model = am.LoadModelAsync("models/sphere.obj");

    auto start = clock::now();
    while (tex.IsLoading() || model.IsLoading()) {
        MainThreadDispatcher::Instance().Drain();
        if (std::chrono::duration<double>(clock::now() - start).count() > 5.0) break; // страховка
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool texOk = tex.IsReady() && tex.Get() && tex->Width() > 0;
    bool modelOk = model.IsReady() && model.Get();
    LOG_INFO("AsyncTest") << "Async-текстура: " << (texOk ? "OK" : "ОШИБКА")
                          << ", async-модель: " << (modelOk ? "OK" : "ОШИБКА");

    // 4) Кэш/дедуп: повторный запрос отдаёт ТОТ ЖЕ ресурс (один читок с диска).
    Asset<Texture> texAgain = am.LoadTexture("textures/checker_demo.png");
    Asset<Mesh> modelAgain = am.LoadModelAsync("models/sphere.obj");
    bool dedupOk = texAgain.Get() == tex.Get() && modelAgain.Get() == model.Get();
    LOG_INFO("AsyncTest") << "Кэш/дедуп: " << (dedupOk ? "OK" : "ОШИБКА");

    // 5) Разрешение путей: "textures/x" и "assets/textures/x" → один ассет.
    Asset<Texture> texFull = am.LoadTexture("assets/textures/checker_demo.png");
    bool resolveOk = texFull.Get() == tex.Get();
    LOG_INFO("AsyncTest") << "Разрешение пути (root): " << (resolveOk ? "OK" : "ОШИБКА");

    // 6) Hot-reload (перезагрузка НА МЕСТЕ): форсим ReloadAll (детерминированно,
    //    без зависимости от mtime), ждём — Version должен вырасти, а тот же
    //    ресурс остаться валиден и Ready (модель перезагружена в те же буферы).
    Mesh* meshBefore = model.Get();
    uint64_t verBefore = model.Version();
    am.ReloadAll();
    start = clock::now();
    while (model.Version() == verBefore) {
        MainThreadDispatcher::Instance().Drain();
        if (std::chrono::duration<double>(clock::now() - start).count() > 5.0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Ресурс тот же объект (обновлён на месте), но версия выросла и он готов.
    bool reloadOk = model.Version() > verBefore && model.IsReady() && model.Get() == meshBefore;
    LOG_INFO("AsyncTest") << "Hot-reload (in-place): " << (reloadOk ? "OK" : "ОШИБКА")
                          << " (version " << verBefore << " -> " << model.Version() << ")";

    // Заодно проверяем автоматическое слежение за файлом (может не работать на
    // некоторых оверлей-ФС — тогда просто информируем, не валим тест).
    am.EnableHotReload(true);
    std::error_code ec;
    std::filesystem::last_write_time(am.Resolve("models/sphere.obj"),
                                     std::filesystem::file_time_type::clock::now(), ec);
    size_t watched = am.PollHotReload();
    am.EnableHotReload(false);
    LOG_INFO("AsyncTest") << "File-watch (PollHotReload): "
                          << (watched > 0 ? "изменение замечено" : "изменение не замечено (ожидаемо на overlay-ФС)");

    // 7) Статистика + сборка мусора: дропаем локальные хендлы дубликатов, GC
    //    удаляет только записи без живых ссылок; текстура/модель ещё держатся.
    AssetManager::Stats st = am.GetStats();
    LOG_INFO("AsyncTest") << "Реестр: " << st.Total << " ассетов, " << st.Ready << " готовы, "
                          << (st.ApproxBytes / 1024) << " КБ";
    Asset<Texture> temp = am.LoadTexture("textures/icon_alert.png");
    void* tempPtr = temp.Get();
    temp = Asset<Texture>();          // отпускаем единственный внешний хендл
    size_t freed = am.CollectGarbage();
    bool gcOk = freed >= 1 && tex.IsReady() && model.IsReady(); // «живые» не тронуты
    LOG_INFO("AsyncTest") << "GC: выгружено " << freed << " (ожидали >=1), живые целы: "
                          << (gcOk ? "OK" : "ОШИБКА");
    (void)tempPtr;

    ok = ok && texOk && modelOk && dedupOk && resolveOk && reloadOk && gcOk;
    LOG_INFO("AsyncTest") << (ok ? "=== ВСЕ ПРОВЕРКИ ПРОЙДЕНЫ ===" : "=== ЕСТЬ ПРОВАЛЫ ===");
    return ok;
}

// Самопроверка UI-системы (интерактив + масштаб + рендер) — по SAGE_TEST_UI.
// Клик мышью эмулируется синтетическим UIInputState, так что интерактив (кнопки,
// переключатели) проверяется без реального курсора, в т.ч. headless в CI.
bool RunUISelfTest() {
    bool ok = true;
    const float W = 1280.0f, H = 720.0f;

    UICanvas canvas;
    canvas.ReferenceSize = {640.0f, 360.0f}; // на 1280x720 масштаб должен быть 2.0

    int clicks = 0;
    auto* btn = canvas.Add<UIButton>();
    btn->Anchor = UIAnchor::Center; btn->Offset = {0.0f, 0.0f}; btn->Size = {200.0f, 50.0f};
    btn->Label = "OK"; btn->OnClick = [&clicks] { ++clicks; };

    bool toggleVal = false;
    auto* tog = canvas.Add<UIToggle>();
    tog->Anchor = UIAnchor::Center; tog->Offset = {0.0f, 90.0f}; tog->Size = {60.0f, 28.0f};
    tog->OnChanged = [&toggleVal](bool v) { toggleVal = v; };

    // Прогоняем «пустой» ввод, чтобы канвас выставил LayoutScale элементам.
    UIInputState idle; idle.Mouse = {-1.0f, -1.0f};
    canvas.Update(idle, W, H);
    bool scaleOk = std::abs(btn->LayoutScale - 2.0f) < 0.001f;
    LOG_INFO("UITest") << "Масштаб под соотношение сторон: " << (scaleOk ? "OK" : "ОШИБКА")
                       << " (scale=" << btn->LayoutScale << ", ожидали 2.0)";

    auto centerOf = [W, H](UIElement* e) {
        return e->ResolvePosition(W, H) + e->ScaledSize() * 0.5f;
    };
    auto clickAt = [&canvas, W, H](glm::vec2 pos) {
        UIInputState down; down.Mouse = pos; down.MouseDown = true; down.MousePressed = true;
        canvas.Update(down, W, H);
        UIInputState up; up.Mouse = pos; up.MouseReleased = true;
        canvas.Update(up, W, H);
    };

    clickAt(centerOf(btn));
    bool clickOk = clicks == 1;
    LOG_INFO("UITest") << "Клик по кнопке: " << (clickOk ? "OK" : "ОШИБКА") << " (clicks=" << clicks << ")";

    clickAt(centerOf(tog));
    bool toggleOk = toggleVal && tog->CurrentValue();
    LOG_INFO("UITest") << "Переключатель: " << (toggleOk ? "OK" : "ОШИБКА");

    // Клик мимо кнопки не должен её срабатывать.
    clickAt({4.0f, 4.0f});
    bool missOk = clicks == 1;
    LOG_INFO("UITest") << "Клик мимо (без ложных срабатываний): " << (missOk ? "OK" : "ОШИБКА");

    // Рендер-проход со спрайтом (текстура) + все виджеты — проверяем, что путь
    // отрисовки (в т.ч. текстурные спрайты) отрабатывает без GL-ошибок.
    Asset<Texture> checker = AssetManager::Instance().LoadTexture("textures/checker_demo.png");
    auto* spr = canvas.Add<UISprite>();
    spr->Anchor = UIAnchor::Center; spr->Offset = {0.0f, -90.0f}; spr->Size = {80.0f, 48.0f};
    spr->KeepAspect = true; spr->SpriteTexture = checker.Get();
    auto* panel = canvas.Add<UIPanel>();
    panel->Anchor = UIAnchor::Center; panel->Size = {260.0f, 240.0f}; panel->OutlineThickness = 2.0f;
    {
        UIRenderer ui;
        ui.Begin((int)W, (int)H);
        canvas.Draw(ui);
        ui.End();
    }
    LOG_INFO("UITest") << "Рендер (спрайт+виджеты): OK";

    ok = scaleOk && clickOk && toggleOk && missOk;
    LOG_INFO("UITest") << (ok ? "=== ВСЕ ПРОВЕРКИ ПРОЙДЕНЫ ===" : "=== ЕСТЬ ПРОВАЛЫ ===");
    return ok;
}

// Панель настроек (F4) — витрина всех компонентов UI: подложка (UIPanel),
// заголовок и подписи (UILabel), иконка (UISprite), полоски статов
// (UIProgressBar), тумблеры (UIToggle) и кнопки (UIButton). Виджеты сами тянут
// актуальные значения и правят реальное состояние игры/движка через источники
// и колбэки — после сборки канвас трогать вручную не нужно. ReferenceSize
// включает масштабирование под соотношение сторон окна.
UICanvas BuildSettingsPanel(GameState& game, bool& postEnabled, bool& shadowsEnabled,
                            const Texture& iconTexture, Window& window,
                            const std::string& screenshotPath) {
    UICanvas panel;
    panel.ReferenceSize = {1280.0f, 720.0f};

    auto* bg = panel.Add<UIPanel>();
    bg->Anchor = UIAnchor::Center; bg->Size = {460.0f, 500.0f};
    bg->Color = {0.05f, 0.06f, 0.09f}; bg->Alpha = 0.94f;
    bg->OutlineThickness = 2.0f; bg->OutlineColor = {0.82f, 0.76f, 0.6f};

    auto* title = panel.Add<UILabel>();
    title->Anchor = UIAnchor::Center; title->Offset = {0.0f, -210.0f}; title->Size = {460.0f, 0.0f};
    title->CenterInSize = true; title->Text = "SETTINGS"; title->Scale = 3.0f;

    auto* icon = panel.Add<UISprite>();
    icon->Anchor = UIAnchor::Center; icon->Offset = {0.0f, -150.0f}; icon->Size = {56.0f, 56.0f};
    icon->KeepAspect = true; icon->SpriteTexture = &iconTexture;

    // Полоски статов (демонстрация UIProgressBar с динамическим источником)
    struct BarDef { const char* label; float PlayerStats::* field; glm::vec3 color; float y; };
    BarDef bars[] = {
        {"HP", &PlayerStats::Health, {0.85f, 0.25f, 0.25f}, -95.0f},
        {"EN", &PlayerStats::Energy, {0.95f, 0.85f, 0.30f}, -68.0f},
    };
    for (const BarDef& d : bars) {
        auto* bar = panel.Add<UIProgressBar>();
        bar->Anchor = UIAnchor::Center; bar->Offset = {0.0f, d.y}; bar->Size = {300.0f, 18.0f};
        bar->Label = d.label; bar->FillColor = d.color;
        float PlayerStats::* field = d.field;
        bar->ValueSource = [&game, field] { return game.Stats.*field / 100.0f; };
    }

    // Тумблеры (UIToggle) — правят реальные флаги рендера/движка
    struct ToggleDef { const char* label; float y; std::function<bool()> get; std::function<void(bool)> set; };
    ToggleDef toggles[] = {
        {"Post-processing", -20.0f, [&postEnabled]{ return postEnabled; }, [&postEnabled](bool v){ postEnabled = v; }},
        {"Shadows",          15.0f, [&shadowsEnabled]{ return shadowsEnabled; }, [&shadowsEnabled](bool v){ shadowsEnabled = v; }},
        {"Asset hot-reload", 50.0f, []{ return AssetManager::Instance().HotReloadEnabled(); },
                                    [](bool v){ AssetManager::Instance().EnableHotReload(v); }},
    };
    for (const ToggleDef& d : toggles) {
        auto* t = panel.Add<UIToggle>();
        t->Anchor = UIAnchor::Center; t->Offset = {-150.0f, d.y}; t->Size = {52.0f, 24.0f};
        t->Label = d.label; t->ValueSource = d.get; t->OnChanged = d.set;
    }

    // Кнопки (UIButton) — действия
    struct ButtonDef { const char* label; float y; std::function<void()> onClick; };
    ButtonDef buttons[] = {
        {"Screenshot", 110.0f, [&window, &screenshotPath]{ SaveScreenshot(screenshotPath, window.Width(), window.Height()); }},
        {"Reload assets", 155.0f, []{ AssetManager::Instance().ReloadAll(); }},
        {"Close", 200.0f, [&game]{ game.UiPanelOpen = false; }},
    };
    for (const ButtonDef& d : buttons) {
        auto* b = panel.Add<UIButton>();
        b->Anchor = UIAnchor::Center; b->Offset = {0.0f, d.y}; b->Size = {200.0f, 38.0f};
        b->Label = d.label; b->OnClick = d.onClick;
    }
    return panel;
}

} // namespace

int main() {
    Log::Init("sage_engine.log");
    LOG_INFO("Game") << "The Boat (alpha) v" << kSageEngineVersion << " запускается...";

    try {
        // Размер и заголовок окна настраиваются через переменные окружения —
        // тот же паттерн, что и остальные SAGE_* debug-переопределения ниже
        // (см. ApplyDebugEnvOverrides), чтобы не редактировать код движка
        // ради другого разрешения при разработке/тестировании.
        int windowWidth = 1280;
        int windowHeight = 720;
        if (const char* w = std::getenv("SAGE_WINDOW_WIDTH")) windowWidth = std::atoi(w);
        if (const char* h = std::getenv("SAGE_WINDOW_HEIGHT")) windowHeight = std::atoi(h);
        std::string windowTitle = std::string("The Boat (alpha) v") + kSageEngineVersion + " - SAGE Engine";

        Window window(windowWidth, windowHeight, windowTitle);

        // Формальное владение жизненным циклом движковых подсистем (пул
        // фоновых задач + единая система ассетов): поднимает их в правильном
        // порядке, гарантированно гасит в обратном при выходе из этой области
        // видимости (в т.ч. на раннем return self-тестов ниже и при
        // исключении) — включая GL-очистку AssetManager ПОКА GL-контекст ещё
        // жив (window объявлено раньше engine, поэтому при разрушении стека
        // engine.Shutdown() отработает первым — см. core/EngineRuntime.h).
        // Hot-reload ассетов (перечитывать изменённые шейдеры/текстуры на
        // лету) включается SAGE_HOT_RELOAD, удобно при разработке; в проде
        // выключен = без накладных stat-ов.
        EngineConfig engineConfig;
        engineConfig.HotReload = (std::getenv("SAGE_HOT_RELOAD") != nullptr);
        EngineRuntime engine(engineConfig);

        // Самопроверка async + системы ассетов для CI/отладки — прогоняет пул
        // задач, async-загрузку, кэш/дедуп, hot-reload и статистику на реальном
        // GL-контексте, затем выходит, не запуская игру. Партии не касается.
        if (std::getenv("SAGE_TEST_ASYNC")) {
            bool passed = RunAsyncSelfTest();
            return passed ? 0 : 1;
        }
        if (std::getenv("SAGE_TEST_UI")) {
            bool passed = RunUISelfTest();
            return passed ? 0 : 1;
        }

        InputSystem input;
        input.Attach(window.Handle());
        GameActions::RegisterDefaultBindings(input);
        // Необязательный файл настроек рядом с игрой — переопределяет
        // раскладку по умолчанию без пересборки (см. GameActions.h).
        GameActions::LoadBindingsFromFile(input.Actions(), "keybindings.cfg");
        glfwSetInputMode(window.Handle(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // ---- Рендер-ресурсы (через единую систему ассетов) ----
        // Держим хендлы Asset<Shader>/Asset<Texture> живыми (пока они в области
        // видимости, ассет не выгрузится), а работаем через ссылки — тогда весь
        // код ниже не меняется, а hot-reload обновляет ту же программу/текстуру
        // НА МЕСТЕ, и ссылки остаются валидны. Пути — относительно asset-root.
        AssetManager& assets = AssetManager::Instance();
        auto loadShader = [&](const char* v, const char* f) -> Asset<Shader> {
            Asset<Shader> s = assets.LoadShader(v, f);
            if (!s.IsReady()) throw std::runtime_error(std::string("Не удалось загрузить шейдер: ") + v);
            return s;
        };
        Asset<Shader> voxelShaderA      = loadShader("shaders/voxel.vert", "shaders/voxel.frag");
        Asset<Shader> waterShaderA      = loadShader("shaders/water.vert", "shaders/water.frag");
        Asset<Shader> basicShaderA      = loadShader("shaders/basic.vert", "shaders/basic.frag");
        Asset<Shader> skyboxShaderA     = loadShader("shaders/skybox.vert", "shaders/skybox.frag");
        Asset<Shader> particleShaderA   = loadShader("shaders/particle.vert", "shaders/particle.frag");
        Asset<Shader> billboardShaderA  = loadShader("shaders/billboard.vert", "shaders/billboard.frag");
        Asset<Shader> shadowDepthShaderA= loadShader("shaders/shadow_depth.vert", "shaders/shadow_depth.frag");
        Asset<Shader> postShaderA       = loadShader("shaders/post.vert", "shaders/post.frag");
        Shader& voxelShader = *voxelShaderA;
        Shader& waterShader = *waterShaderA;
        Shader& basicShader = *basicShaderA;
        Shader& skyboxShader = *skyboxShaderA;
        Shader& particleShader = *particleShaderA;
        Shader& billboardShader = *billboardShaderA;
        Shader& shadowDepthShader = *shadowDepthShaderA;
        Shader& postShader = *postShaderA;

        // Пост-процессинг: сцена рисуется в HDR-буфер, затем полноэкранный
        // проход (тон-маппинг/экспозиция/виньетка) выводит её на экран.
        // Карта теней: depth-проход из точки зрения солнца, приёмники
        // (voxel/basic/water) сэмплируют её. Оба можно отключить env-флагом
        // (SAGE_NO_POST / SAGE_NO_SHADOWS) — удобно для сравнения "до/после".
        Framebuffer sceneFbo(window.Width(), window.Height());
        PostProcess post;
        PostProcessSettings postSettings;
        ShadowMap shadows(2048);
        bool postEnabled = (std::getenv("SAGE_NO_POST") == nullptr);
        bool shadowsEnabled = (std::getenv("SAGE_NO_SHADOWS") == nullptr);
        if (const char* exp = std::getenv("SAGE_EXPOSURE")) postSettings.Exposure = (float)std::atof(exp);

        Skybox skybox({
            "assets/textures/skybox/px.png", "assets/textures/skybox/nx.png",
            "assets/textures/skybox/py.png", "assets/textures/skybox/ny.png",
            "assets/textures/skybox/pz.png", "assets/textures/skybox/nz.png"
        });
        auto loadTexture = [&](const char* p, TextureFilter fil, bool mip) -> Asset<Texture> {
            Asset<Texture> t = assets.LoadTexture(p, fil, mip);
            if (!t.IsReady()) throw std::runtime_error(std::string("Не удалось загрузить текстуру: ") + p);
            return t;
        };
        Asset<Texture> blockAtlasA = loadTexture("textures/blocks_atlas.png", TextureFilter::Nearest, /*mip=*/false);
        Asset<Texture> alertIconA  = loadTexture("textures/icon_alert.png", TextureFilter::Bilinear, /*mip=*/true);
        Texture& blockAtlas = *blockAtlasA;
        Texture& alertIcon = *alertIconA;

        BillboardSystem billboards;
        // Иконка "клюёт!" над поплавком — создаём один раз, скрытую, и
        // просто переключаем видимость/позицию по состоянию рыбалки каждый
        // кадр, вместо Add/Remove (дешевле и демонстрирует типичный паттерн
        // использования системы для долгоживущего маркера).
        int biteIconId = billboards.Add({
            glm::vec3(0.0f), glm::vec2(0.45f, 0.45f), &alertIcon,
            glm::vec4(1.0f), 0.0f, BillboardPivot::Bottom, /*visible=*/false
        });

        UIRenderer ui;
        DebugOverlay debugOverlay;
        Asset<Mesh> cubeMesh = assets.Cube(); // мусор, поплавок (процедурный куб)

        Camera camera;

        // ---- Игровое состояние: мир, корабль, игрок, инвентарь, системы ----
        GameState game;
        // Camera/billboards созданы раньше GameState — привязка снаружи неё,
        // как и InputSystem выше. ParticleSystem же живёт внутри GameState,
        // поэтому её привязываем на неё саму (game.Particles). Все Bind*
        // должны отработать ДО RunScript() ниже — иначе демо-скрипт упадёт
        // на первом же обращении к ещё не привязанной системе.
        game.Scripts.BindInput(input.Actions());
        game.Scripts.BindCamera(camera);
        game.Scripts.BindParticles(game.Particles);
        game.Scripts.BindBillboards(billboards);
        game.Scripts.BindAudio(game.Audio);

        // demo_features.lua — витрина расширенного Lua-API движка (спавн
        // объектов, таймеры, корутины, камера, частицы, билборды), никак не
        // относится к геймплею The Boat — только по явному запросу, как и
        // остальные SAGE_* debug-флаги.
        if (std::getenv("SAGE_RUN_DEMO_SCRIPT")) {
            game.Scripts.RunScript("assets/scripts/demo_features.lua");
        }

        UICanvas statsHud = BuildStatsHud(game);
        ApplyDebugEnvOverrides(game, camera);

        // Путь скриншота — общий для F2 (ручной, по хоткею) и автоскриншота
        // для CI/тестов (по кадру); раньше F2 был захардкожен на
        // "screenshot.png" отдельно от переопределяемого CI-пути, из-за чего
        // SAGE_SCREENSHOT_PATH молча не действовал на ручной скриншот.
        std::string screenshotPath = "screenshot.png";
        if (const char* pathEnv = std::getenv("SAGE_SCREENSHOT_PATH")) screenshotPath = pathEnv;

        // Интерактивная панель настроек (F4): витрина всех UI-компонентов,
        // правит реальные флаги (пост/тени/hot-reload) и делает скриншот/
        // перезагрузку ассетов. Пока открыта — курсор виден, взгляд/движение
        // заморожены, клики идут в UI, а не в мир.
        UICanvas settingsPanel = BuildSettingsPanel(game, postEnabled, shadowsEnabled,
                                                    alertIcon, window, screenshotPath);
        bool prevUiMouseDown = false;
        if (std::getenv("SAGE_OPEN_UI_PANEL")) game.UiPanelOpen = true; // для скриншота/CI

        // ---- Автоскриншот для CI/тестов ----
        int autoScreenshotFrame = -1;
        if (const char* frameEnv = std::getenv("SAGE_SCREENSHOT_AT_FRAME")) autoScreenshotFrame = std::atoi(frameEnv);
        int frameCounter = 0;

        LOG_INFO("Game") << "Мир создан. Корабль в (" << game.Ship.Center.x << ", "
                          << game.Ship.Center.y << ", " << game.Ship.Center.z << ")";

        // Отрисовка отбрасывающей тень геометрии (корабль + заспавненные
        // скриптами объекты) для depth-прохода теней. Один depth-шейдер годится
        // и для воксельных чанков, и для обычных мешей — позиция у обоих в
        // location 0 (см. shadow_depth.vert). Вода не кастит (плоская), только
        // принимает тень.
        auto drawShadowCasters = [&](Shader& depthShader) {
            for (auto& [coord, chunk] : game.Terrain.Chunks()) {
                glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(chunk->WorldPos()));
                depthShader.SetMat4("uModel", model);
                chunk->Mesh().Draw();
            }
            for (auto& object : game.SceneData.Objects()) {
                if (!object->MeshComponent) continue;
                depthShader.SetMat4("uModel", object->TransformComponent.GetMatrix());
                object->MeshComponent->Draw();
            }
        };

        float lastFrame = (float)glfwGetTime();
        float fpsTimer = 0.0f; int fpsFrames = 0; float fps = 0.0f;

        while (!window.ShouldClose()) {
            float currentFrame = (float)glfwGetTime();
            float deltaTime = std::min(currentFrame - lastFrame, 0.05f); // защита от рывка после паузы/лага
            lastFrame = currentFrame;

            fpsTimer += deltaTime; ++fpsFrames;
            if (fpsTimer >= 0.5f) { fps = fpsFrames / fpsTimer; fpsTimer = 0.0f; fpsFrames = 0; }

            GLFWwindow* w = window.Handle();

            // ================= ВВОД =================
            // Один вызов Update() прогоняет ВСЕ зарегистрированные действия
            // через их привязки (клавиатура/мышь/колесо) — дальше весь код
            // читает именованные действия, а не сырые коды клавиш.
            input.Update(w);
            // Пока открыта интерактивная панель — мышь управляет курсором UI,
            // а не поворотом камеры (иначе взгляд бы крутился при наведении).
            if (!game.UiPanelOpen) input.ApplyMouseDelta(camera);
            InputMap& actions = input.Actions();

            if (actions.WasPressed(GameActions::QuitGame)) glfwSetWindowShouldClose(w, true);
            if (actions.WasPressed(GameActions::Screenshot)) SaveScreenshot(screenshotPath, window.Width(), window.Height());
            if (actions.WasPressed(GameActions::ToggleDebugHud)) game.DebugHudVisible = !game.DebugHudVisible;
            if (actions.WasPressed(GameActions::ToggleNoclip)) {
                game.Noclip = !game.Noclip;
                game.ShowToast(game.Noclip ? "Noclip ON" : "Noclip OFF");
            }
            if (actions.WasPressed(GameActions::ToggleCrafting)) {
                game.CraftMenuOpen = !game.CraftMenuOpen;
                game.Audio.PlaySound2D(GameSounds::Pickup, 0.4f); // короткий UI-щелчок
            }
            if (actions.WasPressed(GameActions::ToggleUiPanel)) {
                game.UiPanelOpen = !game.UiPanelOpen;
                // Показываем/прячем системный курсор под интерактивный UI
                glfwSetInputMode(w, GLFW_CURSOR, game.UiPanelOpen ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
                game.Audio.PlaySound2D(GameSounds::Pickup, 0.4f);
            }

            const char* const* hotbarSlots = GameActions::HotbarSlotNames();
            for (int i = 0; i < Inventory::HotbarSize; ++i) {
                if (actions.WasPressed(hotbarSlots[i])) game.Items.SelectSlot(i);
            }
            if (actions.WasPressed(GameActions::HotbarNext)) game.Items.ScrollSlot(1);
            if (actions.WasPressed(GameActions::HotbarPrev)) game.Items.ScrollSlot(-1);

            // Пока открыто меню крафта, клики мышью ему не передаются —
            // иначе клик по пункту меню одновременно ломал бы блок под прицелом
            bool craftMenuConsumedClicks = game.CraftMenuOpen;
            if (game.CraftMenuOpen) {
                PlayerActions::HandleCraftMenuInput(game, actions);
            }

            // Интерактивная панель настроек: собираем состояние мыши из GLFW и
            // прогоняем виджеты. Клики этого кадра "съедаются" панелью (в мир
            // не идут). Фронт нажатия/отпускания считаем сами по prev-состоянию.
            bool uiPanelConsumedClicks = game.UiPanelOpen;
            if (game.UiPanelOpen) {
                double mx, my; glfwGetCursorPos(w, &mx, &my);
                bool lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                UIInputState uiIn;
                uiIn.Mouse = {(float)mx, (float)my};
                uiIn.MouseDown = lmb;
                uiIn.MousePressed = lmb && !prevUiMouseDown;
                uiIn.MouseReleased = !lmb && prevUiMouseDown;
                prevUiMouseDown = lmb;
                settingsPanel.Update(uiIn, (float)window.Width(), (float)window.Height());
            } else {
                prevUiMouseDown = false;
            }

            // ================= ДВИЖЕНИЕ И ФИЗИКА =================
            if (game.UiPanelOpen) {
                // Панель открыта — движение и полёт заморожены (мышь/клики в UI)
                game.Player.Velocity = glm::vec3(0.0f);
            } else if (game.Noclip) {
                UpdateNoclipFly(game, actions, camera, deltaTime);
            } else {
                glm::vec3 moveDir = game.CraftMenuOpen ? glm::vec3(0.0f) : ReadMoveDirection(actions, camera);
                bool wantsJump = !game.CraftMenuOpen && actions.IsDown(GameActions::Jump);
                bool wantsRun = !game.CraftMenuOpen && actions.IsDown(GameActions::Run);
                game.Player.Update(game.Terrain, deltaTime, moveDir, wantsJump, wantsRun, game.Stats.CanRun());
            }
            camera.Position = game.Player.EyePosition();

            // Слушатель 3D-звука едет с камерой — панорама/громкость позиционных
            // звуков (всплески, установка блоков, поклёвка) считаются относительно
            // взгляда игрока. Обновляем до Play* этого кадра.
            game.Audio.SetListener(camera.Position, camera.Front, camera.Up);

            // ================= СИМУЛЯЦИЯ (статы, сутки, мусор, рыбалка, готовка) =================
            bool isExerting = !game.Noclip && game.Player.IsMovingFast();
            game.UpdateSimulation(deltaTime, isExerting);

            // ================= ДЕЙСТВИЯ ИГРОКА =================
            PlayerActions::LookContext look = PlayerActions::ComputeLookContext(game, camera);

            if (actions.WasPressed(GameActions::PickUpTrash) && !game.CraftMenuOpen && !game.UiPanelOpen) {
                PlayerActions::PickUpTrash(game, look);
            }
            if (!craftMenuConsumedClicks && !uiPanelConsumedClicks && actions.WasPressed(GameActions::BreakOrHook)) {
                PlayerActions::HandleLeftClick(game, look);
            }
            if (!craftMenuConsumedClicks && !uiPanelConsumedClicks && actions.WasPressed(GameActions::UseItem)) {
                PlayerActions::HandleRightClick(game, camera, look);
            }

            game.Terrain.RebuildDirtyMeshes();

            // Забираем результаты фоновых загрузок, готовые к GL-загрузке
            // (создание текстур/мешей в главном потоке). Бюджет ~2 мс/кадр —
            // пачка тяжёлых загрузок размазывается по кадрам, а не фризит один.
            MainThreadDispatcher::Instance().Drain(2.0);

            // Hot-reload: перечитываем изменённые на диске ассеты (шейдеры/
            // текстуры/модели) и обновляем их НА МЕСТЕ. No-op, если выключен
            // (SAGE_HOT_RELOAD) — тогда без накладных расходов.
            AssetManager::Instance().PollHotReload();

            // Освобождаем доигравшие одноразовые звуки этого кадра (эмбиент,
            // музыка и активные лупы не трогаются — они управляются по дескриптору)
            game.Audio.Update();

            // ================= РЕНДЕР =================
            g_renderStats.Reset();

            glm::mat4 view = camera.GetViewMatrix();
            glm::mat4 proj = camera.GetProjectionMatrix((float)window.Width() / (float)window.Height());

            // Держим offscreen-буфер сцены в размер окна (no-op, если не менялось)
            sceneFbo.Resize(window.Width(), window.Height());

            // ---- ПРОХОД ТЕНЕЙ: глубина сцены из точки зрения солнца ----
            // Ортобокс следует за кораблём (стабильный центр — меньше мерцания
            // краёв тени, чем если центрировать на игроке).
            if (shadowsEnabled) {
                shadows.SetLightMatrix(game.SceneData.Lighting.Sun.Direction, game.Ship.Center, kShadowRadius);
                shadows.BeginRender();
                shadowDepthShader.Use();
                shadowDepthShader.SetMat4("uLightSpace", shadows.LightMatrix());
                drawShadowCasters(shadowDepthShader);
                shadows.EndRender(window.Width(), window.Height());
            }

            // ---- ПРОХОД СЦЕНЫ: в HDR-буфер (или сразу в экран, если пост выключен) ----
            if (postEnabled) sceneFbo.Bind();
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Карта теней на юнит 1 — общая для voxel/basic/water на весь проход
            // (юнит 0 занят их собственными текстурами: атлас/спрайт).
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, shadows.DepthTexture());

            skybox.Draw(skyboxShader, view, proj, game.DayNight.SkyTint());

            // --- корабль (воксельные чанки) ---
            voxelShader.Use();
            voxelShader.SetMat4("uView", view);
            voxelShader.SetMat4("uProjection", proj);
            UploadLighting(voxelShader, game.SceneData.Lighting);
            UploadShadowUniforms(voxelShader, shadows.LightMatrix(), 1, shadowsEnabled);
            blockAtlas.Bind(0);
            voxelShader.SetInt("uAtlas", 0);
            voxelShader.SetInt("uUseTexture", 1);
            for (auto& [coord, chunk] : game.Terrain.Chunks()) {
                glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(chunk->WorldPos()));
                voxelShader.SetMat4("uModel", model);
                chunk->Mesh().Draw();
            }

            // --- мусор и поплавок (обычные меши) ---
            basicShader.Use();
            basicShader.SetMat4("uView", view);
            basicShader.SetMat4("uProjection", proj);
            basicShader.SetVec3("uViewPos", camera.Position);
            UploadLighting(basicShader, game.SceneData.Lighting);
            UploadShadowUniforms(basicShader, shadows.LightMatrix(), 1, shadowsEnabled);
            basicShader.SetInt("uUseTexture", 0);

            for (const TrashItem& item : game.Trash.Items()) {
                glm::mat4 model = glm::translate(glm::mat4(1.0f), item.Position);
                model = glm::rotate(model, item.BobPhase * 0.4f, glm::vec3(0, 1, 0));
                model = glm::scale(model, glm::vec3(0.35f));
                basicShader.SetMat4("uModel", model);
                basicShader.SetVec3("uObjectColor", GetItemFloatColor(item.Type));
                cubeMesh->Draw();
            }

            // --- объекты сцены, заспавненные Lua-скриптами (game.Scripts) ---
            for (auto& object : game.SceneData.Objects()) {
                if (!object->MeshComponent) continue; // скрипт создал GameObject, но не назначил меш — не рисуем
                basicShader.SetMat4("uModel", object->TransformComponent.GetMatrix());
                basicShader.SetVec3("uObjectColor", object->Color);
                object->MeshComponent->Draw();
            }

            if (game.Fishing.IsActive()) {
                glm::mat4 model = glm::translate(glm::mat4(1.0f), game.Fishing.VisualBobberPosition());
                model = glm::scale(model, glm::vec3(0.16f));
                basicShader.SetMat4("uModel", model);
                basicShader.SetVec3("uObjectColor", glm::vec3(0.9f, 0.15f, 0.1f));
                cubeMesh->Draw();
            }

            // Иконка "клюёт!" — billboard над поплавком, видна только в
            // момент поклёвки (демонстрация BillboardSystem: долгоживущий
            // маркер, у которого меняется позиция/видимость, а не Add/Remove)
            bool isBiting = game.Fishing.CurrentState() == FishingSystem::State::Bite;
            billboards.SetVisible(biteIconId, isBiting);
            if (isBiting) {
                billboards.SetPosition(biteIconId, game.Fishing.VisualBobberPosition() + glm::vec3(0.0f, 0.25f, 0.0f));
            }

            // --- океан (после непрозрачного: полупрозрачная вода) ---
            waterShader.Use();
            waterShader.SetMat4("uView", view);
            waterShader.SetMat4("uProjection", proj);
            waterShader.SetVec3("uViewPos", camera.Position);
            waterShader.SetFloat("uTime", currentFrame);
            waterShader.SetFloat("uScrollSpeed", 2.0f);
            waterShader.SetVec2("uCenter", glm::vec2(game.Player.Position.x, game.Player.Position.z));
            UploadLighting(waterShader, game.SceneData.Lighting);
            UploadShadowUniforms(waterShader, shadows.LightMatrix(), 1, shadowsEnabled);
            game.Water.Draw();

            // --- частицы (дым/искры/всплески) поверх воды, до UI ---
            game.Particles.Draw(particleShader, camera, view, proj);

            // --- билборды (иконки/маркеры, всегда развёрнутые к камере) ---
            billboards.Draw(billboardShader, camera, view, proj);

            // ---- ПОСТ-ПРОЦЕССИНГ: HDR-сцена -> экран ----
            // Полноэкранный проход выводит offscreen-буфер сцены на экран с
            // тон-маппингом/экспозицией/виньеткой. UI рисуется ПОСЛЕ него, в
            // экранный буфер напрямую, чтобы текст/полоски не тон-мапились и
            // не виньетировались вместе со сценой.
            if (postEnabled) {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, window.Width(), window.Height());
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

                postShader.Use();
                postShader.SetFloat("uExposure", postSettings.Exposure);
                postShader.SetFloat("uGamma", postSettings.Gamma);
                postShader.SetFloat("uSaturation", postSettings.Saturation);
                postShader.SetFloat("uContrast", postSettings.Contrast);
                postShader.SetFloat("uVignette", postSettings.VignetteStrength);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, sceneFbo.ColorTexture());
                postShader.SetInt("uScene", 0);
                post.Draw();
            }

            // --- UI: худ (виджеты) + immediate-mode (хотбар/подсказки/крафт) ---
            ui.Begin(window.Width(), window.Height());
            statsHud.Draw(ui);
            GameHud::DrawWorldHud(ui, game, look, window.Width(), window.Height());
            if (game.UiPanelOpen) settingsPanel.Draw(ui); // поверх худа
            ui.End();

            if (game.DebugHudVisible) {
                GameHud::DrawDebugOverlay(debugOverlay, game, fps, window.Width(), window.Height(),
                                          postEnabled, shadowsEnabled);
            }

            // ---- Скриншот на заданном кадре (для CI) ----
            ++frameCounter;
            if (autoScreenshotFrame >= 0 && frameCounter == autoScreenshotFrame) {
                SaveScreenshot(screenshotPath, window.Width(), window.Height());
                glfwSetWindowShouldClose(window.Handle(), true);
            }

            window.SwapBuffers();
            window.PollEvents();
        }

        // Явный вызов не нужен: EngineRuntime гасит JobSystem (join воркеров)
        // и очищает AssetManager (GL-объекты текстур/мешей/шейдеров) в своём
        // деструкторе при выходе из этой области видимости — ПОКА GL-контекст
        // (window) ещё жив, см. объявление engine выше и core/EngineRuntime.h.
    } catch (const std::exception& e) {
        LOG_ERROR("Game") << "Фатальная ошибка: " << e.what();
        std::cerr << "Фатальная ошибка: " << e.what() << std::endl;
        return -1;
    }

    LOG_INFO("Game") << "Завершение работы";
    return 0;
}
