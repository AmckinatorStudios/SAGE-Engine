# SAGE Engine — универсальный модульный 3D-движок (C++ / OpenGL)

> **Статус: в активной разработке, версия `0.3.0`, ещё не 1.0.** Архитектура
> (ECS/RHI/редактор) на месте; боевое тестирование идёт через `games/testgame` —
> реальную игру-стресс-тест (комнаты-сцены, коллизии, подборы, враги, HUD,
> аудио, модели, тени, пост-процесс), которая гоняется в CI каждым коммитом и
> уже вскрыла и помогла починить реальные баги движка (конфликт GLFW-колбэков
> Window/InputSystem, неконструируемый из Lua `Vec3(...)`). До стабильного
> релиза — набрать больше такой практики и стабилизировать API редактора.

**SAGE Engine — универсальный движок общего назначения с ECS, редактором на
ImGui и абстракцией графического бэкенда.** Ядро (окно, рендер, ECS-сцены,
сериализация, скриптинг, аудио) не знает ничего о конкретной игре — оно
одинаково подходит для любой 3D-игры. Проект строго разделён на три части:

- **`engine/`** — переиспользуемое ядро → статическая библиотека `sage::engine`.
  Публичный API отдаётся ТОЛЬКО под префиксом `<sage/...>` — игры и редактор
  физически не могут дотянуться до внутренностей мимо этой границы.
- **`editor/`** — редактор `SageEditor` на ImGui (линкует движок).
- **`games/<name>/`** — конкретные игры (линкуют движок, независимы друг от
  друга). Пример — `games/sandbox/`, минимальный showcase движка (см. ниже).

Добавить новую игру = создать `games/<name>/` и вызвать `sage_add_game()` в её
CMakeLists — движок общий, игровой код независим.

## Архитектура (ключевые решения)

| Слой | Что это | Где |
|---|---|---|
| **Application / Layer** | Движок владеет окном, главным циклом и таймингом; игра/редактор подключаются как слои (`OnAttach/OnUpdate/OnRender`). Точку входа даёт макрос `SAGE_MAIN`. | `sage/core/Application.h`, `Layer.h`, `GameModule.h` |
| **ECS (entt)** | Сцена — это `entt::registry`; сущности собираются из компонентов (`Name`, `Transform`, `MeshRenderer`, `Script`, ...). `GameObject` — дешёвый дескриптор поверх. Системы обходят сущности (`sage/ecs/RenderSystem.h`). | `sage/scene/Scene.h`, `Components.h`, `sage/ecs/` |
| **RHI (абстракция графики)** | Движок обращается не к OpenGL напрямую, а к интерфейсу `GraphicsDevice`. Реализация бэкенда (сейчас OpenGL) изолирована в `engine/src/rhi/opengl/`. Чтобы добавить Vulkan/D3D — реализуют интерфейс, код движка/игр не трогают. | `sage/rhi/GraphicsDevice.h`, `Resources.h`, `engine/src/rhi/opengl/` |
| **Скриптинг (Lua)** | Управление сущностями и логикой уровня из `.lua` без перекомпиляции. | `sage/scripting/ScriptEngine.*` |

## Структура репозитория
```
SAGE-Engine/
  CMakeLists.txt              — оркестратор: сторонние зависимости + подпроекты
  cmake/SageHelpers.cmake     — функция sage_add_game() (новая игра в пару строк)
  cmake/mingw-toolchain.cmake — кросс-сборка .exe из Linux
  .github/workflows/ci.yml    — CI: сборка + unit-тесты (ctest) + smoke (см. ниже)
  tests/                      — модульные тесты (математика/сериализация/анимация)

  engine/                     → библиотека sage::engine (универсальное ядро)
    CMakeLists.txt
    src/sage/                 — ПУБЛИЧНЫЙ API (включается как <sage/...>)
      core/     Application, Layer, GameModule, Window, InputSystem, Log
      rhi/      GraphicsDevice — абстракция графического устройства
      ecs/      Registry (фасад entt), RenderSystem
      scene/    Scene (ECS), Components, Transform, Light, SceneSerializer
      render/   Shader, Camera, Mesh (+примитивы), Model, SkinnedModel, Font,
                Material, Skybox, SkyRenderer (небо), ShadowMap, PostProcess,
                DebugDraw (гизмо/линии в мире), ...
      anim/     Skeleton, Animator, SkinnedModel — скелетная анимация
      ui/       UIRenderer (текст TrueType), Widgets — immediate-mode UI
      physics/  PhysicsWorld (абстракция) — Jolt / Simple / Null бэкенды
      scripting/ ScriptEngine — Lua (sol2)
      audio/    AudioEngine — 2D/3D-звук, музыка (miniaudio)
    src/rhi/opengl/           — OpenGL-бэкенд: ЕДИНСТВЕННОЕ место с glad (уровень устройства)

  runtime/                    → exe SagePlayer — универсальный рантайм игр из
    src/PlayerLayer.*            редактора: грузит проект + главную сцену,
    assets/shaders/              привязывает Lua-скрипты, рендерит с полным
                                 освещением/тенями от Primary-камеры сцены

  editor/                     → exe SageEditor (редактор на ImGui, см. раздел ниже)
    src/EditorLayer.*         — ядро: сцена/проект/Play/undo/рендер превью
    src/EditorHost.h          — контракт операций редактора для панелей
    src/EditorTheme.*         — тема (палитра/метрики/шрифт с кириллицей)
    src/panels/               — панели: Hierarchy, Inspector, Viewport, Game,
                                Console, Assets, Launcher (каждая — свой класс)
    src/RecentProjects.*      — список недавних проектов (~/.config/sage/)
    src/PluginAPI.h, PluginManager.*  — система плагинов редактора (см. ниже)
    plugins/example_stats/    — плагин-пример (собирается в plugins/ рядом с exe)

  games/sandbox/              → exe Sandbox (минимальный showcase движка)
    src/  main.cpp (тонкий), SandboxLayer.*
    assets/ shaders/basic.*, scripts/spin.lua

  games/testgame/             → exe TestGame (боевая игра-стресс-тест, см. ниже)
    src/  main.cpp, TestGameLayer.*, GameComponents.h (кастомные ECS-компоненты)
    assets/ shaders (тени+пост), scripts/*.lua, audio/*.wav, models/monument.obj

  scripts/                    — упаковка релиза + ci_smoke_test.sh (см. ниже)
  external/                   — glad, stb, tinygltf, miniaudio, imgui, imguizmo (вендорены)
```

Сборка даёт: библиотеку `sage::engine`, exe `Sandbox`, exe `TestGame` и exe
редактора `SageEditor`. Граница строгая и проверяемая: код движка нигде не включает
игровой код (см. `## Как разрабатывать движок и игру(ы) параллельно` ниже), а
`#include <glad/...>` на уровне устройства есть только в `engine/src/rhi/opengl/`.
Следующую игру можно собрать, слинковав ту же библиотеку и написав свой слой +
`CreateApplication`.

## Как сделать новую игру
```cmake
# games/mygame/CMakeLists.txt
sage_add_game(
    NAME MyGame
    SOURCES src/main.cpp src/MyGameLayer.cpp
    ASSETS  ${CMAKE_CURRENT_SOURCE_DIR}/assets
)
```
```cpp
// games/mygame/src/main.cpp
#include "sage/core/GameModule.h"
#include "MyGameLayer.h"
sage::Application* sage::CreateApplication(int, char**) {
    auto* app = new sage::Application({.Title = "My Game"});
    app->PushLayer(std::make_unique<MyGameLayer>());
    return app;
}
SAGE_MAIN()
```
Затем добавить `add_subdirectory(games/mygame)` в корневой CMakeLists. Игровой
слой наследует `sage::Layer` и работает с движковой `Scene` (ECS), рендером
через RHI-девайс и т.д. — именно так устроен `games/sandbox/src/SandboxLayer.*`,
самый простой рабочий пример, с которого можно скопировать структуру.

## games/sandbox — минимальный showcase движка
Это не игра, а нарочно упрощённый пример: плоскость-пол + три цветных куба
через ECS (`Scene::CreateObject`/`MeshRendererComponent`) + один куб со
`ScriptComponent` (крутится через `assets/scripts/spin.lua`, доказывая, что
скриптинг работает и вне редактора), авто-облёт камеры, один простой
шейдер с направленным светом (`assets/shaders/basic.*`). Никаких
теней/пост-процесса/аудио/UI — это подсистемы движка, которые игра включает
по мере необходимости (см. соответствующие разделы ниже), не обязательная
часть минимального примера.

`Sandbox` служит и референсом «как сделать игру», и запускаемой fixture для
headless smoke-тестов в CI (сама библиотека `sage_engine` не запускается —
нужен exe). Поддерживает те же env-хуки, что и `SageEditor`:
`SAGE_WINDOW_WIDTH`/`SAGE_WINDOW_HEIGHT`, `SAGE_SCREENSHOT_AT_FRAME`/
`SAGE_SCREENSHOT_PATH`.

## games/testgame — боевая игра-стресс-тест движка

Не игра ради игры, а полигон: маленький, но ПОЛНЫЙ игровой цикл, сознательно
нагружающий каждую подсистему движка (после удаления The Boat у многих из них
не оставалось ни одного реального потребителя — «компилируется» не значит
«работает»). Что внутри и что этим проверяется:

- **Игрок от первого лица** — `InputSystem` с именованными действиями
  (WASD/мышь/ESC) + **своя AABB-коллизия** со стенами в коде игры
  (`MovePlayer`): коллизии — забота игры, движок их не навязывает.
- **Две комнаты-сцены** (`SceneManager`) с переходом по пульсирующему
  порталу: реальная проверка мультисценового API и переноса состояния
  (здоровье игрока переезжает между комнатами).
- **Кастомные ECS-компоненты** (`src/GameComponents.h`: Health, Pickup,
  Patrol, StaticCollider, Portal) — навешиваются через
  `scene.Registry().emplace<T>()` и обходятся обычными entt-view в коде игры.
  Это ОФИЦИАЛЬНЫЙ паттерн расширения: движок не правится. (Ограничение:
  `SceneSerializer` кастомные компоненты не сохраняет — см. «Известные
  ограничения».)
- **Десятки Lua-скриптов одновременно** (монетки, маяки порталов) — стресс
  `ScriptEngine`; сериализация сцены проверяется round-trip-тестом на старте.
- **HUD движковым `UIRenderer`/`Widgets`** — health bar с динамическим
  источником значения, счёт, подсказки.
- **Аудио** (`AudioEngine`): эмбиент-луп + эффекты подбора/урона/портала; в
  headless CI тихо деградирует (нет устройства — no-op, не падает).
- **Модель `.obj` двумя путями**: ECS-энтити через `ResourceManager::GetModel`
  (комната 1) и прямой `Model::Load`+`Model::Draw` (монумент в комнате 2).
- **Тени** (`ShadowMap` + PCF) и **пост-процесс** (HDR `Framebuffer` +
  ACES-тон-маппинг) — первый реальный рендер-прогон обеих подсистем после
  RHI-миграции.

Env-хуки: общие `SAGE_SCREENSHOT_*`, плюс `SAGE_NO_SHADOWS`, `SAGE_NO_POST`,
`SAGE_TESTGAME_START_ROOM=room2` (старт в конкретной комнате для отладки) и
`SAGE_TESTGAME_AUTOPILOT=1` — бот сам собирает монеты и проходит портал; CI
гоняет его каждым коммитом и проверяет лог-маркеры `TESTGAME:` + отсутствие
ERROR-строк (см. `scripts/ci_smoke_test.sh`, тест 4/4).

Уже вскрытые этой игрой и починенные баги движка:
1. `Window` и `InputSystem` оба монопольно занимали GLFW user pointer окна —
   resize с активным вводом кастовал чужой указатель (UB). Теперь колбэками
   владеет `Window`, ввод подписывается через его хуки.
2. `Vec3(...)`/`Vec2(...)`/`Vec4(...)` нельзя было сконструировать из Lua
   (sol2 требует отдельной регистрации `call_constructor`) — ни один прежний
   скрипт векторы не создавал, поэтому баг жил незамеченным.

## Редактор SageEditor
Полноценный редактор сцен на ImGui (docking + multi-viewport) + ImGuizmo.

**Архитектура (v3, модульная)**: ядро `EditorLayer` (сцена/проект/Play/undo/
рендер превью) реализует контракт `EditorHost` (`editor/src/EditorHost.h`), а
каждая панель — независимый класс в `editor/src/panels/` со СВОИМ
UI-состоянием, зависящий только от контракта: Hierarchy, Inspector, Viewport,
Game, Console, Assets, Launcher. Новая панель = новый файл + вызов `Draw()` в
кадре — в ядро врастать не нужно. Тема — `EditorTheme` (единая палитра/метрики).

- **Доккинг + отдельные окна**: раскладка по умолчанию строится автоматически
  (Hierarchy слева, Inspector/Lighting справа, Console/Assets табами снизу,
  Viewport+Game табами в центре); панели свободно перетаскиваются, стыкуются и
  **вытаскиваются в отдельные OS-окна** (multi-viewport), раскладка
  сохраняется между запусками (`sage_editor_imgui.ini`), Window > Reset Layout
  возвращает дефолт.
- **Тулбар** — полоса под меню-баром (не оверлей во вьюпорте): слева режим
  гизмо (Move/Rotate/Scale), snap, пространство (Local/World); по центру
  Play/Pause/Stop; справа режим рендера и сетка. Внизу — статус-бар: проект |
  сцена (+`*` при несохранённых правках — он же в заголовке OS-окна) |
  сущности | Play-статус | сообщения плагинов | FPS.
- **Режимы рендера вьюпорта** (тулбар / env `SAGE_EDITOR_RENDER_MODE`):
  **Shaded** (полное освещение), **Wireframe** (каркас — движковый
  `PolygonMode::Line`), **Unlit** (плоский цвет), **Normals** (визуализация
  нормалей). Игровое окно (Game) всегда Shaded.
- **Гизмо и выделение**: выбранная сущность обведена **аутлайном**
  (масштабированная оболочка) + оси; **невидимые сущности видны и кликабельны**
  — у камеры рисуется каркас пирамиды видимости (frustum), у света — маркер и
  зона действия (сфера/конус). Клик по гизмо камеры/света выбирает сущность.
  Сетка вьюпорта — 3D-линии `DebugDraw` (заслоняются геометрией).
- **Игровое окно (Game)**: рендер сцены от ИГРОВОЙ камеры — первой сущности с
  `CameraComponent.Primary` (движковый компонент, сериализуется в `.sage`;
  Inspector > Camera — Add/Remove, FOV/Near/Far/Primary). При нажатии Play таб
  Game автоматически выходит на передний план — редактируешь в Viewport,
  играешь в Game. Демо-сцена сразу содержит Main Camera.
- **Стартовый Project Launcher**: пока проект не открыт — окно с недавними
  проектами (хранятся в `~/.config/sage/recent_projects.json`), формой
  создания нового и открытием по пути; открытие проекта автоматически грузит
  его первую сцену. File > Project Launcher... открывает hub повторно; File >
  Project Scenes — быстрый доступ ко всем сценам открытого проекта.
- **Полное освещение в редакторе (PBR)**: Viewport и Game рендерятся физически-
  корректным (metallic-roughness, Cook-Torrance GGX) forward-шейдером движка.
  Единый GLSL-блок освещения (`PbrShader.h`, `kPbrSharedGlsl`) встроен во ВСЕ
  пути — инстансный статический, текстурный (нормал-маппинг) и скиннинг — так
  статика, текстурированные и анимированные меши освещаются одинаково. Все
  стандартные типы света для forward-рендера:
  - **Ambient** (полусферический): SkyColor тонирует верхние грани, GroundColor
    — нижние (не плоская засветка). **Если включён скайбокс — засветка берётся
    ИЗ НЕГО** (верх тянется к цвету зенита/неба, низ — к приглушённому горизонту),
    так что освещение согласовано с видимым небом (см. `LightingEnvironment::
    ResolveAmbient`).
  - **Directional** («солнце»): один на сцену, отбрасывает **PCF-тени** (общий
    shadow-проход на кадр). Тени — только от солнца.
  - **Point** (точечный): лампа/факел, светит во все стороны, затухание из Range.
  - **Spot** (прожектор): конусный свет вдоль «вперёд» сущности (-Z её поворота),
    внутренний/внешний угол для мягкого края — фонарик, лампа-спот.

  Ambient и солнце — окружение сцены (панель **Lighting**, сериализуются, с undo).
  Точечные света и прожекторы — СУЩНОСТИ с движковым `LightComponent`
  (`Type` Point/Spot, Color/Intensity/Range + Inner/OuterCone у spot; позиция и
  направление из Transform — двигаются гизмо и скриптами): Entity > Create Light,
  секция Light в Inspector (переключение типа), у выбранного света рисуется зона
  действия (сфера радиуса — point, конус — spot). Итоговое освещение кадра
  собирает `sage::ecs::CollectLighting` (окружение + света-сущности, лимит
  шейдера — 8 точечных + 8 прожекторов).
- **Атмосфера сцены** (панель Lighting, сериализуется): **скайбокс** —
  процедурный градиент неба (`SkyRenderer`, зенит → горизонт, без ассетов) и
  **линейный туман** (цвет + start/end, применяется в Shaded). Оба поля —
  часть окружения сцены, работают и в редакторе, и в собранной игре.
- **Примитивы**: движок генерирует Cube/Sphere/Plane/Cylinder/Cone
  (`Mesh::Create*`, с нормалями и UV; кэш `ResourceManager::GetPrimitive`).
  Entity > Create Primitive, выбор в Inspector (Mesh), сериализация в `.sage`.
- **Сборка игры (File > Build Game...)**: упаковывает открытый проект в
  ЗАПУСКАЕМУЮ игру — папка `<out>/<Имя>/` с копией рантайма `SagePlayer`
  (переименован в имя проекта), его шейдерами и `project/` (сцены/ассеты).
  Собранная игра самодостаточна: запускается двойным кликом, без движка и
  редактора. Проверяется smoke-тестом 5/5 в CI — собранная self-test-игра
  реально запускается headless.
- **Гизмо** (ImGuizmo): перемещение/поворот/масштаб выбранной сущности прямо во
  вьюпорте. Горячие клавиши **W/E/R**, привязка к сетке — галка Snap. Выбранная
  сущность дополнительно подсвечивается каркасом и осями X/Y/Z, а сетка-
  ориентир рисуется движковым `DebugDraw` (3D-линии С ТЕСТОМ ГЛУБИНЫ — объекты
  сцены её заслоняют; раньше сетка была 2D-оверлеем и просвечивала сквозь всё).
- **Камера вьюпорта**: ПКМ — осмотреться, ПКМ+WASD (Q/E — вниз/вверх, Shift —
  быстрее) — полёт, колесо — наезд.
- **Пикинг**: клик ЛКМ по объекту выбирает его (луч в ECS-сцену), клик в пустоту
  снимает выбор.
- **Проекты**: File > New Project создаёт `<папка>/<имя>/` с `project.sageproj`,
  `scenes/` и `assets/`; Open Project открывает существующий. Сцены сохраняются
  в `scenes/` проекта (`.sage` JSON) и открываются двойным кликом в Assets.
- **Панели**: Hierarchy (создание/дублирование/удаление сущностей, контекстное
  меню), Inspector (имя, Transform, цвет, выбор меша Cube/Model, материал,
  скрипт), Console (живой лог движка с цветами уровней), Assets (сетка цветных
  тайлов по типу файла — папка/сцена/материал/скрипт/меш/текстура/аудио/шейдер —
  с breadcrumb, поиском по имени и rename/delete через ПКМ; двойной клик по
  `.sage` грузит сцену). ПКМ по пустому месту Assets создаёт ассеты: **New
  Folder / New Script (.lua с шаблоном OnStart/OnUpdate) / New Text File /
  New Material (.sagemat)**.
- **Материалы (PBR)**: `.sagemat` (JSON: albedo/emissive/**metallic/roughness**/
  **texture (albedo-карта)**/**normalMap (tangent-space)**/shininess-legacy) —
  переиспользуемое описание внешнего вида в metallic-roughness workflow. Клик по
  `.sagemat` в Assets открывает его редактор в Inspector (слайдеры Metallic/
  Roughness, поля Albedo/Normal Map; правки видны на всех сущностях с этим
  материалом сразу — экземпляр разделяемый, Save пишет на диск); у выбранной
  сущности секция Material — Assign/Clear. Материал с назначенными картами
  рисуется отдельным текстурным PBR-путём (TBN, нормал-маппинг); без карт —
  быстрым инстансным путём с metallic/roughness из материала. Назначенный материал заменяет Color
  сущности (`EffectiveColor` в `sage/scene/Components.h`), путь сохраняется в
  `.sage` и восстанавливается при загрузке.
- **Play-режим**: кнопка Play в тулбаре вьюпорта (или меню Play). При входе
  сцена снапшотится, к сущностям со Script-компонентом привязываются их `.lua`
  (`OnStart`/`OnUpdate` — тот же ScriptEngine, что в играх) и тикают каждый
  кадр; Pause замораживает, Stop откатывает сцену ровно к состоянию до Play.
  Скрипт назначается в Inspector (секция Script, пример: `assets/scripts/spin.lua`).
- **Undo/Redo** (Ctrl+Z / Ctrl+Y, меню Edit): откатывает создание/удаление/
  дублирование сущностей, перетаскивания гизмо и правки в Inspector (одна
  запись на завершённое перетаскивание/правку). До 100 шагов истории.
- Горячие клавиши: Ctrl+S — сохранить сцену, Ctrl+D — дублировать, Del — удалить,
  Ctrl+Z/Ctrl+Y (или Ctrl+Shift+Z) — undo/redo, W/E/R — режим гизмо.
- CI-хуки: `SAGE_SCREENSHOT_AT_FRAME`/`SAGE_SCREENSHOT_PATH` — авто-скриншот,
  `SAGE_EDITOR_SELFTEST=1` — headless-проверка «проект + сцена + undo/redo +
  Play» (пишет `SELFTEST: PASS/FAIL` в лог), `SAGE_EDITOR_AUTOPLAY=1` —
  авто-вход в Play при старте (для визуальных проверок).

## Производительность: отсечение по фрустуму + инстансный батчинг
Статические меши сцены рисуются через `sage::ecs::RenderBatch`
(`engine/src/sage/ecs/RenderBatch.*`) — так стоимость кадра зависит от ВИДИМЫХ
объектов и числа РАЗНЫХ мешей, а не от общего числа сущностей:

- **Отсечение по фрустуму.** Из матрицы `proj*view` извлекаются 6 плоскостей
  (`sage/render/Frustum.h`, метод Gribb–Hartmann); у каждого меша есть локальная
  ограничивающая сфера (`Mesh::BoundsCenter/BoundsRadius`, считается при
  создании), переводимая в мир по `Transform`. Сущности, чья сфера целиком вне
  фрустума, не рисуются. В depth-проходе теней отсечение идёт по фрустуму СВЕТА.
- **Инстансный батчинг.** Видимые сущности группируются по мешу и рисуются
  ОДНИМ инстансным вызовом на группу: `Mesh` несёт per-instance поток (модельная
  матрица + цвет + metallic/roughness, loc 4..10; per-vertex — position/normal/
  uv/**tangent**, loc 0..3), заливаемый раз в кадр, и `DrawInstances` →
  `glDrawElementsInstanced`. N одинаковых кубов = 1 draw call вместо N.

Освещение инстансного прохода — тот же общий PBR-блок (`kPbrSharedGlsl`,
Cook-Torrance), что и у текстурного/скиннинг-путей (ambient/солнце с тенями/
точечные/прожекторы/туман). Подключено в редакторе (Viewport и Game),
`SagePlayer` и `games/testgame`. Статистика (`RenderStats`: нарисовано/отсечено/
батчей) видна в статус-баре редактора; в testgame есть стресс-хук
`SAGE_TESTGAME_STRESS=N` (рассыпает N×N кубов — почти все отсекаются/батчатся).
Корректность отсечения и согласованность батч-статистики проверяются
editor self-test'ом.

## Гизмо и отладочная графика (DebugDraw)

`sage/render/DebugDraw.h` — движковая система гизмо: батч 3D-линий, рисуемых
В МИРЕ с тестом глубины (не 2D-оверлей). Доступна и редактору, и играм:

```cpp
DebugDraw dbg; // шейдер встроен, внешних ассетов не требует
// каждый кадр, после отрисовки сцены в тот же буфер:
dbg.Grid({0,0,0}, 12.0f, 1.0f, {0.35f, 0.35f, 0.4f}); // сетка XZ, оси X/Z подсвечены
dbg.WireBox(transform.GetMatrix(), {1.0f, 0.6f, 0.1f}); // каркас (подсветка выбора/AABB)
dbg.WireSphere(center, radius, {0.2f, 0.8f, 0.3f});
dbg.Axes(transform.GetMatrix(), 1.5f);                  // оси: X-красная, Y-зелёная, Z-синяя
dbg.Line(from, to, color);
dbg.Flush(view, proj); // один draw call на весь батч
```

Линии проходят тест глубины (геометрия сцены их заслоняет), но глубину не
пишут. Редактор рисует этим сетку вьюпорта и подсветку выбранной сущности;
играм это же API годится для отладочных AABB/трасс/направлений.

## Плагины редактора

> **По умолчанию ОТКЛЮЧЕНЫ** (экспериментальны — нестабильный ABI). Редактор не
> грузит плагины, пока не задана переменная окружения `SAGE_EDITOR_PLUGINS=1`.
> Без неё каталог `plugins/` игнорируется. Всё описанное ниже работает только
> при явном включении.

**v1: только редактор** (панели/инструменты) — плагины НЕ участвуют в рантайме
игр и не грузятся ни во что, кроме `SageEditor`. Без гарантий бинарной
совместимости между версиями движка/редактора: плагин пересобирается вместе с
редактором (собирается тем же CMake-проектом, тем же компилятором).

- Плагин — динамическая библиотека (`.so`/`.dll`), которую `PluginManager`
  грузит при старте редактора из каталога `plugins/` рядом с бинарником
  `SageEditor` (переопределяется переменной окружения `SAGE_PLUGINS_DIR`, тот
  же паттерн, что у `SAGE_SCREENSHOT_*`/`SAGE_EDITOR_SELFTEST`).
- Контракт — `editor/src/PluginAPI.h`: класс `SageEditorPlugin` с
  `Name()/OnLoad()/OnUnload()/OnUpdate(dt)/OnImGui()` (в `OnImGui()` плагин
  рисует свои `ImGui::Begin(...)` панели/пункты меню как обычный код
  редактора). `OnLoad` получает `EditorPluginContext&` — узкий facade
  (`Log`, `SelectedEntityName`, `SetStatusMessage`), **сознательно НЕ**
  пробрасывающий `Scene&`/`entt::registry&` через границу библиотеки (разные
  версии компилятора/STL у хоста и плагина делают это ABI-небезопасным);
  facade расширяется по мере появления реальных плагинов, не заранее.
- Плагин экспортирует две `extern "C"` функции:
  ```cpp
  extern "C" SageEditorPlugin* CreateSageEditorPlugin() { return new MyPlugin(); }
  extern "C" void DestroySageEditorPlugin(SageEditorPlugin* p) { delete p; }
  ```
- Сборка — `sage_add_editor_plugin(NAME <lib> SOURCES <files...>)` в
  `cmake/SageHelpers.cmake` (по образцу `editor/plugins/example_stats/`):
  собирает `MODULE`-библиотеку и копирует её в `plugins/` рядом с
  `SageEditor`. Плагин **не линкует** ни `sage::engine`, ни статическую
  `imgui` — только заголовки (ImGui хранит состояние `GImGui` в статической
  переменной; если плагин слинкует свою копию `libimgui.a`, он получит
  отдельный `GImGui` и упадёт на первом же `ImGui::Begin()`). Вместо этого
  вызовы `ImGui::` остаются неразрешёнными символами в библиотеке плагина и
  резолвятся в рантайме против уже загруженного ImGui хоста: на Linux/macOS —
  автоматически (dlopen против процесса, `SageEditor` собран с
  `ENABLE_EXPORTS`), на Windows — линковкой на сгенерированную
  import-библиотеку `SageEditor.exe` (`--export-all-symbols`).
- Пример: `editor/plugins/example_stats/` — панель с графиком времени кадра
  и именем выбранной сущности, показывающая полный цикл `OnLoad -> OnImGui`
  каждый кадр `-> OnUnload`.

## Как добавить графический бэкенд
Реализовать интерфейсы `sage::rhi::GraphicsDevice` (устройство: инициализация,
состояние конвейера, чтение пикселей) и ресурсы из `sage/rhi/Resources.h`
(`ShaderProgram`, `Geometry`, `Texture2D`, `TextureCube`, `RenderTarget`) в новом
каталоге `engine/src/rhi/<backend>/`, добавить ветку в фабрику
`GraphicsDevice::Create()`. Код движка и игр обращается ТОЛЬКО к этим
интерфейсам: `glad`/GL-вызовы существуют исключительно в `engine/src/rhi/opengl/`
(инвариант проверяется grep'ом — см. ниже), поэтому смена бэкенда не затрагивает
ни движок, ни игры, ни редактор.

## Скелетная анимация
Движок умеет **скелетную анимацию** (skinning) — деформацию меша скелетом
костей. Данные независимы от GPU и живут в `sage/anim/`:

- `Skeleton` — кости (TRS + родитель + обратная bind-матрица);
- `AnimationClip` — каналы (перенос/поворот/масштаб кости) с ключами и
  интерполяцией (линейная со сферической для поворотов / STEP);
- `Animator` — проигрыватель: сэмплирует клип, считает глобальные матрицы
  костей и выдаёт **палитру** (`global · inverseBind`) для скиннинг-шейдера.

Геометрия и рендер — в `sage/render/SkinnedModel.*`: вершина несёт до 4 костей
с весами; скиннинг-шейдер считает позицию/нормаль как взвешенную сумму костей на
GPU. Его **фрагментная стадия использует тот же общий PBR-блок**
(`kPbrSharedGlsl`, Cook-Torrance metallic-roughness) — ambient (в т.ч. из
скайбокса) + солнце с PCF-тенями + точечные + прожекторы + туман, — так что
анимированные и статичные меши освещаются ФИЗИЧЕСКИ ОДИНАКОВО (metallic/roughness
берутся из glTF-материала submesh'а). Скелетные модели
также **отбрасывают тень**: у карты теней есть отдельный depth-проход со
скиннингом (`SkinnedModel::DrawDepth` / `DrawAnimatedModelsDepth`), поэтому
анимированный меш и получает, и отбрасывает тень в текущей позе. Модель —
разделяемый ассет (скелет/клипы), а покадровое состояние держит `Animator` у
каждой сущности.

**Загрузка.** `SkinnedModel::Load("hero.glb")` читает из glTF/GLB скин
(JOINTS_0/WEIGHTS_0, inverseBindMatrices) и анимации (каналы/сэмплеры) —
стандартный формат экспорта из Blender/Maya. Без внешнего ассета есть
процедурная демо-модель `SkinnedModel::CreateDemoTentacle()` (гибкий «щупалец»
с клипом Wave) — на ней держатся примеры и headless-тесты.

**В ECS/редакторе.** Повесь `AnimatedModelComponent` (поле `Path` — файл .glb,
пустое = процедурное демо; `Clip`/`Speed`/`Loop`/`Playing`). Система
`sage::anim::UpdateAnimators`/`DrawAnimatedModels` (`sage/anim/AnimationSystem.h`)
инициализирует, продвигает и рисует все такие сущности — вызывается редактором
(анимация видна в вьюпорте, секция **Animated Model** в Inspector, меню
**Entity → Create Animated Model**) и рантаймом `SagePlayer`. Описательные поля
сериализуются в `.sage`. Живой пример — «тотем» в `games/testgame` (комната 1).
Ограничения: до 128 костей на модель (`kMaxBones`), морф-таргеты и IK не
поддерживаются.

## Физика
Физика построена по тому же принципу, что и графика (RHI): **подключаемая
абстракция** `sage::physics::PhysicsWorld` (заголовок `sage/physics/
PhysicsWorld.h`) — движок, редактор и игры создают тела и шагают симуляцию
через этот интерфейс, не завися от конкретной библиотеки. Конкретные бэкенды
живут в `engine/src/physics/<backend>/` и выбираются фабрикой
`PhysicsWorld::Create(Backend)`:

- **Jolt** (`Backend::Jolt`) — основной, «взрослый» бэкенд поверх
  [jrouwe/JoltPhysics](https://github.com/jrouwe/JoltPhysics): полная динамика
  твёрдых тел с вращением, честные контакты, формы Box/Sphere/Capsule.
  Подтягивается через FetchContent (тег `v5.3.0`), включён по умолчанию
  (`-DSAGE_PHYSICS_JOLT=ON`). Это бэкенд по умолчанию (`DefaultBackend()`),
  когда собран.
- **Simple** (`Backend::Simple`) — встроенный лёгкий интегратор без внешних
  зависимостей: гравитация + столкновение динамики со статикой как AABB,
  упругость/трение, без вращательной динамики. Доступен всегда — на нём
  движок собирается и работает даже с `-DSAGE_PHYSICS_JOLT=OFF` (быстрая
  сборка, окружения без сети). Fallback, если Jolt не скомпилирован.
- **Null** (`Backend::Null`) — заглушка (физика отключена, `IsAvailable()` =
  `false`).

**Как пользоваться в игре/редакторе.** Повесь на сущность два компонента:
`RigidBodyComponent` (тип тела Static/Dynamic/Kinematic + масса/трение/
упругость) и `ColliderComponent` (форма Box/Sphere/Capsule + размеры;
домножаются на `Transform.Scale`). Мост `PhysicsScene` (`sage/physics/
PhysicsScene.h`) строит физический мир по всем таким сущностям, а его `Step()`
каждый кадр синхронизирует: Dynamic — тело → `Transform`, Kinematic —
`Transform` → тело, Static — неподвижно. В редакторе это работает в Play-режиме
(секции Rigid Body / Collider в Inspector, каркас коллайдера в вьюпорте, пункт
меню **Entity → Create Physics Cube**); в собранной игре (`SagePlayer`) — сразу
при запуске; живой пример в `games/testgame` (комната 1 роняет башенку ящиков).
Компоненты сериализуются в `.sage`. Env-переопределения бэкенда не требуется —
`RigidBody`/`Collider` одинаковы для всех бэкендов.

**Как добавить свой бэкенд физики.** Реализуй интерфейс `PhysicsWorld` в новом
каталоге `engine/src/physics/<backend>/`, добавь значение в `enum class Backend`
и ветку в `PhysicsWorld::Create()`. Код игр/редактора не меняется — это и есть
требование «подключать разные библиотеки».

## Система сцен и сериализация
`Scene` — это ECS-сцена на `entt::registry`. Сущности собираются из компонентов
(`NameComponent`, `Transform`, `MeshRendererComponent`, `ScriptComponent`,
`CameraComponent`, ...);
`GameObject` — дешёвый дескриптор `{registry, entity}` с аксессорами к
компонентам. `SceneManager` держит несколько сцен и переключает активную:

```cpp
SceneManager sceneManager;
Scene& level1 = sceneManager.CreateScene("Level1");
GameObject obj = level1.CreateObject("MyCube");
obj.GetTransform().Position = {0.0f, 1.0f, 0.0f};
obj.Renderer().Ref = MeshRef{MeshRef::Type::Cube, ""};
obj.Renderer().MeshPtr = ResourceManager::Instance().GetCube();

// Сохранить сцену на диск (человекочитаемый JSON, расширение .sage)
sceneManager.SaveScene("Level1", "assets/scenes/level1.sage");

// Загрузить сцену обратно (сам пересоздаст GPU-ресурсы мешей)
sceneManager.LoadScene("assets/scenes/level1.sage");
```

Обход сущностей — через ECS-view (`scene.Registry().view<...>()`) или готовую
систему `sage::ecs::ForEachRenderable(scene, ...)`. Для снапшотов в памяти
(undo/redo, Play-режим) есть строковые варианты — `SceneSerializer::SaveToString`/
`LoadFromString`, тот же JSON, без обращения к диску.

Файл `.sage` — обычный JSON, можно открыть и посмотреть/поправить руками:
```json
{
  "sage_scene_version": 1,
  "name": "Level1",
  "objects": [
    {
      "id": 1, "name": "Cube1",
      "position": {"x":0,"y":0,"z":0},
      "rotation": {"x":0,"y":0,"z":0},
      "scale":    {"x":1,"y":1,"z":1},
      "color":    {"x":0.8,"y":0.3,"z":0.3},
      "mesh": {"type":"cube","path":""}
    }
  ]
}
```

Для модели вместо куба: `"mesh": {"type":"model","path":"assets/models/dragon.obj"}`.
Сущность со скриптом добавляет поле `"script": "assets/scripts/spin.lua"`.

## Скриптинг (Lua)
Часть ЯДРА движка (`engine/src/sage/scripting/ScriptEngine.*`) — не зависит ни
от какой конкретной игры. Позволяет управлять поведением сущностей через
`.lua` файлы без перекомпиляции движка: правишь скрипт, перезапускаешь игру,
видишь результат.

```cpp
ScriptEngine scriptEngine;
scriptEngine.BindScene(scene);
scriptEngine.AttachScript(myGameObject, "assets/scripts/spin.lua");

// каждый кадр:
scriptEngine.UpdateAll(deltaTime);
```

Скрипт получает объект как `entity` и может читать/писать его Transform и цвет:
```lua
-- assets/scripts/spin.lua
function OnStart(entity)          -- необязательно, вызывается один раз
    log("Скрипт запущен для: " .. entity.Name)
end

function OnUpdate(entity, dt)     -- обязательно, вызывается каждый кадр
    entity.Transform.Rotation.y = entity.Transform.Rotation.y + dt * 45.0
end
```

`games/sandbox` показывает это на практике: один куб со `ScriptComponent`
крутится через `spin.lua`, привязанный напрямую через `ScriptEngine` (без
редактора) — доказательство, что скриптинг (и вся ECS-сцена) общая часть
движка, а не что-то специфичное для конкретной игры или для `SageEditor`.

Что уже доступно скриптам из коробки: `entity.Name`, `entity.Transform`
(`.Position`/`.Rotation`/`.Scale`, каждое `Vec3` с полями `.x/.y/.z`),
`entity.Color`, функции `SpawnObject`/`FindObject`/`DestroyObject`,
`Schedule`/`Repeat`/`StartCoroutine`, доступ к камере/частицам/билбордам/аудио
(после соответствующих `Bind*`), функция `log(строка)`. Чтобы добавить свою
API-функцию — смотри `ScriptEngine::RegisterEngineApi()`, там пара строк на
sol2 на каждую привязку.

## Текстуры и модели
Часть ЯДРА движка (`engine/src/sage/render/Texture.*`, через stb_image) —
грузит PNG/JPG/BMP/TGA в GPU-текстуру через текущий RHI-бэкенд:

```cpp
Texture myTexture("assets/textures/что-то.png", TextureFilter::Trilinear);
myTexture.Bind(0);
shader.SetInt("uTexture", 0);
shader.SetInt("uUseTexture", 1); // если 0 — рендер работает по-старому, только цветом
```

`TextureFilter` — 4 режима, выбираются явно при загрузке:

| Режим | Когда использовать |
|---|---|
| `Nearest` | Пиксельная чёткость без сглаживания — текстурные атласы, pixel-art |
| `Bilinear` | Простое сглаживание без перехода между уровнями мипмапов |
| `Trilinear` | Сглаживание + плавный переход между мипмапами — стандартный выбор по умолчанию |
| `Anisotropic` | Как Trilinear, плюс убирает размытость на поверхностях под острым углом к камере (типично — земля/пол, если смотреть почти вдоль неё) |

Уровень анизотропии запрашивается автоматически: движок узнаёт максимум,
который поддерживает видеокарта (`Texture::MaxSupportedAnisotropy()`), и
использует `min(4.0, максимум)`. `Anisotropic`/`Trilinear` требуют мипмапов и
включают их автоматически. Для атласов (несколько текстур в одном файле,
разные UV-прямоугольники на разные части меша) мипмапы нужно отключать
(`generateMipmaps=false`) вместе с `Nearest` — иначе на дальних объектах
мипмапы «усредняют» цвет с соседними тайлами атласа (протекание цвета на
границах).

**Загрузка 3D-моделей** (`engine/src/sage/render/Model.*`) — форматы OBJ+MTL
(через tinyobjloader) и GLTF/GLB (через tinygltf, включая встроенные как
base64 текстуры), автоопределение по расширению файла:

```cpp
auto model = Model::Load("assets/models/dragon.glb"); // или .obj, или .gltf
shader.Use();
shader.SetMat4("uModel", transformMatrix);
model->Draw(shader); // сама переключает текстуру/цвет между подмешами (submesh)
```

Известные ограничения: только `pbrMetallicRoughness.baseColorTexture` (normal
maps/metallic-roughness/emissive пока не читаются), OBJ без переиспользования
вершин. Это путь для СТАТИЧЕСКОЙ геометрии (`Model`); скелетные меши с анимацией
из glTF грузит отдельный путь `SkinnedModel::Load` — см. `## Скелетная анимация`.

## Skybox
Часть ЯДРА движка (`engine/src/sage/render/Skybox.*`) — кубическая (cubemap)
текстура, создающая иллюзию бескрайнего неба вокруг сцены:

```cpp
Skybox skybox({
    "assets/textures/skybox/px.png", "assets/textures/skybox/nx.png",
    "assets/textures/skybox/py.png", "assets/textures/skybox/ny.png",
    "assets/textures/skybox/pz.png", "assets/textures/skybox/nz.png"
});
// каждый кадр, ПЕРВЫМ (до остальной сцены):
skybox.Draw(skyboxShader, view, projection);
```

Технически: из view-матрицы перед отрисовкой убирается позиция камеры
(скайбокс реагирует на поворот, но не двигается при перемещении); вершинный
шейдер даёт глубину 1.0 через `pos.xyww`, что вместе с `DepthFunc::LessEqual`
гарантирует, что скайбокс всегда позади сцены независимо от порядка отрисовки;
backface culling на время отрисовки выключается (куб рисуется изнутри).
`GL_TEXTURE_CUBE_MAP_SEAMLESS` включается один раз при инициализации RHI-девайса
— без этого на стыках граней cubemap виден шов.

## Частицы (billboard-эффекты)
Система частиц (`engine/src/sage/render/ParticleSystem.*`) — инстансные
billboard-частицы (всегда развёрнуты к камере) для огня/дыма/искр/всплесков.
Один draw call на все живые частицы; встроенный шейдер (мягкий круг/квад) —
частицы рисует кто угодно без ассетных шейдеров:

```cpp
particles.Update(dt);
particles.DrawFromView(view, proj); // camRight/Up берутся из матрицы вида
```

**В ECS/редакторе.** Повесь `ParticleEmitterComponent` (см. `## ECS`): готовый
пресет (`ParticlePresets::Registry` — Fire/Smoke/Sparks/Water Splash/Embers/
Block Break) плюс тонкая настройка (скорость/гравитация/время жизни/размер/цвет/
разлёт/эмиссия). `Continuous` — непрерывная струя (частиц/сек), иначе —
периодические залпы. Система `sage::fx::UpdateEmitters` (`ParticleECS.h`) рождает
частицы в позиции `Transform` каждого эмиттера в общий пул, которым владеет слой
(редактор/рантайм/игра) и рисует одним вызовом. Параметры сериализуются в
`.sage`. В редакторе: секция **Particle Emitter** в Inspector, меню **Entity →
Create Particle Emitter**, гизмо-маркер эмиттера, живое превью в вьюпорте. Живой
пример — факел в `games/testgame` (комната 1).

## Пост-процессинг (HDR + тон-маппинг)
Часть ЯДРА рендера (`engine/src/sage/render/Framebuffer.h`, `PostProcess.h`,
`assets/shaders/post.*`). Сцена рисуется не в экран напрямую, а в offscreen
HDR-буфер (float-цвет, значения могут выходить за [0,1]), затем полноэкранный
проход выводит его на экран, применяя экспозицию, тон-маппинг ACES,
гамма-коррекцию, насыщенность/контраст и виньетку. UI обычно рисуется ПОСЛЕ
этого прохода, чтобы текст/интерфейс не тон-мапились вместе со сценой. Все
параметры — поля `PostProcessSettings`, игра заливает их каждый кадр и может
менять картинку без правки движка.

## Тени (shadow mapping от направленного света)
Часть ЯДРА рендера (`engine/src/sage/render/ShadowMap.h`,
`assets/shaders/shadow_depth.*`). Классический shadow mapping в два прохода:
депт-проход из точки зрения солнца (ортографическая проекция) в depth-текстуру,
затем основной проход сэмплирует её с PCF-фильтрацией мягких краёв и
slope-scaled bias против самозатенения (плюс отсечение лицевых, а не задних,
граней в депт-проходе — тот же классический приём). Ортобокс света задаётся
центром и радиусом, которые игра передаёт каждый кадр под свою сцену.

## Конфигурация и настройки (EngineConfig)
Единый гибкий конфиг движка — `sage::EngineConfig` (`engine/src/sage/core/Config.h`)
— заменяет разрозненное чтение env-переменных одним источником правды. Приоритет
применения (от слабого к сильному):

    значения по умолчанию  →  файл sage.cfg (JSON)  →  env-переменные

Каждое приложение на движке в точке входа делает `cfg.LoadFile("sage.cfg");
cfg.ApplyEnvOverrides(); EngineConfig::Set(cfg);`, после чего окно строится из
конфига, а слои читают `EngineConfig::Get()`.

Что настраивается:

- **Окно**: размер (`width`/`height`), режим (`windowed`/`borderless`/
  `fullscreen`), `vsync`, `resizable`, ограничение кадров `frameCap`,
  сглаживание `msaa` (0/2/4/8).
- **Дисплей**: соотношение сторон `aspect` (`free`/`16:9`/`16:10`/`4:3`/`21:9`
  — фиксированное даёт letterbox/pillarbox чёрными полосами) и масштаб
  внутреннего разрешения `renderScale` (0.25..2.0 — быстрее/чётче).
- **Графика**: `shadows` (+`shadowResolution` 512..4096), `postProcessing`,
  `fog`, `skybox` — любой тяжёлый проход отключается одним флагом.
- **Пост-эффекты**: `exposure`/`gamma`/`saturation`/`contrast`/`vignette`.

**Файл** (`sage.cfg` рядом с игрой). Пример:

```json
{
  "window":  { "width": 1920, "height": 1080, "mode": "borderless", "vsync": true },
  "display": { "aspect": "16:9", "renderScale": 1.0 },
  "graphics":{ "shadows": true, "shadowResolution": 2048, "postProcessing": true }
}
```

**env-переопределения** (поверх файла — для отладки/CI): `SAGE_WINDOW_WIDTH/
HEIGHT`, `SAGE_WINDOW_MODE`, `SAGE_VSYNC`, `SAGE_FRAME_CAP`, `SAGE_MSAA`,
`SAGE_ASPECT`, `SAGE_RENDER_SCALE`, `SAGE_SHADOWS`(+`SAGE_SHADOW_RES`),
`SAGE_POST`, `SAGE_FOG`, `SAGE_SKYBOX` (плюс обратно-совместимые
`SAGE_NO_SHADOWS`/`SAGE_NO_POST`, чьё наличие выключает проход).

**Редактор**: окно **Window → Settings** правит настройки визуально и сохраняет
их в `<проект>/sage.cfg`; **File → Build Game** кладёт файл рядом с собранной
игрой, так что игрок правит настройки, не залезая внутрь. Оконные параметры
(размер/режим/vsync) применяются при следующем запуске игры.

## Текст и шрифты (TrueType)
Текст в игровом интерфейсе рисует `UIRenderer` (`engine/src/sage/ui/`)
настоящим **TrueType-шрифтом** через класс `Font`
(`engine/src/sage/render/Font.*`, на базе
[stb_truetype](https://github.com/nothings/stb)). При создании `UIRenderer`
запекает глифы в атлас-текстуру (R8) и печатает текстурными квадами:

- **Unicode/UTF-8** — строки в `Text/TextCentered` декодируются как UTF-8, из
  коробки запечены латиница (Basic Latin + Latin-1), **кириллица** и типографская
  пунктуация (тире, кавычки, «…»). Игровые надписи можно писать по-русски —
  `games/testgame` так и делает (нижняя подсказка HUD).
- **Шрифт по умолчанию** — `engine/assets/fonts/sage-default.ttf` (DejaVu Sans,
  свободная лицензия Bitstream Vera). `sage_add_game()` копирует его рядом с
  каждой игрой автоматически, так что текст работает без ассетов в самой игре;
  если файл не найден, `UIRenderer` пробует системные шрифты, а в последнюю
  очередь откатывается на встроенный векторный `stb_easy_font` (ASCII-only) —
  движок рисует текст в любом случае.
- **Свой шрифт** — `ui.SetFont("assets/fonts/my.ttf", pixelHeight)`; метрики
  масштабируются от базовой высоты запекания, `MeasureText` учитывает
  пропорциональную ширину глифов (для вёрстки/центрирования).

Редактор `SageEditor` рисует свой UI через ImGui (у него собственная загрузка
шрифта с кириллицей — `EditorTheme::LoadFont`), поэтому `Font`/`UIRenderer` —
это именно текст ВНУТРИ игры, не редактора.

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

## Стабильность API к 1.0
Текущая версия (`0.3.0`) — ДО 1.0, API ещё может меняться без предупреждения.
С версии `1.0.0` (когда движок пройдёт боевое тестирование — см. статус в
начале файла) публичный API движка (всё, что включается как `<sage/...>` из
`engine/src/sage/**`) не будет ломаться без повышения мажорной версии. НЕ входит в
этот контракт: внутренности `engine/src/rhi/opengl/` (детали конкретного
бэкенда), внутренности самого `SageEditor` (не библиотека, а приложение) и
`games/sandbox` (пример, может меняться свободно). Версия читается из файла
`VERSION` в корне и запекается в бинарник (`sage::core::Version.h`, константа
`kSageEngineVersion`).

## Как разрабатывать движок и игру(ы) параллельно без переписывания архитектуры
Модульная граница `engine/` ↔ `editor/` ↔ `games/<name>/` уже поддерживает это
— вот как ей пользоваться:

1. **Разрабатывай игру прямо в этом репозитории**, как `games/<name>/`, рядом
   с `games/sandbox/`, пока движок и игра меняются вместе — быстрый цикл без
   версионирования/пиннинга, именно для этого спроектирован `sage_add_game()`.
   Несколько игр могут существовать в `games/` одновременно уже сейчас (сегодня
   это `games/sandbox`, но структура одинаково подходит и для второй, третьей
   игры) — не нужно ничего перестраивать, чтобы добавить ещё одну.
2. **Жёсткое правило границы**, которое нельзя нарушать: код `engine/` и
   `editor/` никогда не включает файлы из `games/*`. Проверяется командой
   (гоняй её перед коммитом, если тронул `engine/`):
   ```bash
   grep -rnE '#include "(\.\./)*games/' engine/ editor/
   ```
   должно быть пусто. Обратное направление — `games/<name>/` включает
   `<sage/...>` из движка — это и есть нормальный, ожидаемый способ
   использования библиотеки.
3. Когда/если конкретная игра станет самостоятельным продуктом и понадобится
   отделить её исходники от движка (например, чтобы поставлять движок другим
   разработчикам без вашей игры внутри) — её `games/<name>/` можно механически
   вынести в отдельный репозиторий, подключив движок как git submodule или
   через `FetchContent` на тегнутую версию (`v1.0.0` и выше). Контракт
   `sage_add_game()`/`CreateApplication`/`SAGE_MAIN` при этом не меняется —
   это перенос файлов, а не переписывание архитектуры.

## Известные ограничения / roadmap после 1.0
Сознательно не входит в 1.0 (см. `## Стабильность API` выше — это не забытое,
а осознанное решение по масштабу релиза):

- **Физика — подключаемая, но не всеобъемлющая.** Есть полноценная физическая
  подсистема с выбором бэкенда (см. `## Физика` ниже): основной — **Jolt**,
  плюс встроенный `Simple` (без зависимостей) и `Null` (выкл). Твёрдые тела
  (Box/Sphere/Capsule, Static/Dynamic/Kinematic) с гравитацией, контактами и
  вращением. НЕ входит пока: персонажный контроллер, триггеры/сенсоры,
  рейкасты из API движка, соединения (constraints/joints) — всё это есть в
  самом Jolt, но ещё не проброшено через абстракцию `PhysicsWorld`. Игра со
  своей узкой потребностью (как AABB-игрок в `games/testgame`) по-прежнему
  вольна считать коллизии сама — физика не навязывается.
- **UI-тулкит минимален** (но текст — полноценный). `UIRenderer`/`Widgets.h` —
  только `Panel`/`Label`/`ProgressBar` (immediate-mode HUD). Текст рисуется
  настоящим TrueType-шрифтом с Unicode/кириллицей (см. `## Текст и шрифты`), но
  интерактивных виджетов (кнопки/поля ввода/окна) для игровых меню нет — если
  нужно, расширяй `Widgets.h` или интегрируй стороннюю UI-библиотеку сам (для
  редактора это уже ImGui).
- **`ResourceManager` не выгружает и не кэширует текстуры** — только меши
  (`GetCube`/`GetModel`) и материалы (`GetMaterial`). Для маленьких/средних игр
  это не проблема, но для больших сцен с сотнями уникальных текстур стоит
  завести свой кэш поверх `Texture`.
- **Материалы v1 — данные, не шейдеры.** Движок/редактор гарантированно
  применяют только `Albedo` (базовый цвет вместо Color сущности);
  Emissive/Shininess/Texture сериализуются и доступны игре, но их поддержка —
  дело шейдеров конкретной игры. Путь материала хранится как записан при
  назначении (для переносимости проекта используй относительные пути).
- **Освещение — forward, тени только от солнца.** Полный набор типов света
  (ambient/directional/point/spot) есть, но тени отбрасывает лишь направленное
  «солнце» (одна shadow-карта); точечные и прожекторы не затеняются
  (для этого нужны cube-shadow'ы/доп. карты). Площадных (area) источников нет —
  они требуют другой модели затенения (LTC), это за рамками forward-рендера.
  Лимит шейдера — 8 точечных + 8 прожекторов одновременно.
- **Редактор**: нет мультивыделения, префабов, универсального добавления
  произвольных компонентов (Inspector понимает Transform/MeshRenderer/Material/
  Camera/Light/Script/RigidBody/Collider), нет ассет-импорт-пайплайна (импорт
  .obj/.gltf — вручную через путь в Inspector).
- **`SceneSerializer` не сохраняет кастомные компоненты игр** — в `.sage`
  попадают только встроенные (Name/Id/Transform/MeshRenderer/Material-путь/
Script/Camera + свет + RigidBody/Collider).
  Игра с собственными компонентами (как `games/testgame`) либо строит мир
  кодом, либо сериализует своё сама поверх движкового файла. Подтверждено
  боевым тестом; кандидат на «пользовательские сериализаторы компонентов»
  после 1.0.
- **Тестовое покрытие** — два уровня в CI. **Модульные тесты** (`tests/`,
  цель `sage_tests`, гоняются через `ctest`): быстрые проверки БЕЗ GL —
  математика (фрустум-отсечение, матрица трансформа, вычисления конфигурации),
  сериализация (round-trip `EngineConfig`/`Material`/`Scene`) и анимация
  (сэмплирование каналов, иерархия костей Animator). **Headless smoke-тесты**
  (`scripts/ci_smoke_test.sh`): движок реально запускается и рисует кадр,
  self-test редактора проходит, `TestGame` в автопилоте проходит игровой цикл
  (подбор/портал/рендер без ERROR). Покрытие пока точечное, не сплошное.
- Второй графический бэкенд (Vulkan/D3D) — RHI к этому готов (`glad` изолирован
  в `engine/src/rhi/opengl/`), но никто пока не реализован.
- Хот-релоад скриптов; каскадные тени (CSM) + мягкие тени (PCSS); bloom/SSAO/DoF;
  IBL (image-based lighting) поверх уже сделанного metallic-roughness PBR.

## CI и smoke-тесты
`.github/workflows/ci.yml` на каждый push/PR:
- **Linux**: собирает движок + `Sandbox` + `TestGame` + `SageEditor`, затем
  гоняет headless (`xvfb-run`) `scripts/ci_smoke_test.sh` — тот же скрипт можно
  запустить локально, чтобы воспроизвести падение CI:
  ```bash
  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
  ./scripts/ci_smoke_test.sh build
  ```
  Четыре проверки: `Sandbox` реально рисует кадр (скриншот на заданном кадре,
  файл непустой); `SageEditor` проходит собственный self-test
  (`SAGE_EDITOR_SELFTEST=1` — создание проекта, сохранение/загрузка сцены,
  undo/redo, Play-режим); плагин-пример `example_stats` грузится и выгружается
  без падения (см. «Плагины редактора»); `TestGame` в автопилоте проходит
  реальный игровой цикл — сериализация сцены, подбор предметов, портал между
  комнатами, рендер с тенями/пост-процессом/HUD — без единой ERROR-строки.
- **Windows (build-only)**: кросс-компиляция через `mingw-w64` тем же
  `cmake/mingw-toolchain.cmake`, что и `scripts/build_windows.sh` — ловит
  поломки Windows-сборки без необходимости реально запускать .exe в CI.

## Сборка готового продукта под платформу
Есть скрипты, которые одной командой создают ГОТОВЫЙ К РАЗДАЧЕ архив —
бинарник + все assets, версионировано:

```
scripts/
  build_linux.sh      — собрать + упаковать под Linux (из Linux)
  build_windows.sh     — собрать + упаковать под Windows (кросс-компиляция из Linux)
  build_windows.bat      — собрать + упаковать под Windows (если собираешь прямо на Windows)
  build_all.sh             — собрать сразу под обе платформы
  ci_smoke_test.sh          — headless smoke-тест (см. "CI и smoke-тесты" выше)
```

Использование:
```bash
./scripts/build_linux.sh              # соберёт "Sandbox" (имя по умолчанию)
./scripts/build_windows.sh            # кросс-компиляция в .exe
./scripts/build_all.sh                # обе платформы разом

./scripts/build_linux.sh MyGame       # для СВОЕЙ игры на этом же движке —
                                       # просто другое имя (см. games/<name>/), весь код общий
```

Результат появляется в `dist/`:
```
dist/linux/Sandbox-0.3.0-linux-x64.tar.gz
dist/windows/Sandbox-0.3.0-windows-x64.zip
```

Версия берётся из файла `VERSION` в корне — подними её перед следующим
релизом. Каждый архив самодостаточен: распаковал — запустил, ничего
доустанавливать не нужно (Windows-сборка статически линкует рантайм MinGW,
DLL не требуются).

## Как собрать вручную (без скриптов, для разработки)

Имена таргетов фиксированы (задаются через `sage_add_game`): `Sandbox` (пример
игры), `SageEditor` (редактор), `sage_engine` (библиотека). `-DGAME_NAME`
не нужен — таргет каждой игры называется так, как указано в её CMakeLists.

### Linux
```bash
sudo apt install cmake g++ libx11-dev libxrandr-dev libxinerama-dev \
                  libxcursor-dev libxi-dev libgl1-mesa-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/games/sandbox/Sandbox      # запуск примера
./build/editor/SageEditor          # запуск редактора
```
Собрать только один таргет: `cmake --build build --target Sandbox` (или `SageEditor`).

### Windows
```
cmake -B build
cmake --build build --config Release
```
Или открой папку в Visual Studio через "Open Folder" — она сама подхватит CMake.

### Кросс-компиляция в .exe из Linux
```bash
sudo apt install g++-mingw-w64-x86-64
cmake -B build-windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows -j$(nproc)
```
.exe линкуется статически — запускается на чистой Windows без установки
рантайма MinGW. Проверено сборкой (кросс-компиляция всего проекта, включая
ImGui/ImGuizmo/entt/sol2, под `x86_64-w64-mingw32-g++`), CI гоняет ту же
сборку на каждый push.
