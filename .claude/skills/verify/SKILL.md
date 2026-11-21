---
name: verify
description: Выбор и запуск проверок SAGE по изменённой области (сборка, модульные и кадровые тесты, самопроверка редактора, сторожа check_*.py, mingw-сборка, визуальная проверка снимком); полный прогон перед слиянием. Использовать после правки кода, перед отчётом о готовности.
---

# Проверка правки

## 1. Выбрать проверки по области

Определить изменённые файлы (`git diff --name-only` против базы ветки) и взять
только нужные строки. Сборка `cmake --build build -j"$(nproc)"` нужна для
любой правки C++ (нет `build/` — `cmake -B build -DCMAKE_BUILD_TYPE=Release`).

| Изменено | Проверки |
|---|---|
| любой C++ | сборка; `./build/tests/sage_tests` (или нужный тест) |
| граф кадра, рендер, шейдеры | + кадровые; снимок |
| `editor/src` | + самопроверка; сторожа UI; снимок, если видно |
| строки интерфейса, `editor/lang` | `check_localization` |
| API скриптов | `gen_script_api.py --check` |
| заголовки, CMake, пути, платформа | + сборка `build-windows` |
| рантайм, упаковка, сборка | + smoke |
| только docs / инструкции | ничего не собирать |

Сторожа по области: C++ движка — `check_layers`, `check_rhi_boundary`,
`check_paths`, `check_file_size`; редактор — `check_ui_kit`,
`check_popup_ids`, `check_menu_style`, `check_imgui_pairs`,
`check_release_ui`, `check_asset_slots`, `check_localization`. Запуск —
`python3 scripts/<имя>.py`.

## 2. Команды (из корня)

```
./build/tests/sage_tests
xvfb-run -a ./build/tests/render/sage_render_tests --references tests/render/references
bash scripts/ci_smoke_test.sh
python3 scripts/gen_script_api.py --check
cmake -B build-windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows -j"$(nproc)"
```

Самопроверка редактора сама не завершается: запустить в фоне ИЗ
`build/editor` — `SAGE_EDITOR_SELFTEST=1 xvfb-run -a ./SageEditor`, ждать в
`build/editor/sage_editor.log` строку `SELFTEST: PASS`/`FAIL`, затем
остановить процесс.

Вывод обрезать: при успехе — итоговая строка, при сбое — `tail -n 40` или
`grep` по ошибке. Прошедшую проверку без новых правок не повторять.

## 3. Визуальная проверка

Видимая правка (интерфейс, рендер, редактор) — глазами: снимок (редактор под
`xvfb-run` или кадровая проверка, сохраняющая картинку), открыть его и
убедиться, что получилось задуманное. В отчёте — что видно на снимке.

## 4. Полный прогон

Перед слиянием в `main` и при крупных правках: обе сборки (`build`,
`build-windows`), все тесты и самопроверка, smoke, все сторожа (они быстрые),
`gen_script_api.py --check`. Половина поломок Windows на Linux не видна.

## 5. Отчёт

Перечислить только реально запущенное и результат; что пропущено — и почему.
