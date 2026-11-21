---
paths:
  - "tests/**"
  - "scripts/**"
  - ".github/workflows/**"
  - "CHANGELOG.md"
---

# Тесты, проверки, CI

## Команды (из корня, после `cmake --build build`)

| Что | Команда | Когда |
|---|---|---|
| модульные | `./build/tests/sage_tests` | любая правка C++ |
| кадровые | `xvfb-run -a ./build/tests/render/sage_render_tests --references tests/render/references` | рендер, шейдеры, граф кадра |
| самопроверка редактора | `cd build/editor && SAGE_EDITOR_SELFTEST=1 xvfb-run -a ./SageEditor` | редактор |
| smoke | `bash scripts/ci_smoke_test.sh` | сборка, рантайм, перед слиянием |
| сторожа | `python3 scripts/check_<имя>.py` | по затронутой области |
| API скриптов | `python3 scripts/gen_script_api.py --check` | скрипты |
| Windows | `cmake --build build-windows -j"$(nproc)"` | платформа, CMake, заголовки, перед слиянием |

Сторожа: `check_localization`, `check_rhi_boundary`, `check_layers`,
`check_paths`, `check_asset_slots`, `check_popup_ids`, `check_menu_style`,
`check_release_ui`, `check_imgui_pairs`, `check_ui_kit`, `check_file_size`.
Они быстрые — перед слиянием прогонять все.

Самопроверка редактора сама не завершается: пишет `SELFTEST: PASS`/`FAIL` в
`build/editor/sage_editor.log` и работает дальше. Запускать ИЗ `build/editor`
в фоне, вердикт читать из лога, процесс потом остановить.

## Полный прогон

Всё из таблицы плюс обе сборки (`build`, `build-windows`) — перед слиянием в
`main` и при крупных правках. Половина поломок Windows на Linux не видна.

## Визуальная проверка

Видимая правка (интерфейс, рендер, редактор) проверяется глазами: снимок
(редактор под `xvfb-run` или кадровая проверка, сохраняющая картинку),
открыть его и убедиться, что получилось задуманное. В отчёте — что видно.

## Написание тестов

- Правка поведения — с тестом, который на старом коде падает.
- Новый модульный тест — `tests/test_<область>.cpp` по образцу соседних
  (`tests/TestFramework.h`), подключить в `tests/CMakeLists.txt`.
- Кадровые проверки — `tests/render/Checks_*.cpp`, эталоны в
  `tests/render/references`.

## CI и релизы

- `linux.yml` → релиз `linux-latest`, `windows.yml` → `windows-latest`;
  публикуются только с `main`. `macos.yml` — ручной запуск, не ждать.
- Резервные ветки исключены из сборки (`branches-ignore` в `windows.yml`),
  чтобы старое состояние не перезаписывало релиз.
- Любой push, запуск workflow и публикация — только с разрешения
  пользователя (см. CLAUDE.md, «Git»).

## CHANGELOG.md

Запись с датой, разделы Исправлено / Добавлено / Изменено / Ускорено.
Последняя запись уходит в описание релиза. `VERSION` до 1.0 — 0.5.0.
