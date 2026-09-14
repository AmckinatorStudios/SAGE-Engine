# Правила работы над SAGE-Engine

Порядок работы для Claude Code. Движок описан в `docs/` (оглавление —
`docs/README.md`) — это источник правды по архитектуре. README.md — короткая
витрина репозитория, подробности туда не дописывать.

## Порядок внесения изменений (без исключений)

1. **Резерв перед началом:** `git fetch origin main && git branch -f claude/backup-main-<ГГГГММДД>-<sha> origin/main && git push origin <она>`.
   Веткой, не тегом — пуш тегов даёт 403. Резерв не удалять.
2. **Работа — в отдельной ветке** от свежего `origin/main`:
   `git fetch origin main && git checkout -B claude/<о-чём-правка> origin/main`.
   Прямо в `main` не коммитить.
3. **Проверка до слияния** — полный прогон (см. ниже) обязан быть зелёным
   на ветке.
4. **Слить в `main` и запушить СРАЗУ, как только правка проверена и готова**
   — не дожидаясь отдельной просьбы. Слияние `--no-ff` (граница правки в
   истории, откатывается одним `git revert -m 1`).
5. **После каждого пуша в `main` — дождаться релиза**: workflow «Сборка
   редактора для Windows» обязан стать зелёным, а релиз `windows-latest`
   обновиться (дата и коммит в описании совпадают с тем, что запушено).
   Если работа упала — чинить сразу, релиз не должен оставаться старым.
6. Ветка не удаляется сразу после слияния.

Откат: `git revert -m 1 <коммит слияния>` (мягко) или `git reset --hard
<резервная ветка>` + `push --force-with-lease` (жёстко, только по прямой
просьбе).

Релиз публикуется автоматически только с `main` (с рабочих и резервных
веток — артефактом на вкладку Actions, публичную ссылку это не трогает).
Резервные ветки не собираются (`branches-ignore` в `windows.yml`) — иначе
старое состояние перезаписывает свежий релиз.

## Что прогнать перед слиянием

```
cmake --build build -j"$(nproc)"                                  # сборка
./build/tests/sage_tests                                          # модульные
xvfb-run -a ./build/tests/render/sage_render_tests \
    --references tests/render/references                          # кадровые
cd build/editor && SAGE_EDITOR_SELFTEST=1 xvfb-run -a ./SageEditor  # самопроверка
bash scripts/ci_smoke_test.sh                                     # smoke
python3 scripts/check_localization.py                             # переводы
python3 scripts/check_rhi_boundary.py                             # граница RHI
python3 scripts/check_paths.py                                    # пути из окружения
python3 scripts/check_asset_slots.py                              # ассеты — слотами
python3 scripts/check_popup_ids.py                                # имена всплывающих окон
python3 scripts/check_menu_style.py                               # отступы меню
python3 scripts/gen_script_api.py --check                         # подсказка по API скриптов
cmake --build build-windows -j"$(nproc)"                          # кросс-сборка mingw
```

Обе платформы обязательны всегда (Linux `build/`, Windows `build-windows/`
mingw) — половина поломок Windows-сборки на Linux не видна. macOS не собираем.

Самопроверка редактора не завершается сама — пишет в
`build/editor/sage_editor.log` строку `SELFTEST: PASS`/`FAIL` и продолжает
работать; запускать ИЗ `build/editor`, вердикт читать из лога.

## Пути — не ASCII

Значение окружения превращается в путь только через
`sage::EnvPath`/`sage::EnvString` (`engine/src/sage/core/Paths.h`);
`std::getenv` для путей запрещён — сторожит `scripts/check_paths.py`.
(`std::filesystem::path` на байтах ANSI бросает исключение — так не
запускался редактор на кириллических путях Windows.)

## Правила самого кода

- **Комментарии и текст интерфейса — по-русски**, объясняют ПОЧЕМУ, а не
  пересказывают код.
- **Каждая строка интерфейса — через `T("...")`**, перевод в
  `editor/lang/ru.json`, затем `python3 scripts/gen_lang.py`. Проверяет
  `scripts/check_localization.py`.
- **Вызовы `gl*` — только в `engine/src/rhi/`.** Проверяет
  `scripts/check_rhi_boundary.py`.
- **Ассет выбирается СЛОТОМ** (`assetslot::Draw`,
  `editor/src/AssetSlot.h`), не полем с путём. Исключение — путь НАРУЖУ
  проекта (папка проекта, создание/открытие проекта, папка сборки).
  Проверяет `scripts/check_asset_slots.py`.
- **`ImGui::OpenPopup` и `BeginPopup*` — одно имя**, вместе с «###»
  (перевод через `T()` меняет строку → меняет идентификатор окна).
  Разойдутся — окно висит, не рисуется, а открывший его просит его каждый
  кадр, закрывая всё остальное («редактор завис»). Проверяет
  `scripts/check_popup_ids.py`, в рантайме подстраховывает
  `EditorLayer::CloseGhostPopups`.
- **Всплывающее меню объявляет свои отступы** — `Sage::UI::MenuScope` перед
  `BeginPopup` (иначе наследует стиль того, кто его открыл — например,
  обнулённый `WindowPadding` панели). Модальных окон не касается. Проверяет
  `scripts/check_menu_style.py`.
- **Новая функция скриптам — вызов `Bind(...)`** в
  `engine/src/sage/scripting/`, затем `python3 scripts/gen_script_api.py`
  (собирает подсказку `editor/assets/api/sage.lua`). Сторожит `--check`.
- **Новый файл в редакторе** — дописать в `editor/CMakeLists.txt`.
- **Правка поведения — вместе с проверкой**, которая на старом коде падает.
