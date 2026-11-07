# Сборка и CI

Как собрать движок и игру, что гоняется в CI.

## Содержание

- [CI и smoke-тесты](#ci-и-smoke-тесты)
- [Сборка готового продукта под платформу](#сборка-готового-продукта-под-платформу)
- [Настройки игры: название, автор, версия, иконка](#настройки-игры-название-автор-версия-иконка)
- [Пользовательские папки: скины, музыка, моды игрока](#пользовательские-папки-скины-музыка-моды-игрока)
- [Сборка игры из проекта без редактора (sage_build)](#сборка-игры-из-проекта-без-редактора-sage_build)
- [Как собрать вручную (без скриптов, для разработки)](#как-собрать-вручную-без-скриптов-для-разработки)

## CI и smoke-тесты
`.github/workflows/ci.yml` на каждый push/PR:
- **Linux**: собирает движок + `Sandbox` + `TestGame` + `SageEditor`, затем
  гоняет headless (`xvfb-run`) `scripts/ci_smoke_test.sh` — тот же скрипт можно
  запустить локально, чтобы воспроизвести падение CI:
  ```bash
  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
  ./scripts/ci_smoke_test.sh build
  ```
  Одиннадцать проверок: `Sandbox` реально рисует кадр (скриншот на заданном кадре,
  файл непустой); `SageEditor` проходит собственный self-test
  (`SAGE_EDITOR_SELFTEST=1` — создание проекта, сохранение/загрузка сцены,
  undo/redo, Play-режим); плагин-пример `example_stats` грузится и выгружается
  без падения (см. «Плагины редактора»); `TestGame` в автопилоте проходит
  реальный игровой цикл — сериализация сцены, подбор предметов, портал между
  комнатами, рендер с тенями/пост-процессом/HUD — без единой ERROR-строки;
  собранная игра запускается из чужого каталога и по `.sageproj`; E2E-прогон
  «игра с Lua создаётся в редакторе, играется и собирается в exe»; обработчик
  падений ловит настоящее падение и пишет отчёт; отказ запуска без дисплея
  слышно в логе; пути с кириллицей; шаблон скачивается по HTTP и ставится;
  **выход из движка завершается кодом 0** без обходного `ResourceManager::Clear()`
  (см. `tests/exit_probe.cpp` и [history.md](history.md)).
- **Windows (build-only)**: кросс-компиляция через `mingw-w64` тем же
  `cmake/mingw-toolchain.cmake`, что и `scripts/build_windows.sh` — ловит
  поломки Windows-сборки без необходимости реально запускать .exe в CI.

### Headless-сессия над ЧУЖИМ проектом

Проверки выше гоняют редактор на играх, которые редактор сам же и сочинил у
себя в коде. Настоящая игра лежит в отдельном репозитории, её сцены и скрипты
пишет человек — и чтобы CI ТАКОЙ игры мог проверить себя настоящим редактором,
`SageEditor` умеет отработать сессию над любым проектом без единого клика:

```bash
SAGE_EDITOR_OPEN_PROJECT=/путь/к/проекту \
SAGE_EDITOR_OPEN_SCENE=main.sage \
SAGE_EDITOR_PLAY_SECONDS=60 \
SAGE_EDITOR_PLAY_STEP=0.0166 \
SAGE_GAME_ARGS="autopilot=1 seed=42" \
SAGE_EDITOR_BUILD_TO=dist \
  ./build/editor/SageEditor
```

Делается ровно то, что человек делает мышью: **Open Project → выбрать сцену →
Play → Stop → File > Build Game**. Время в Play идёт ФИКСИРОВАННЫМ шагом, а не
настоящим: прогон должен быть воспроизводимым, иначе «успела ли игра дойти до
события» зависело бы от загрузки машины. Итог — строка `SESSION: PASS` (или
`FAIL`) в логе, по которой CI игры и решает, жива ли она. Так [The
Boat](https://github.com/AmckinatorStudios/The-Boat) на каждый push проходится
автопилотом внутри редактора, собирается в exe и проходится ещё раз уже
собранным бинарником.

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

## Настройки игры: название, автор, версия, иконка

Окно **Window → Game Settings** (или раздел `"game"` в `project.sageproj`,
`sage/assets/GameInfo.h`):

```json
"game": {"name": "My Game", "description": "...", "author": "...", "company": "...",
         "version": "1.2.0", "copyright": "...", "icon": "assets/icon.png"}
```

- **Название** — заголовок окна игры и имя папки и `.exe` сборки (символы,
  недопустимые в именах файлов, убираются). Пусто — имя проекта.
- **Иконка** — PNG проекта, квадрат 256×256 и больше. Плеер ставит её иконкой
  окна (Windows, Linux; у macOS иконка — у пакета `.app`). Сборка на Windows
  вшивает её в `.exe` (размеры 16…256) — её показывают Проводник и панель
  задач.
- **Версия, автор, издатель, описание, копирайт** — сборка на Windows вшивает
  их в «Свойства» `.exe` (VERSIONINFO). Сборка на другой системе игру
  собирает, а вшивание пропускает с предупреждением в логе.

## Пользовательские папки: скины, музыка, моды игрока

Собранная игра прячет ресурсы в зашифрованный `game.sagepak` — положить туда
свой скин или трек нельзя. Для этого игра объявляет **открытые папки**:
окно **Game Settings → User folders** или раздел `"game"` проекта
(`sage/assets/UserContent.h`):

```json
"userFolders": [
  {"name": "skins", "title": "Skins", "description": "Put 64x64 PNG skins here.",
   "extensions": [".png"], "location": "game"},
  {"name": "music", "title": "Music", "extensions": [".ogg", ".wav"], "location": "data"}
]
```

- **name** — имя папки на диске и ключ в скриптах (латиница, цифры, `-`, `_`).
- **location**: `game` — рядом с собранной игрой (видно сразу, переносится
  вместе с игрой; если туда нельзя писать — Program Files, образ диска —
  папка уходит в данные игрока), `data` — в папке данных игры этого
  пользователя, рядом с `saves/` и `screenshots/`. Запуск из проекта и
  Play-режим редактора всегда берут папку данных — проект не засоряется.
- **extensions** — какие файлы видят скрипты (без учёта регистра); пусто —
  любые. Подпапки читаются, скрытые файлы и `README.txt` — нет.
- **Примеры** — файлы из `<проект>/userfolders/<name>/`: сборка кладёт их в
  папку рядом с игрой (в пакет они не идут), а при первом создании папки
  данных — туда. Уже лежащий у игрока файл не затирается.
- В каждой папке — `README.txt` с названием, описанием и списком типов.

Скрипт: `user_folder.list("skins")` — файлы `{name, path, relative, size,
modified}`, `path` сразу годится для `texture.load`, `PlayMusic`;
`user_folder.changed("skins")` — изменилось ли содержимое с прошлого
вопроса (перечитать без перезапуска); `user_folder.open("skins")` — открыть
в проводнике; `user_folder.path(name)`, `user_folder.folders()`. Имя, не
объявленное в проекте, — ошибка: так скрипт не прочтёт произвольное место
диска.

## Сборка игры из проекта без редактора (sage_build)

`tools/sage_build` — то же, что **File → Build Game**, из командной строки
(CI игры, выпуск без редактора). Упаковка одна: `sage::assets::BuildGame`
(`engine/src/sage/assets/GameBuild.h`) зовут и окно редактора, и инструмент.

```bash
sage_build --project <папка проекта> --player <SagePlayer[.exe]> --out <папка> \
           [--name <имя игры>] [--project-file]
```

Результат — `<out>/<имя>/`: исполняемый файл игры, `game.sagepak` (проект,
ресурсы движка и оглавление ассетов) и `sage.cfg`. Имя по умолчанию — поле
`name` из `project.sageproj`. Плеер может быть другой платформы: игру для
Windows можно собрать на Linux, взяв `runtime/SagePlayer.exe` и его `assets/`
из архива редактора. `sage_build.exe` лежит в корне архива редактора для
Windows. Код возврата: 0 — собрано, 1 — ошибка, 2 — неверные аргументы.

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

### macOS (Apple Silicon)
```bash
brew install cmake ninja
cmake -B build-macos -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 -DSAGE_VULKAN=OFF
cmake --build build-macos -j$(sysctl -n hw.ncpu)
./build-macos/editor/SageEditor
```
Рендер — OpenGL 3.3 core: его macOS даёт на любом Mac. Vulkan выключен, на
Mac его нет без MoltenVK. Готовый `SageEditor.app` с плеером, темами и
подписью ad hoc собирает `.github/workflows/macos.yml`, он же выкладывает
релиз `macos-latest`. Собрать macOS из Linux нельзя, поэтому эта сборка
проверяется только в CI.

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
