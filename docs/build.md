# Сборка и CI

Как собрать движок и игру, что гоняется в CI.

## Содержание

- [CI и smoke-тесты](#ci-и-smoke-тесты)
- [Сборка готового продукта под платформу](#сборка-готового-продукта-под-платформу)
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
