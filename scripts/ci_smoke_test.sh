#!/usr/bin/env bash
# Headless smoke-тест движка: собранные Sandbox и SageEditor реально
# запускаются, рисуют кадр и корректно завершаются (exit 0). Не заменяет
# unit-тесты (их у движка нет — см. README, "Известные ограничения"), но
# ловит явные регрессии сборки/рантайма при каждом push/PR.
#
# Используется и в CI (.github/workflows/ci.yml), и локально:
#   ./scripts/ci_smoke_test.sh [build-dir]
set -euo pipefail

BUILD_DIR="${1:-build}"
SANDBOX_EXE="${BUILD_DIR}/games/sandbox/Sandbox"
EDITOR_EXE="${BUILD_DIR}/editor/SageEditor"
SCRATCH_DIR=$(mktemp -d)
trap 'rm -rf "${SCRATCH_DIR}"' EXIT

# xvfb-run — если уже внутри Xvfb (переменная DISPLAY выставлена и не
# локальный дисплей CI сам поднял), запускаем напрямую; иначе оборачиваем.
run_headless() {
    if command -v xvfb-run &> /dev/null; then
        xvfb-run -a "$@"
    else
        "$@"
    fi
}

echo "=== Smoke-тест 1/9: Sandbox (рендер сцены + скриптинг) ==="
if [ ! -x "${SANDBOX_EXE}" ]; then
    echo "ОШИБКА: не найден собранный бинарник ${SANDBOX_EXE}"
    exit 1
fi
SANDBOX_LOG="${SCRATCH_DIR}/sandbox.log"
SANDBOX_SHOT="${SCRATCH_DIR}/sandbox.png"
STATUS=0
( cd "$(dirname "${SANDBOX_EXE}")" && \
  run_headless env SAGE_SCREENSHOT_AT_FRAME=10 SAGE_SCREENSHOT_PATH="${SANDBOX_SHOT}" \
      "./$(basename "${SANDBOX_EXE}")" ) > "${SANDBOX_LOG}" 2>&1 || STATUS=$?
if [ ${STATUS} -ne 0 ]; then
    echo "ОШИБКА: Sandbox завершился с кодом ${STATUS}"; cat "${SANDBOX_LOG}"; exit 1
fi
SHOT_SIZE=$(stat -c%s "${SANDBOX_SHOT}" 2>/dev/null || echo 0)
if [ "${SHOT_SIZE}" -lt 1024 ]; then
    echo "ОШИБКА: скриншот Sandbox отсутствует или подозрительно мал (${SHOT_SIZE} байт)"
    cat "${SANDBOX_LOG}"; exit 1
fi
echo "OK: Sandbox отрисовал кадр, скриншот ${SHOT_SIZE} байт"

echo "=== Smoke-тест 2/9: SageEditor (self-test: проект+сцена+undo/redo+play) ==="
if [ ! -x "${EDITOR_EXE}" ]; then
    echo "ОШИБКА: не найден собранный бинарник ${EDITOR_EXE}"
    exit 1
fi
EDITOR_LOG="${SCRATCH_DIR}/editor.log"
EDITOR_SHOT="${SCRATCH_DIR}/editor.png"
STATUS=0
# SAGE_EDITOR_PLUGINS=1 — плагины по умолчанию ОТКЛЮЧЕНЫ (экспериментальны),
# здесь включаем явно, чтобы smoke-тест 3 всё ещё проверял загрузку плагина.
( cd "$(dirname "${EDITOR_EXE}")" && rm -rf selftest_project && \
  run_headless env SAGE_EDITOR_SELFTEST=1 SAGE_EDITOR_PLUGINS=1 SAGE_SCREENSHOT_AT_FRAME=10 SAGE_SCREENSHOT_PATH="${EDITOR_SHOT}" \
      "./$(basename "${EDITOR_EXE}")" ) > "${EDITOR_LOG}" 2>&1 || STATUS=$?
if [ ${STATUS} -ne 0 ]; then
    echo "ОШИБКА: SageEditor завершился с кодом ${STATUS}"; cat "${EDITOR_LOG}"; exit 1
fi
if ! grep -q "SELFTEST: PASS" "${EDITOR_LOG}"; then
    echo "ОШИБКА: self-test редактора не прошёл (нет 'SELFTEST: PASS' в логе)"
    cat "${EDITOR_LOG}"; exit 1
fi
echo "OK: SageEditor self-test прошёл"

echo "=== Smoke-тест 3/9: плагины редактора (opt-in, SAGE_EDITOR_PLUGINS=1) ==="
if ! grep -q "Загружен плагин: Example Stats" "${EDITOR_LOG}"; then
    echo "ОШИБКА: плагин example_stats не загрузился при SAGE_EDITOR_PLUGINS=1"
    cat "${EDITOR_LOG}"; exit 1
fi
echo "OK: плагин example_stats загрузился и выгрузился без падения (плагины — opt-in)"

echo "=== Smoke-тест 4/9: TestGame (боевая игра: автопилот собирает монеты и проходит портал) ==="
TESTGAME_EXE="${BUILD_DIR}/games/testgame/TestGame"
if [ ! -x "${TESTGAME_EXE}" ]; then
    echo "ОШИБКА: не найден собранный бинарник ${TESTGAME_EXE}"
    exit 1
fi
TESTGAME_LOG="${SCRATCH_DIR}/testgame.log"
TESTGAME_SHOT="${SCRATCH_DIR}/testgame.png"
STATUS=0
( cd "$(dirname "${TESTGAME_EXE}")" && \
  run_headless env SAGE_TESTGAME_AUTOPILOT=1 SAGE_SCREENSHOT_AT_FRAME=400 SAGE_SCREENSHOT_PATH="${TESTGAME_SHOT}" \
      "./$(basename "${TESTGAME_EXE}")" ) > "${TESTGAME_LOG}" 2>&1 || STATUS=$?
if [ ${STATUS} -ne 0 ]; then
    echo "ОШИБКА: TestGame завершился с кодом ${STATUS}"; cat "${TESTGAME_LOG}"; exit 1
fi
SHOT_SIZE=$(stat -c%s "${TESTGAME_SHOT}" 2>/dev/null || echo 0)
if [ "${SHOT_SIZE}" -lt 1024 ]; then
    echo "ОШИБКА: скриншот TestGame отсутствует или подозрительно мал (${SHOT_SIZE} байт)"
    cat "${TESTGAME_LOG}"; exit 1
fi
# Реальный игровой цикл: сериализация, подбор предметов, переход между сценами
# должны отработать; TrueType-шрифт HUD — загрузиться; физика — на Jolt; а лог
# — не содержать ни одной ERROR-строки движка.
for MARKER in "serialization round-trip PASS" "TESTGAME: picked up" "TESTGAME: portal -> room2" \
              "Font] Загружен шрифт" "TESTGAME: physics backend" "Anim] SkinnedModel"; do
    if ! grep -q "${MARKER}" "${TESTGAME_LOG}"; then
        echo "ОШИБКА: в логе TestGame нет маркера '${MARKER}'"
        cat "${TESTGAME_LOG}"; exit 1
    fi
done
if grep -q "ERROR" "${TESTGAME_LOG}"; then
    echo "ОШИБКА: в логе TestGame есть ERROR-строки:"
    grep "ERROR" "${TESTGAME_LOG}"; exit 1
fi
echo "OK: TestGame прошёл игровой цикл (подбор + портал + рендер, скриншот ${SHOT_SIZE} байт, без ERROR)"

echo "=== Smoke-тест 5/9: собранная игра (SagePlayer + проект из редактора) ==="
# Self-test редактора (тест 2) собрал selftest-проект в запускаемую игру через
# File > Build Game — здесь она реально запускается отдельным процессом.
GAME_DIR="${BUILD_DIR}/editor/selftest_dist/selftest_project"
GAME_EXE="${GAME_DIR}/selftest_project"
if [ ! -x "${GAME_EXE}" ]; then
    echo "ОШИБКА: собранная игра не найдена: ${GAME_EXE} (self-test редактора должен был её собрать)"
    exit 1
fi
PLAYER_LOG="${SCRATCH_DIR}/player.log"
PLAYER_SHOT="${SCRATCH_DIR}/player.png"
STATUS=0
( cd "${GAME_DIR}" && \
  run_headless env SAGE_SCREENSHOT_AT_FRAME=10 SAGE_SCREENSHOT_PATH="${PLAYER_SHOT}" \
      ./selftest_project ) > "${PLAYER_LOG}" 2>&1 || STATUS=$?
if [ ${STATUS} -ne 0 ]; then
    echo "ОШИБКА: собранная игра завершилась с кодом ${STATUS}"; cat "${PLAYER_LOG}"; exit 1
fi
SHOT_SIZE=$(stat -c%s "${PLAYER_SHOT}" 2>/dev/null || echo 0)
if [ "${SHOT_SIZE}" -lt 1024 ]; then
    echo "ОШИБКА: скриншот собранной игры отсутствует или подозрительно мал (${SHOT_SIZE} байт)"
    cat "${PLAYER_LOG}"; exit 1
fi
if ! grep -q "PLAYER: started" "${PLAYER_LOG}"; then
    echo "ОШИБКА: в логе плеера нет маркера 'PLAYER: started'"
    cat "${PLAYER_LOG}"; exit 1
fi
echo "OK: собранная игра запустилась и отрисовала кадр (скриншот ${SHOT_SIZE} байт)"

# Проект собранной игры лежит ПАКЕТОМ, а не россыпью: россыпь означала бы
# медленный старт, игру, открываемую блокнотом, и лишний размер.
if [ ! -f "${GAME_DIR}/game.sagepak" ]; then
    echo "ОШИБКА: в собранной игре нет пакета game.sagepak"; ls -la "${GAME_DIR}"; exit 1
fi
if [ -d "${GAME_DIR}/project" ]; then
    echo "ОШИБКА: рядом с игрой осталась распакованная папка project/ —"
    echo "        значит сборка положила и пакет, и россыпь, и что из них прочтут, дело случая"
    exit 1
fi
PACK_SIZE=$(stat -c%s "${GAME_DIR}/game.sagepak")
echo "OK: проект упакован в game.sagepak (${PACK_SIZE} байт), россыпи рядом нет"

# --- Смена сцены из скрипта -------------------------------------------------
#
# Отдельным проектом НА ДИСКЕ, а не правкой собранной игры: содержимое той
# теперь в пакете, дописать в него второй уровень нечем. Заодно это проверяет
# вторую половину vfs — что игра одинаково читается и без пакета.
# Копируем ВСЮ собранную игру и убираем из неё пакет: плееру нужны его
# собственные шейдеры рядом с exe (см. sage/core/Paths.h), а не только сцены.
SWITCH_DIR="${SCRATCH_DIR}/twoscene"
rm -rf "${SWITCH_DIR}"
cp -r "${GAME_DIR}" "${SWITCH_DIR}"
mkdir -p "${SWITCH_DIR}/scenes" "${SWITCH_DIR}/assets/scripts"
python3 - "${GAME_DIR}/game.sagepak" "${SWITCH_DIR}" <<'PYEOF'
import json, pathlib, struct, sys
pack, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])

# Достаём сцену из пакета — тем же форматом, которым его пишет движок.
# Разбор здесь СВОЙ и намеренно: если формат разъедется с описанием, тест
# упадёт, а не «просто не найдёт сцену».
import zlib
data = pack.read_bytes()
count = struct.unpack_from("<I", data, 8)[0]
index_off = struct.unpack_from("<Q", data, 16)[0]
p = index_off
scene = None
for _ in range(count):
    n = struct.unpack_from("<I", data, p)[0]; p += 4
    name = data[p:p+n].decode(); p += n
    off, stored, orig, comp = struct.unpack_from("<QQQI", data, p); p += 28
    if name.endswith(".sage") and scene is None:
        raw = data[off:off+stored]
        scene = json.loads(zlib.decompress(raw) if comp else raw)
assert scene is not None, "в пакете нет ни одной сцены"

level2 = json.loads(json.dumps(scene)); level2["name"] = "level2"
(out / "scenes" / "level2.sage").write_text(json.dumps(level2, indent=2))

(out / "assets" / "scripts" / "switcher.lua").write_text(
    "local frames = 0\n"
    "function OnUpdate(entity, dt)\n"
    "    frames = frames + 1\n"
    "    if frames == 3 then sage.scene.Load('level2') end\n"
    "end\n")

key = "objects" if "objects" in scene else "entities"
scene["name"] = "main"
scene[key].append({
    "name": "Switcher", "id": 9999,
    "transform": {"position": {"x": 0, "y": 0, "z": 0},
                  "rotation": {"x": 0, "y": 0, "z": 0},
                  "scale": {"x": 1, "y": 1, "z": 1}},
    "script": "assets/scripts/switcher.lua"})
(out / "scenes" / "main.sage").write_text(json.dumps(scene, indent=2))
(out / "project.sageproj").write_text(json.dumps({"sage_project_version": 1, "name": "twoscene"}))
PYEOF
rm -f "${SWITCH_DIR}/game.sagepak"   # тут проверяется путь БЕЗ пакета
# Диагностика на случай отказа: без неё «смена сцены не сработала» не говорит,
# что именно не так — не собрался проект, не удалился пакет или запустилось не то.
echo "  проверка: ${SWITCH_DIR}"
ls -1 "${SWITCH_DIR}" | sed 's/^/    /'
ls -1 "${SWITCH_DIR}/scenes" 2>/dev/null | sed 's/^/    scenes\//' || echo "    (нет scenes/)"
SWITCH_LOG="${SCRATCH_DIR}/twoscene.log"
SWITCH_EXE="$(basename "${GAME_EXE}")"
STATUS=0
( cd "${SWITCH_DIR}" && run_headless env SAGE_SCREENSHOT_AT_FRAME=10 \
      SAGE_SCREENSHOT_PATH="${SCRATCH_DIR}/twoscene.png" "./${SWITCH_EXE}" . ) \
    > "${SWITCH_LOG}" 2>&1 || STATUS=$?
if [ ${STATUS} -ne 0 ]; then
    echo "ОШИБКА: игра с двумя сценами завершилась с кодом ${STATUS}"; cat "${SWITCH_LOG}"; exit 1
fi
if ! grep -q "сцена: level2.sage" "${SWITCH_LOG}"; then
    echo "ОШИБКА: смена сцены из скрипта не сработала (нет перехода в level2)"
    cat "${SWITCH_LOG}"; exit 1
fi
echo "OK: смена сцены из скрипта работает (main.sage -> level2.sage)"

# ...и ещё раз — ИЗ ЧУЖОЙ ПАПКИ, по пути к самому project.sageproj. Так игру и
# запускают на самом деле: ярлыком, из другого каталога, перетаскиванием файла
# проекта. До появления sage/core/Paths.h такой запуск умирал на первом же
# шейдере («не удалось открыть файл шейдера: assets/shaders/lit.vert»), потому
# что плеер искал СВОИ ресурсы от текущей папки, а не от места установки, — и
# вдобавок не понимал путь к файлу проекта, требуя непременно папку.
PLAYER_LOG2="${SCRATCH_DIR}/player_elsewhere.log"
PLAYER_SHOT2="${SCRATCH_DIR}/player_elsewhere.png"
GAME_ABS=$(cd "${GAME_DIR}" && pwd)
STATUS=0
( cd "${SCRATCH_DIR}" && \
  run_headless env SAGE_SCREENSHOT_AT_FRAME=10 SAGE_SCREENSHOT_PATH="${PLAYER_SHOT2}" \
      "${GAME_ABS}/selftest_project" "${GAME_ABS}/project.sageproj" ) \
  > "${PLAYER_LOG2}" 2>&1 || STATUS=$?
if [ ${STATUS} -ne 0 ]; then
    echo "ОШИБКА: запуск игры из другой папки (по пути к .sageproj) завершился с кодом ${STATUS}"
    cat "${PLAYER_LOG2}"; exit 1
fi
if ! grep -q "PLAYER: started" "${PLAYER_LOG2}"; then
    echo "ОШИБКА: игра, запущенная из другой папки, не стартовала"
    cat "${PLAYER_LOG2}"; exit 1
fi
SHOT_SIZE2=$(stat -c%s "${PLAYER_SHOT2}" 2>/dev/null || echo 0)
if [ "${SHOT_SIZE2}" -lt 1024 ]; then
    echo "ОШИБКА: игра из другой папки не отрисовала кадр (скриншот ${SHOT_SIZE2} байт)"
    cat "${PLAYER_LOG2}"; exit 1
fi
echo "OK: игра запускается из ЛЮБОЙ папки и понимает путь к project.sageproj (скриншот ${SHOT_SIZE2} байт)"

# Пост-обработка в СОБРАННОЙ ИГРЕ, а не только в редакторе. Движок объявляет её
# подсистемой, конфиг её настраивает, окно Game редактора её показывает — а
# плеер рисовал прямо в экран и не выполнял её НИ РАЗУ: игра в редакторе и та же
# игра, запущенная по-настоящему, выглядели по-разному. Проверка простая и
# неубиваемая: один и тот же кадр с SAGE_POST=0 и SAGE_POST=1 обязан
# ОТЛИЧАТЬСЯ. Пока цепочка не подключена, оба кадра совпадают до байта.
POST_OFF="${SCRATCH_DIR}/post_off.png"
POST_ON="${SCRATCH_DIR}/post_on.png"
for MODE in 0 1; do
    OUT="${POST_OFF}"; [ "${MODE}" = "1" ] && OUT="${POST_ON}"
    STATUS=0
    ( cd "${GAME_DIR}" && \
      run_headless env SAGE_POST="${MODE}" SAGE_SCREENSHOT_AT_FRAME=10 \
          SAGE_SCREENSHOT_PATH="${OUT}" ./selftest_project ) > /dev/null 2>&1 || STATUS=$?
    if [ ${STATUS} -ne 0 ]; then
        echo "ОШИБКА: игра с SAGE_POST=${MODE} завершилась с кодом ${STATUS}"; exit 1
    fi
done
if cmp -s "${POST_OFF}" "${POST_ON}"; then
    echo "ОШИБКА: кадр с пост-обработкой и без неё СОВПАДАЕТ — цепочка эффектов"
    echo "        не выполняется в собранной игре (см. PlayerLayer::OnRender)."
    exit 1
fi
echo "OK: пост-обработка выполняется в собранной игре (кадры с SAGE_POST=0/1 различаются)"

echo "=== Smoke-тест 6/9: E2E — игра с Lua-логикой создаётся В РЕДАКТОРЕ, играется и собирается в exe ==="
# Редактор (SAGE_EDITOR_E2E=1) сам создаёт проект «Coin Rush»: пишет три Lua-
# скрипта (бот-сборщик, монеты с OnMessage, HUD-счёт из Lua), строит сцену,
# сохраняет и перечитывает .sage, проигрывает её в Play (проверяя, что бот
# реально собрал все монеты и HUD показывает 5/5), затем File > Build Game.
E2E_LOG="${SCRATCH_DIR}/e2e_editor.log"
STATUS=0
( cd "$(dirname "${EDITOR_EXE}")" && rm -rf e2e_game e2e_dist && \
  run_headless env SAGE_EDITOR_E2E=1 SAGE_SCREENSHOT_AT_FRAME=10 SAGE_SCREENSHOT_PATH="${SCRATCH_DIR}/e2e_ed.png" \
      "./$(basename "${EDITOR_EXE}")" ) > "${E2E_LOG}" 2>&1 || STATUS=$?
if [ ${STATUS} -ne 0 ] || ! grep -q "E2E: PASS" "${E2E_LOG}"; then
    echo "ОШИБКА: E2E-сценарий редактора не прошёл (код ${STATUS})"; cat "${E2E_LOG}"; exit 1
fi
if ! grep -q "E2E: ALL COINS COLLECTED" "${E2E_LOG}"; then
    echo "ОШИБКА: в Play-режиме редактора Lua-логика не собрала монеты"; cat "${E2E_LOG}"; exit 1
fi
# Теперь СОБРАННЫЙ редактором бинарник: та же Lua-логика обязана отработать
# в готовой игре (реальный процесс, реальное время, скриншот с HUD).
E2E_GAME_DIR="${BUILD_DIR}/editor/e2e_dist/e2e_game"
if [ ! -x "${E2E_GAME_DIR}/e2e_game" ]; then
    echo "ОШИБКА: e2e-игра не собрана: ${E2E_GAME_DIR}/e2e_game"; exit 1
fi
E2E_GAME_LOG="${SCRATCH_DIR}/e2e_game.log"
E2E_GAME_SHOT="${SCRATCH_DIR}/e2e_game.png"
STATUS=0
( cd "${E2E_GAME_DIR}" && \
  run_headless env SAGE_SCREENSHOT_AT_FRAME=400 SAGE_SCREENSHOT_PATH="${E2E_GAME_SHOT}" \
      ./e2e_game ) > "${E2E_GAME_LOG}" 2>&1 || STATUS=$?
if [ ${STATUS} -ne 0 ]; then
    echo "ОШИБКА: e2e-игра завершилась с кодом ${STATUS}"; cat "${E2E_GAME_LOG}"; exit 1
fi
if ! grep -q "E2E: ALL COINS COLLECTED" "${E2E_GAME_LOG}"; then
    echo "ОШИБКА: в СОБРАННОЙ игре Lua-логика не собрала все монеты"; cat "${E2E_GAME_LOG}"; exit 1
fi
COLLECTED=$(grep -c "E2E: coin collected" "${E2E_GAME_LOG}")
SHOT_SIZE=$(stat -c%s "${E2E_GAME_SHOT}" 2>/dev/null || echo 0)
if [ "${SHOT_SIZE}" -lt 1024 ]; then
    echo "ОШИБКА: скриншот e2e-игры отсутствует или подозрительно мал (${SHOT_SIZE} байт)"; exit 1
fi
echo "OK: E2E — редактор создал игру с Lua-логикой, сыграл её, собрал exe; собранный exe собрал ${COLLECTED}/5 монет (скриншот ${SHOT_SIZE} байт)"

echo "=== Smoke-тест 7/9: обработчик падений (настоящее падение) ==="
# Единственный честный способ проверить обработчик падений — уронить процесс.
# Обычным тестом это не сделать: обработчик по замыслу доводит падение до
# конца и убивает процесс, а вместе с ним весь прогон. Поэтому падение вынесено
# в отдельную программу (tests/crash_probe.cpp), и проверяется здесь.
# Путь абсолютный: запускаем из каталога отчётов, и относительный
# указывал бы уже не туда.
PROBE_EXE="$(cd "$(dirname "${BUILD_DIR}/tests/sage_crash_probe")" && pwd)/sage_crash_probe"
if [ ! -x "${PROBE_EXE}" ]; then
    echo "ОШИБКА: ${PROBE_EXE} не собран"; exit 1
fi
CRASH_DIR="${SCRATCH_DIR}/crash"
rm -rf "${CRASH_DIR}"; mkdir -p "${CRASH_DIR}"

for MODE in segv terminate; do
    STATUS=0
    ( cd "${CRASH_DIR}" && "${PROBE_EXE}" "${MODE}" "${CRASH_DIR}" ) > "${CRASH_DIR}/${MODE}.log" 2>&1 || STATUS=$?
    # Падение обязано ОСТАТЬСЯ падением: обработчик пишет отчёт и добивает
    # процесс тем же сигналом. Проглоти он падение — отладчик и системный
    # сборщик отчётов его бы не увидели, а код возврата соврал бы сборке.
    if [ ${STATUS} -lt 128 ]; then
        echo "ОШИБКА: ${MODE} завершился кодом ${STATUS} — падение проглочено"; exit 1
    fi
    REPORT=$(ls -t "${CRASH_DIR}"/sage-crash-*.txt 2>/dev/null | head -1)
    if [ -z "${REPORT}" ]; then
        echo "ОШИБКА: после ${MODE} отчёт о падении не создан"; cat "${CRASH_DIR}/${MODE}.log"; exit 1
    fi
    # В отчёте должно быть то, ради чего он пишется: причина, контекст от
    # приложения, стек и хвост лога. Отчёт без них бесполезен так же, как его
    # отсутствие.
    for MARKER in "МАРКЕР-КОНТЕКСТА" "Стек вызовов" "МАРКЕР-ЛОГА"; do
        if ! grep -q "${MARKER}" "${REPORT}"; then
            echo "ОШИБКА: в отчёте (${MODE}) нет '${MARKER}'"; cat "${REPORT}"; exit 1
        fi
    done
    rm -f "${CRASH_DIR}"/sage-crash-*.txt
done
# Аварийное сохранение: работа не должна теряться вместе с процессом.
if [ ! -f "${CRASH_DIR}/crashprobe-recovered.txt" ]; then
    echo "ОШИБКА: аварийное сохранение не сработало"; exit 1
fi
echo "OK: падение перехвачено (segv и terminate), отчёт со стеком и контекстом записан, работа сохранена"

echo "=== Smoke-тест 8/9: отказ запуска слышно ==="
# «Просто запускаю — ничего не происходит, даже ошибки нет, а в логе только
# строка о старте». Так выглядел ЛЮБОЙ отказ запуска: причина уходила в stderr,
# которого у окна Windows нет, и в лог не попадала вовсе.
#
# Проверяем самым честным способом — запускаем редактор БЕЗ дисплея. Это тот же
# путь, что и «драйвер не умеет OpenGL 3.3» на чужом ПК: окно не создаётся,
# наружу летит исключение.
STARTFAIL_DIR="${SCRATCH_DIR}/startfail"
rm -rf "${STARTFAIL_DIR}"; mkdir -p "${STARTFAIL_DIR}"
STATUS=0
# Путь АБСОЛЮТНЫЙ: заходим в чужую папку, и относительный тут превратился бы
# в «команда не найдена» — то есть проверка мерила бы не то.
EDITOR_ABS="$(cd "$(dirname "${EDITOR_EXE}")" && pwd)/$(basename "${EDITOR_EXE}")"
( cd "${STARTFAIL_DIR}" && env -u DISPLAY -u WAYLAND_DISPLAY \
    "${EDITOR_ABS}" ) > "${STARTFAIL_DIR}/out.txt" 2>&1 || STATUS=$?
if [ ${STATUS} -eq 0 ]; then
    echo "ОШИБКА: редактор без дисплея завершился успешно — отказ не обнаружен"; exit 1
fi
if [ ! -f "${STARTFAIL_DIR}/sage_editor.log" ]; then
    echo "ОШИБКА: лог не создан — причину отказа человеку взять неоткуда"; exit 1
fi
# В логе обязаны быть ДВЕ вещи: слова самого GLFW (почему именно) и отметка о
# фатальном отказе (что запуск не состоялся). Без первого причина неизвестна,
# без второго лог выглядит просто оборванным.
if ! grep -q "ФАТАЛЬНАЯ ОШИБКА ПРИ ЗАПУСКЕ" "${STARTFAIL_DIR}/sage_editor.log"; then
    echo "ОШИБКА: фатальный отказ не записан в лог"; cat "${STARTFAIL_DIR}/sage_editor.log"; exit 1
fi
if ! grep -q "GLFW" "${STARTFAIL_DIR}/sage_editor.log"; then
    echo "ОШИБКА: в логе нет объяснения от GLFW — причина отказа потеряна"
    cat "${STARTFAIL_DIR}/sage_editor.log"; exit 1
fi
echo "OK: отказ запуска записан в лог с причиной от GLFW (код ${STATUS})"

echo "=== Smoke-тест 9/9: путь с кириллицей (имя пользователя по-русски) ==="
# ЭТО ПРОВЕРКА ТОЙ САМОЙ ОШИБКИ, из-за которой редактор не запускался на
# русской Windows: «filesystem error: Cannot convert character sequence».
# Причина — путь с кириллицей, пришедший из окружения (APPDATA у пользователя
# «Владимир»). Ловим её сразу с трёх сторон: настройки редактора, папка
# проекта и аргумент командной строки плеера — все три содержат кириллицу.
#
# На Linux кодировка пути ничего не ломает сама по себе, и падения здесь ждать
# неоткуда. Проверка всё равно нужна: тот же код собирается для Windows, и
# сломать его правкой, проверенной только на Linux, — ровно то, как эта ошибка
# и появилась. Статическую половину стережёт scripts/check_paths.py.
UNI_ROOT="${SCRATCH_DIR}/Пользователи/Владимир"
UNI_PROJECT="${UNI_ROOT}/Документы/SAGE Projects/Моя игра"
mkdir -p "${UNI_ROOT}/.config" "${UNI_PROJECT}"
UNI_LOG="${SCRATCH_DIR}/unicode_editor.log"
STATUS=0
# HOME и XDG_CONFIG_HOME с кириллицей — это и есть аналог APPDATA у
# «Владимира»: редактор кладёт по ним свои настройки и список проектов.
( cd "$(dirname "${EDITOR_EXE}")" && \
  run_headless env HOME="${UNI_ROOT}" XDG_CONFIG_HOME="${UNI_ROOT}/.config" \
      SAGE_EDITOR_SELFTEST=1 SAGE_EDITOR_LANG=ru \
      "./$(basename "${EDITOR_EXE}")" ) > "${UNI_LOG}" 2>&1 || STATUS=$?
if [ ${STATUS} -ne 0 ]; then
    echo "ОШИБКА: редактор с кириллицей в HOME завершился кодом ${STATUS}"
    cat "${UNI_LOG}"; exit 1
fi
if grep -q "ФАТАЛЬНАЯ ОШИБКА ПРИ ЗАПУСКЕ" "${UNI_LOG}"; then
    echo "ОШИБКА: кириллица в пути обрушила запуск редактора"
    grep -n "ФАТАЛЬНАЯ" "${UNI_LOG}"; exit 1
fi
if ! grep -q "SELFTEST: PASS" "${UNI_LOG}"; then
    echo "ОШИБКА: self-test редактора не прошёл при кириллице в HOME"
    cat "${UNI_LOG}"; exit 1
fi
# Настройки действительно легли по русскому пути — значит путь дожил до записи,
# а не был молча заменён на запасной.
if [ ! -f "${UNI_ROOT}/.config/sage/editor_prefs.json" ]; then
    echo "ОШИБКА: настройки не записались в папку с кириллицей"
    find "${UNI_ROOT}" -type f | head; exit 1
fi

# Вторая половина: СОБРАННАЯ ИГРА лежит по пути с кириллицей, и путь к её
# проекту приходит АРГУМЕНТОМ командной строки. На Windows argv отдаётся в
# ANSI-кодировке, и до NormalizeArgs (см. GameModule.h) первая же строка
# разбора аргументов убивала плеер: игру из русской папки нельзя было
# запустить вовсе — ни из редактора кнопкой Play, ни ярлыком.
UNI_GAME="${UNI_PROJECT}/сборка"
mkdir -p "${UNI_GAME}"
cp -r "${GAME_DIR}/." "${UNI_GAME}/"
UNI_PLAYER_LOG="${SCRATCH_DIR}/unicode_player.log"
UNI_SHOT="${UNI_PROJECT}/кадр.png"
STATUS=0
# Запуск ИЗ ДРУГОЙ ПАПКИ с путём аргументом — так игру и запускают ярлыком.
( cd "${SCRATCH_DIR}" && \
  run_headless env SAGE_SCREENSHOT_AT_FRAME=10 SAGE_SCREENSHOT_PATH="${UNI_SHOT}" \
      "${UNI_GAME}/selftest_project" "${UNI_GAME}" ) > "${UNI_PLAYER_LOG}" 2>&1 || STATUS=$?
if [ ${STATUS} -ne 0 ]; then
    echo "ОШИБКА: игра по пути с кириллицей завершилась кодом ${STATUS}"
    cat "${UNI_PLAYER_LOG}"; exit 1
fi
if grep -q "ФАТАЛЬНАЯ ОШИБКА ПРИ ЗАПУСКЕ" "${UNI_PLAYER_LOG}"; then
    echo "ОШИБКА: кириллица в пути обрушила запуск игры"
    grep -n "ФАТАЛЬНАЯ" "${UNI_PLAYER_LOG}"; exit 1
fi
UNI_SHOT_SIZE=$(stat -c%s "${UNI_SHOT}" 2>/dev/null || echo 0)
if [ "${UNI_SHOT_SIZE}" -lt 1024 ]; then
    echo "ОШИБКА: игра из папки с кириллицей не отрисовала кадр (${UNI_SHOT_SIZE} байт)"
    cat "${UNI_PLAYER_LOG}"; exit 1
fi
echo "OK: кириллица в пути работает везде — настройки редактора, папка проекта,"
echo "    аргумент командной строки и запись кадра (${UNI_SHOT_SIZE} байт)"

echo "=== Все smoke-тесты прошли ==="
