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

## 4. Полный прогон (обязателен перед слиянием)

Ветка не «готова к слиянию», пока этот прогон не прошёл целиком на её
последнем коммите. Этапы по порядку, у каждого — команда, код возврата
(`...; echo "exit=$?"`), итог и причина, если не прошёл:

| # | Этап | Команда |
|---|---|---|
| 1 | сторожа (как в `ci.yml`) | `python3 scripts/check_<имя>.py` для rhi_boundary, layers, localization, paths, asset_slots, popup_ids, menu_style, release_ui, imgui_pairs, ui_kit, file_size |
| 2 | сборка Linux | `cmake --build build -j"$(nproc)"` |
| 3 | подсказки API | `python3 scripts/gen_script_api.py --check` |
| 4 | модульные тесты | `./build/tests/sage_tests` |
| 5 | кадровые тесты рендера | `xvfb-run -a --server-args="-screen 0 800x600x24" ./build/tests/render/sage_render_tests --references tests/render/references` |
| 6 | smoke (Sandbox + самопроверка редактора) | `bash scripts/ci_smoke_test.sh build` |
| 7 | сборка Windows | `cmake -B build-windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release` и `cmake --build build-windows -j"$(nproc)"` |

Порядок работы: изменения закончены и закоммичены → полный прогон → ошибки
исправлены → **повторный полный прогон** (любая правка проверяемого кода после
сбоя, даже одна строка, — заново с этапа 1) → только тогда штамп и `/merge-main`.

Правила:

- Этап **нельзя пропустить** и нельзя считать пройденным по предположению
  («тот же код», «упало и раньше», «на Linux видно»). Нет инструмента, бинарника,
  дисплея или эталонов — этап «не пройден: причина», штамп не ставится.
- Сбой, не связанный с правкой (был на `main`), всё равно стоп: сообщить, как
  проверено, что он старый, и ждать решения пользователя. Молча не принимать.
- Штамп ставится ТОЛЬКО когда все семь этапов дали код 0 и дерево чистое:

```
[ -z "$(git status --porcelain)" ] && git rev-parse HEAD > build/.verify-full
```

  Штамп — `build/.verify-full` (в `.gitignore`): хеш проверенного коммита.
  Новый коммит или правка делают его устаревшим, `/merge-main` сверяет хеш.

## 5. Отчёт

Таблица по этапам: что запускалось, код возврата, итог, причина сбоя или
невозможности запуска. Незапущенное — «не запущено: причина», не «пройдено».
Для полного прогона — ещё хеш коммита и записан ли штамп.
