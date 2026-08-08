#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "sage/core/Layer.h"
#include "sage/core/InputSystem.h"
#include "sage/audio/AudioEngine.h"
#include "sage/render/Shader.h"
#include "sage/render/Camera.h"
#include "sage/render/ShadowMap.h"
#include "sage/render/SkyRenderer.h"
#include "sage/render/Reflection.h"
#include "sage/core/SystemScheduler.h"
#include "sage/render/FrameGraph.h"
#include "sage/render/ParticleSystem.h"
#include "sage/render/PostFX.h"
#include "sage/ui/UIRenderer.h"
#include "sage/ui/UIInteraction.h"
#include "sage/ecs/RenderBatch.h"
#include "sage/scene/Scene.h"
#include "sage/physics/PhysicsScene.h"

class ScriptEngine;

// ---------------------------------------------------------------------------
// PlayerLayer — универсальный рантайм игр, собранных в редакторе SAGE
// (exe SagePlayer). Это «вторая половина» системы сборки игр: редактор
// упаковывает проект (File > Build Game...), плеер его ЗАПУСКАЕТ:
//
//   1. Находит проект: argv[1] -> $SAGE_PROJECT -> ./project -> текущая папка.
//   2. Грузит главную сцену: scenes/main.sage, иначе первая по алфавиту.
//   3. Привязывает Lua-скрипты всех сущностей со ScriptComponent и сразу
//      запускает симуляцию (это игра, а не редактор — Play всегда включён).
//   4. Рендерит от Primary-камеры сцены (CameraComponent) с ПОЛНЫМ освещением
//      движка: hemisphere ambient + солнце + точечные света (LightComponent)
//      + PCF-тени. Нет камеры в сцене — статичная камера-облёт (fallback).
//
// Свои шейдеры плеер грузит ДО перехода в папку проекта (они лежат рядом с
// бинарником), затем меняет CWD на корень проекта — так относительные пути
// сцены («assets/scripts/…», «assets/models/…») резолвятся внутри проекта.
//
// Env-хуки (как у всех приложений движка): SAGE_SCREENSHOT_AT_FRAME/
// SAGE_SCREENSHOT_PATH. ESC — выход.
// ---------------------------------------------------------------------------
class PlayerLayer : public sage::Layer {
public:
    // launchArgs — строка вида "autopilot=1 seed=42" из командной строки и/или
    // SAGE_GAME_ARGS; попадает скриптам как LaunchArg("seed").
    explicit PlayerLayer(std::filesystem::path projectDir, std::string launchArgs = {});
    ~PlayerLayer() override; // unique_ptr<ScriptEngine> требует полный тип в .cpp

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(float dt) override;
    void OnRender() override;

private:
    std::filesystem::path FindMainScene() const;
    void UpdateUiInput(float dt);
    void ResetUiEdits();

    std::filesystem::path m_projectDir;
    std::string m_projectName = "SAGE Game";
    std::string m_launchArgs; // "autopilot=1 seed=42" -> LaunchArg в Lua

    std::unique_ptr<Scene> m_scene;

    // Собирает рантайм под ЗАГРУЖЕННУЮ сцену: скрипты, ввод, звук, физика,
    // состав кадра. Вынесено из OnAttach ради смены сцены: уровень должен
    // подниматься ровно тем же кодом, что и первый, — иначе второй уровень
    // однажды окажется собран чуть иначе, и разница вылезет только у игрока.
    void BuildSceneRuntime();
    // Меняет сцену по ИМЕНИ (без пути и расширения) — то, что просит скрипт
    // через sage.scene.Load. Пустое имя означает перезагрузку текущей.
    bool SwitchScene(const std::string& sceneName);
    // Выполняет то, что игра запросила за кадр (смена сцены, перезапуск,
    // выход). Зовётся МЕЖДУ кадрами, когда ни один скрипт не исполняется.
    void ApplyGameFlowRequests();

    std::filesystem::path m_scenePath;   // что сейчас загружено
    std::unique_ptr<ScriptEngine> m_scripts;
    std::unique_ptr<PhysicsScene> m_physics; // симуляция физики (игра всегда «в Play»)

    // Ввод игры. Действия объявляют САМИ СКРИПТЫ (BindAction из Lua) — плеер
    // не знает и не должен знать раскладку конкретной игры; его дело — раз в
    // кадр опросить устройства и отдать результат скриптам.
    InputSystem m_input;
    std::unique_ptr<WindowRawInput> m_rawInput; // мышь/захват курсора для Lua
    std::unique_ptr<AudioEngine> m_audio;       // звук игры (PlaySound/PlayMusic из Lua)

    std::optional<Shader> m_shader;       // lit: ambient+sun+point lights+тени
    std::optional<Shader> m_shadowShader;
    std::optional<ShadowMap> m_shadows;
    std::optional<SkyRenderer> m_sky;     // процедурный скайбокс сцены
    // Отражения кадра. Карта окружения переснимается только при смене цвета
    // неба (см. ReflectionSystem), поэтому в кадре это стоит ноль.
    sage::render::ReflectionSystem m_reflections;
    sage::render::PlanarReflection m_planar;

    // Состав и порядок кадра — см. sage/core/SystemScheduler.h. Игра может
    // добавить сюда свою систему, не переписывая цикл.
    sage::SystemScheduler m_systems;

    // Кадр как граф проходов (см. sage/render/FrameGraph.h): описание
    // пересобирается каждый кадр, потому что состав зависит от настроек сцены.
    sage::render::FrameGraph m_frame;
    int m_lastFramePasses = -1;   // описание кадра логируется при СМЕНЕ состава
    std::optional<ParticleSystem> m_particles; // пул частиц сцены (эмиттеры ECS)

    // --- Пост-обработка ------------------------------------------------------
    // Сцена рисуется в HDR-буфер, а на экран уходит уже результат цепочки
    // эффектов (bloom/SSAO/DoF/тон-маппинг/FXAA). Раньше рантайм писал прямо в
    // экран и всей пост-обработки в игре просто НЕ БЫЛО: движок её объявлял,
    // конфиг настраивал, окно Game в редакторе показывало — а собранная игра
    // выглядела иначе, потому что в ней этот кусок не выполнялся ни разу.
    //
    // Буфер создаётся лениво и только при cfg.PostProcessing: с выключенным
    // постом полноразмерный HDR-таргет — чистый расход видеопамяти.
    std::optional<Framebuffer> m_sceneFbo;
    std::optional<sage::render::PostFX> m_postfx;
    // UI сцены (UIElementComponent из .sage) — рисуется поверх кадра; лениво,
    // создаётся при первом кадре со сценой, содержащей UI-сущности.
    std::unique_ptr<UIRenderer> m_ui;
    // Ввод для интерфейса сцены: символы и клавиши редактирования копятся
    // колбэками окна между кадрами, мышь опрашивается в OnUpdate. Собирается
    // ЗДЕСЬ, а не в системе UI: та обязана оставаться чистой функцией от
    // состояния ввода, иначе её нельзя ни прогнать в тесте, ни отдать
    // редактору с его пересчитанными в панель координатами.
    sage::ui::UIInputState m_uiInput;
    sage::ui::UIInputResult m_uiResult;
    bool m_uiCallbacksBound = false;
    bool m_uiMouseWasDown = false;
    // Размер области, в которой нарисован интерфейс (letterbox-viewport). Мышь
    // сравнивается именно с ним, иначе клики уезжают на ширину чёрных полос.
    int m_uiWidth = 0, m_uiHeight = 0;
    sage::ecs::RenderBatch m_batch;            // отсечение по фрустуму + инстансинг статики
    Camera m_fallbackCamera; // когда в сцене нет CameraComponent
    bool m_warnedNoCamera = false; // предупреждение «нет Primary-камеры» — один раз

    // ESC удерживается несколько кадров — чтобы одно нажатие не «отпустило
    // курсор и тут же закрыло игру», ждём отпускания клавиши (см. OnUpdate).
    bool m_escLatched = false;
    // Пауза рантайма: мир не тикает, курсор свободен, поверх кадра — меню.
    bool m_paused = false;
    bool m_pauseClickLatched = false;
    void DrawPauseMenu(int vpW, int vpH);

    float m_sceneTime = 0.0f; // секунды с начала игры — uTime шейдеров

    std::string m_screenshotPath = "player.png";
    int m_autoScreenshotFrame = -1;
    int m_frameCounter = 0;
};
