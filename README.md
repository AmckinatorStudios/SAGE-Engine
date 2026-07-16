# SAGE Engine — универсальный модульный 3D-движок (C++ / OpenGL)

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
  .github/workflows/ci.yml    — CI: сборка + headless smoke-тесты (см. ниже)

  engine/                     → библиотека sage::engine (универсальное ядро)
    CMakeLists.txt
    src/sage/                 — ПУБЛИЧНЫЙ API (включается как <sage/...>)
      core/     Application, Layer, GameModule, Window, InputSystem, Log
      rhi/      GraphicsDevice — абстракция графического устройства
      ecs/      Registry (фасад entt), RenderSystem
      scene/    Scene (ECS), Components, Transform, Light, SceneSerializer
      render/   Shader, Camera, Mesh, Model, Skybox, ShadowMap, PostProcess, ...
      ui/       UIRenderer, Widgets — immediate-mode UI
      scripting/ ScriptEngine — Lua (sol2)
      audio/    AudioEngine — 2D/3D-звук, музыка (miniaudio)
    src/rhi/opengl/           — OpenGL-бэкенд: ЕДИНСТВЕННОЕ место с glad (уровень устройства)

  editor/                     → exe SageEditor (редактор на ImGui, см. раздел ниже)
    src/, assets/shaders/

  games/sandbox/              → exe Sandbox (минимальный showcase движка)
    src/  main.cpp (тонкий), SandboxLayer.*
    assets/ shaders/basic.*, scripts/spin.lua

  scripts/                    — упаковка релиза + ci_smoke_test.sh (см. ниже)
  external/                   — glad, stb, tinygltf, miniaudio, imgui, imguizmo (вендорены)
```

Сборка даёт: библиотеку `sage::engine`, exe `Sandbox` и exe редактора
`SageEditor`. Граница строгая и проверяемая: код движка нигде не включает
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

`Sandbox` служит и референсом «как сделать игру», и единственной запускаемой
fixture для headless smoke-тестов в CI (сама библиотека `sage_engine` не
запускается — нужен exe). Поддерживает те же env-хуки, что и `SageEditor`:
`SAGE_WINDOW_WIDTH`/`SAGE_WINDOW_HEIGHT`, `SAGE_SCREENSHOT_AT_FRAME`/
`SAGE_SCREENSHOT_PATH`.

## Редактор SageEditor
Полноценный редактор сцен на ImGui (docking) + ImGuizmo:
- **Доккинг**: раскладка по умолчанию строится автоматически (Hierarchy слева,
  Inspector справа, Console/Assets табами снизу, Viewport в центре); панели
  свободно перетаскиваются и стыкуются, раскладка сохраняется между запусками
  (`sage_editor_imgui.ini`), Window > Reset Layout возвращает дефолт.
- **Гизмо** (ImGuizmo): перемещение/поворот/масштаб выбранной сущности прямо во
  вьюпорте. Горячие клавиши **W/E/R**, привязка к сетке — галка Snap.
- **Камера вьюпорта**: ПКМ — осмотреться, ПКМ+WASD (Q/E — вниз/вверх, Shift —
  быстрее) — полёт, колесо — наезд.
- **Пикинг**: клик ЛКМ по объекту выбирает его (луч в ECS-сцену), клик в пустоту
  снимает выбор.
- **Проекты**: File > New Project создаёт `<папка>/<имя>/` с `project.sageproj`,
  `scenes/` и `assets/`; Open Project открывает существующий. Сцены сохраняются
  в `scenes/` проекта (`.sage` JSON) и открываются двойным кликом в Assets.
- **Панели**: Hierarchy (создание/дублирование/удаление сущностей, контекстное
  меню), Inspector (имя, Transform, цвет, выбор меша Cube/Model, скрипт),
  Console (живой лог движка с цветами уровней), Assets (браузер файлов проекта).
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

## Как добавить графический бэкенд
Реализовать интерфейсы `sage::rhi::GraphicsDevice` (устройство: инициализация,
состояние конвейера, чтение пикселей) и ресурсы из `sage/rhi/Resources.h`
(`ShaderProgram`, `Geometry`, `Texture2D`, `TextureCube`, `RenderTarget`) в новом
каталоге `engine/src/rhi/<backend>/`, добавить ветку в фабрику
`GraphicsDevice::Create()`. Код движка и игр обращается ТОЛЬКО к этим
интерфейсам: `glad`/GL-вызовы существуют исключительно в `engine/src/rhi/opengl/`
(инвариант проверяется grep'ом — см. ниже), поэтому смена бэкенда не затрагивает
ни движок, ни игры, ни редактор.

## Система сцен и сериализация
`Scene` — это ECS-сцена на `entt::registry`. Сущности собираются из компонентов
(`NameComponent`, `Transform`, `MeshRendererComponent`, `ScriptComponent`, ...);
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
maps/metallic-roughness/emissive пока не читаются), анимации/скелеты glTF не
поддержаны (только статическая геометрия), OBJ без переиспользования вершин.

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
С версии `1.0.0` публичный API движка (всё, что включается как `<sage/...>` из
`engine/src/sage/**`) не ломается без повышения мажорной версии. НЕ входит в
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

- **Нет общей (движковой) системы коллизий/физики.** Каждая игра решает это
  сама под свою геометрию (voxel-коллизии, physics-движок стороннего
  производителя, простые AABB — что угодно). Кандидат на добавление в
  будущем как ОПЦИОНАЛЬНАЯ подсистема (не обязательная для игр, которым она
  не нужна).
- **UI-тулкит минимален.** `UIRenderer`/`Widgets.h` — только `Panel`/`Label`/
  `ProgressBar` (immediate-mode HUD). Нет кнопок/полей ввода/окон для игровых
  меню — если нужно интерактивное UI для игры (не для редактора — там ImGui),
  либо расширяй `Widgets.h`, либо интегрируй стороннюю UI-библиотеку сам.
- **`ResourceManager` не выгружает и не кэширует текстуры** — только меши
  (`GetCube`/`GetModel`). Для маленьких/средних игр это не проблема, но для
  больших сцен с сотнями уникальных текстур стоит завести свой кэш поверх
  `Texture`.
- **Редактор**: нет мультивыделения, префабов, универсального добавления
  произвольных компонентов (Inspector сейчас понимает только Transform/
  MeshRenderer/Script), нет ассет-импорт-пайплайна (импорт .obj/.gltf — вручную
  через путь в Inspector).
- **Тестовое покрытие** — только headless smoke-тесты в CI (см.
  `scripts/ci_smoke_test.sh`): движок реально запускается и рисует кадр,
  self-test редактора проходит. Юнит-тестов на отдельные классы нет.
- Второй графический бэкенд (Vulkan/D3D) — RHI к этому готов (`glad` изолирован
  в `engine/src/rhi/opengl/`), но никто пока не реализован.
- Хот-релоад скриптов; каскадные тени (CSM) + мягкие тени (PCSS); bloom/SSAO/DoF;
  PBR-материалы.

## CI и smoke-тесты
`.github/workflows/ci.yml` на каждый push/PR:
- **Linux**: собирает движок + `Sandbox` + `SageEditor`, затем гоняет headless
  (`xvfb-run`) `scripts/ci_smoke_test.sh` — тот же скрипт можно запустить
  локально, чтобы воспроизвести падение CI:
  ```bash
  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
  ./scripts/ci_smoke_test.sh build
  ```
  Проверяет: `Sandbox` реально рисует кадр (скриншот на заданном кадре, файл
  непустой) и `SageEditor` проходит собственный self-test (`SAGE_EDITOR_SELFTEST=1`
  — создание проекта, сохранение/загрузка сцены, undo/redo, Play-режим).
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
dist/linux/Sandbox-1.0.0-linux-x64.tar.gz
dist/windows/Sandbox-1.0.0-windows-x64.zip
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
