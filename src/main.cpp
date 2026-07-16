// ============================================================================
// SAGE Engine — generic bootstrap.
//
// Часть Фазы 1 масштабного рефакторинга: старая игра The Boat (воксельный
// мир, рыбалка/крафт/статы/HUD, ручной voxel-AABB PlayerController) удалена
// целиком (src/game/*, src/voxel/*) — движок больше не тянет за собой ни
// строки конкретной игровой логики. main() ниже — это ТОЛЬКО инициализация
// движковых подсистем и цикл "ввод -> скрипты -> рендер", без какой-либо
// игровой механики (даже передвижение игрока сюда сознательно не
// возвращается — оно появится позже как Lua-скрипт поверх физического API
// из Фазы 3). Полная генерализация (games/<name>/, SAGE_GAME) — Фаза 5;
// сейчас это промежуточный смок-тест движка: окно, куб, ScriptEngine,
// гоняющий Lua-скрипт по SAGE_RUN_DEMO_SCRIPT.
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
#include "core/EventBus.h"
#include "render/Shader.h"
#include "render/Camera.h"
#include "asset/AssetManager.h"
#include "render/Texture.h"
#include "render/Skybox.h"
#include "render/BillboardSystem.h"
#include "render/ParticleSystem.h"
#include "render/LightingUpload.h"
#include "render/Screenshot.h"
#include "render/DebugOverlay.h"
#include "render/Framebuffer.h"
#include "render/ShadowMap.h"
#include "render/PostProcess.h"
#include "render/RenderContext.h"
#include "render/RenderPass.h"
#include "render/RenderPipeline.h"
#include "render/CallbackPass.h"
#include "render/ShadowPass.h"
#include "render/PostProcessPass.h"
#include "ui/UIRenderer.h"
#include "ui/UICanvas.h"
#include "ui/Widgets.h"
#include "ui/UIManager.h"
#include "scene/Scene.h"
#include "audio/AudioEngine.h"
#include "scripting/ScriptEngine.h"

namespace {

// Читает позицию/угол камеры и время суток из переменных окружения —
// используется автотестами/CI для детерминированных скриншотов без
// реального ввода.
void ApplyDebugEnvOverrides(Camera& camera) {
    if (const char* posEnv = std::getenv("SAGE_CAM_POS")) {
        float x, y, z;
        if (std::sscanf(posEnv, "%f,%f,%f", &x, &y, &z) == 3) camera.Position = {x, y, z};
    }
    if (const char* yawEnv = std::getenv("SAGE_CAM_YAW")) camera.Yaw = (float)std::atof(yawEnv);
    if (const char* pitchEnv = std::getenv("SAGE_CAM_PITCH")) camera.Pitch = (float)std::atof(pitchEnv);
    camera.ProcessMouse(0.0f, 0.0f); // пересчитать Front/Right/Up после ручной правки Yaw/Pitch
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

} // namespace

int main() {
    Log::Init("sage_engine.log");
    LOG_INFO("Engine") << "SAGE Engine v" << kSageEngineVersion << " запускается...";

    try {
        int windowWidth = 1280;
        int windowHeight = 720;
        if (const char* w = std::getenv("SAGE_WINDOW_WIDTH")) windowWidth = std::atoi(w);
        if (const char* h = std::getenv("SAGE_WINDOW_HEIGHT")) windowHeight = std::atoi(h);
        std::string windowTitle = std::string("SAGE Engine v") + kSageEngineVersion;

        Window window(windowWidth, windowHeight, windowTitle);

        // Формальное владение жизненным циклом движковых подсистем — см.
        // core/EngineRuntime.h. Hot-reload ассетов включается SAGE_HOT_RELOAD.
        EngineConfig engineConfig;
        engineConfig.HotReload = (std::getenv("SAGE_HOT_RELOAD") != nullptr);
        EngineRuntime engine(engineConfig);

        // Самопроверки для CI/отладки — прогоняют подсистему и выходят, не
        // запуская основной цикл.
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
        // Минимальный набор действий для смок-теста рантайма (свободная
        // камера-полёт) — это движковая утилита (Camera::ProcessKeyboard уже
        // часть ядра), не игровая механика. Настоящее передвижение игрока с
        // коллизией/гравитацией появится позже как Lua-скрипт поверх
        // физического API (см. план, Фаза 3), не здесь.
        InputMap& actions = input.Actions();
        actions.Register("FlyForward").Bind(InputBinding::Key(GLFW_KEY_W));
        actions.Register("FlyBackward").Bind(InputBinding::Key(GLFW_KEY_S));
        actions.Register("FlyLeft").Bind(InputBinding::Key(GLFW_KEY_A));
        actions.Register("FlyRight").Bind(InputBinding::Key(GLFW_KEY_D));
        actions.Register("FlyUp").Bind(InputBinding::Key(GLFW_KEY_SPACE));
        actions.Register("FlyDown").Bind(InputBinding::Key(GLFW_KEY_LEFT_CONTROL));
        actions.Register("Quit").Bind(InputBinding::Key(GLFW_KEY_ESCAPE));
        actions.Register("Screenshot").Bind(InputBinding::Key(GLFW_KEY_F2));
        actions.Register("ToggleDebugHud").Bind(InputBinding::Key(GLFW_KEY_F3));
        glfwSetInputMode(window.Handle(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // ---- Рендер-ресурсы ----
        AssetManager& assets = AssetManager::Instance();
        auto loadShader = [&](const char* v, const char* f) -> Asset<Shader> {
            Asset<Shader> s = assets.LoadShader(v, f);
            if (!s.IsReady()) throw std::runtime_error(std::string("Не удалось загрузить шейдер: ") + v);
            return s;
        };
        Asset<Shader> basicShaderA     = loadShader("shaders/basic.vert", "shaders/basic.frag");
        Asset<Shader> skyboxShaderA    = loadShader("shaders/skybox.vert", "shaders/skybox.frag");
        Asset<Shader> particleShaderA  = loadShader("shaders/particle.vert", "shaders/particle.frag");
        Asset<Shader> billboardShaderA = loadShader("shaders/billboard.vert", "shaders/billboard.frag");
        Shader& basicShader = *basicShaderA;
        Shader& skyboxShader = *skyboxShaderA;
        Shader& particleShader = *particleShaderA;
        Shader& billboardShader = *billboardShaderA;

        PostProcessPass postProcessPass(window.Width(), window.Height());
        bool postEnabled = (std::getenv("SAGE_NO_POST") == nullptr);
        bool shadowsEnabled = (std::getenv("SAGE_NO_SHADOWS") == nullptr);
        if (const char* exp = std::getenv("SAGE_EXPOSURE")) postProcessPass.Settings.Exposure = (float)std::atof(exp);

        Skybox skybox({
            "assets/textures/skybox/px.png", "assets/textures/skybox/nx.png",
            "assets/textures/skybox/py.png", "assets/textures/skybox/ny.png",
            "assets/textures/skybox/pz.png", "assets/textures/skybox/nz.png"
        });

        BillboardSystem billboards;
        UIRenderer ui;
        DebugOverlay debugOverlay;
        Asset<Mesh> cubeMesh = assets.Cube();

        Camera camera;
        camera.Position = {0.0f, 2.0f, 6.0f};

        // ---- Сцена + скриптинг движка ----
        // ВАЖНО про порядок объявления: Scripts должен пережить SceneData
        // (объекты сцены могут держать sol::table в LuaData) — объявляем
        // ScriptEngine ПЕРЕД Scene (C++ разрушает члены в обратном порядке
        // объявления, Scripts разрушится последним).
        ScriptEngine scripts;
        Scene scene("Untitled");
        ParticleSystem particles;
        AudioEngine audio;

        // UI, полностью управляемый из Lua (см. ScriptEngine::BindUI).
        // Объявлена ПОСЛЕ scripts специально — виджеты держат Lua-колбэки
        // (sol::protected_function), должны разрушиться ДО sol::state внутри
        // scripts (обратный порядок объявления это обеспечивает).
        UIManager scriptUi;

        // Шина событий движка (см. ScriptEngine::BindEvents) — та же причина
        // порядка объявления, что у scriptUi (подписки держат Lua-замыкания).
        EventBus eventBus;

        scripts.BindScene(scene);
        scripts.BindInput(actions);
        scripts.BindCamera(camera);
        scripts.BindParticles(particles);
        scripts.BindBillboards(billboards);
        scripts.BindAudio(audio);
        scripts.BindUI(scriptUi);
        scripts.BindEvents(eventBus);

        // Один куб на сцене — минимальный смок-объект, доказывающий, что
        // рендер-пайплайн ниже реально что-то рисует до появления полноценной
        // Lua-игры (Фаза 5).
        GameObject& smokeCube = scene.CreateObject("SmokeTestCube");
        smokeCube.MeshComponent = cubeMesh.Shared();
        smokeCube.Color = {0.6f, 0.75f, 0.9f};

        // demo_features.lua — витрина Lua-API движка (спавн объектов, таймеры,
        // корутины, камера, частицы, билборды, JSON, события, сцены) — не
        // относится ни к какой конкретной игре, запускается только по явному
        // запросу, как и остальные SAGE_* debug-флаги.
        if (std::getenv("SAGE_RUN_DEMO_SCRIPT")) {
            scripts.RunScript("assets/scripts/demo_features.lua");
        }

        ApplyDebugEnvOverrides(camera);

        std::string screenshotPath = "screenshot.png";
        if (const char* pathEnv = std::getenv("SAGE_SCREENSHOT_PATH")) screenshotPath = pathEnv;

        int autoScreenshotFrame = -1;
        if (const char* frameEnv = std::getenv("SAGE_SCREENSHOT_AT_FRAME")) autoScreenshotFrame = std::atoi(frameEnv);
        int frameCounter = 0;

        LOG_INFO("Engine") << "Сцена создана: " << scene.Objects().size() << " объект(ов)";

        // Отрисовка отбрасывающей тень геометрии — сцена целиком (объекты,
        // заспавненные Lua-скриптами, включая smokeCube выше).
        auto drawShadowCasters = [&](Shader& depthShader) {
            for (auto& object : scene.Objects()) {
                if (!object->MeshComponent) continue;
                depthShader.SetMat4("uModel", object->TransformComponent.GetMatrix());
                object->MeshComponent->Draw();
            }
        };

        // ---- Рендер-пайплайн: собирается ОДИН РАЗ, дальше каждый кадр —
        // просто pipeline.Execute(ctx) (см. render/RenderPipeline.h).
        RenderPipeline pipeline;

        ShadowPass* shadowPassPtr = pipeline.Add<ShadowPass>(2048);
        shadowPassPtr->DrawCasters = [&](Shader& depthShader, RenderContext&) {
            drawShadowCasters(depthShader);
        };

        pipeline.Add<CallbackPass>([&](RenderContext& ctx) { postProcessPass.Begin(ctx); });

        pipeline.Add<CallbackPass>([&](RenderContext& ctx) {
            unsigned int shadowTex = ctx.Shadows ? ctx.Shadows->DepthTexture() : 0;
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, shadowTex);
            glm::mat4 lightMatrix = ctx.Shadows ? ctx.Shadows->LightMatrix() : glm::mat4(1.0f);

            skybox.Draw(skyboxShader, ctx.View, ctx.Proj, glm::vec3(1.0f));

            basicShader.Use();
            basicShader.SetMat4("uView", ctx.View);
            basicShader.SetMat4("uProjection", ctx.Proj);
            basicShader.SetVec3("uViewPos", ctx.Cam->Position);
            UploadLighting(basicShader, *ctx.Lighting);
            UploadShadowUniforms(basicShader, lightMatrix, 1, ctx.ShadowsEnabled);
            basicShader.SetInt("uUseTexture", 0);

            for (auto& object : scene.Objects()) {
                if (!object->MeshComponent) continue;
                basicShader.SetMat4("uModel", object->TransformComponent.GetMatrix());
                basicShader.SetVec3("uObjectColor", object->Color);
                object->MeshComponent->Draw();
            }

            particles.Draw(particleShader, *ctx.Cam, ctx.View, ctx.Proj);
            billboards.Draw(billboardShader, *ctx.Cam, ctx.View, ctx.Proj);
        });

        pipeline.Add<CallbackPass>([&](RenderContext& ctx) { postProcessPass.Resolve(ctx); });

        float lastFrame = (float)glfwGetTime();
        float fpsTimer = 0.0f; int fpsFrames = 0; float fps = 0.0f;
        bool debugHudVisible = false;

        while (!window.ShouldClose()) {
            float currentFrame = (float)glfwGetTime();
            float deltaTime = std::min(currentFrame - lastFrame, 0.05f);
            lastFrame = currentFrame;

            fpsTimer += deltaTime; ++fpsFrames;
            if (fpsTimer >= 0.5f) { fps = fpsFrames / fpsTimer; fpsTimer = 0.0f; fpsFrames = 0; }

            GLFWwindow* w = window.Handle();

            input.Update(w);
            input.ApplyMouseDelta(camera);

            if (actions.WasPressed("Quit")) glfwSetWindowShouldClose(w, true);
            if (actions.WasPressed("Screenshot")) SaveScreenshot(screenshotPath, window.Width(), window.Height());
            if (actions.WasPressed("ToggleDebugHud")) debugHudVisible = !debugHudVisible;

            // Свободный полёт — движковая утилита для смок-теста, см. комментарий выше.
            if (actions.IsDown("FlyForward")) camera.ProcessKeyboard(CameraMove::Forward, deltaTime);
            if (actions.IsDown("FlyBackward")) camera.ProcessKeyboard(CameraMove::Backward, deltaTime);
            if (actions.IsDown("FlyLeft")) camera.ProcessKeyboard(CameraMove::Left, deltaTime);
            if (actions.IsDown("FlyRight")) camera.ProcessKeyboard(CameraMove::Right, deltaTime);
            if (actions.IsDown("FlyUp")) camera.ProcessKeyboard(CameraMove::Up, deltaTime);
            if (actions.IsDown("FlyDown")) camera.ProcessKeyboard(CameraMove::Down, deltaTime);

            audio.SetListener(camera.Position, camera.Front, camera.Up);

            scripts.UpdateAll(deltaTime);

            MainThreadDispatcher::Instance().Drain(2.0);
            AssetManager::Instance().PollHotReload();
            audio.Update();

            g_renderStats.Reset();

            RenderContext ctx;
            ctx.View = camera.GetViewMatrix();
            ctx.Proj = camera.GetProjectionMatrix((float)window.Width() / (float)window.Height());
            ctx.Cam = &camera;
            ctx.Lighting = &scene.Lighting;
            ctx.ScreenWidth = window.Width();
            ctx.ScreenHeight = window.Height();
            ctx.Time = currentFrame;
            ctx.DeltaTime = deltaTime;
            ctx.ShadowCenter = glm::vec3(0.0f);
            ctx.ShadowRadius = 16.0f;
            ctx.ShadowsEnabled = shadowsEnabled;
            ctx.PostEnabled = postEnabled;

            pipeline.Execute(ctx);

            ui.Begin(window.Width(), window.Height());
            scriptUi.DrawAll(ui);
            ui.End();

            if (debugHudVisible) {
                std::vector<DebugLine> lines = {
                    {"FPS: " + std::to_string((int)fps)},
                    {"Objects: " + std::to_string(scene.Objects().size())},
                    {"Post: " + std::string(postEnabled ? "on" : "off")
                         + "  Shadows: " + std::string(shadowsEnabled ? "on" : "off")},
                };
                debugOverlay.Draw(lines, window.Width(), window.Height());
            }

            ++frameCounter;
            if (autoScreenshotFrame >= 0 && frameCounter == autoScreenshotFrame) {
                SaveScreenshot(screenshotPath, window.Width(), window.Height());
                glfwSetWindowShouldClose(window.Handle(), true);
            }

            window.SwapBuffers();
            window.PollEvents();
        }

        // Явный вызов не нужен: EngineRuntime гасит JobSystem и очищает
        // AssetManager в своём деструкторе при выходе из этой области
        // видимости — ПОКА GL-контекст (window) ещё жив.
    } catch (const std::exception& e) {
        LOG_ERROR("Engine") << "Фатальная ошибка: " << e.what();
        std::cerr << "Фатальная ошибка: " << e.what() << std::endl;
        return -1;
    }

    LOG_INFO("Engine") << "Завершение работы";
    return 0;
}
