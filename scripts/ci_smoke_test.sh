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

echo "=== Smoke-тест 1/6: Sandbox (рендер сцены + скриптинг) ==="
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

echo "=== Smoke-тест 2/6: SageEditor (self-test: проект+сцена+undo/redo+play) ==="
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

echo "=== Smoke-тест 3/6: плагины редактора (opt-in, SAGE_EDITOR_PLUGINS=1) ==="
if ! grep -q "Загружен плагин: Example Stats" "${EDITOR_LOG}"; then
    echo "ОШИБКА: плагин example_stats не загрузился при SAGE_EDITOR_PLUGINS=1"
    cat "${EDITOR_LOG}"; exit 1
fi
echo "OK: плагин example_stats загрузился и выгрузился без падения (плагины — opt-in)"

echo "=== Smoke-тест 4/6: TestGame (боевая игра: автопилот собирает монеты и проходит портал) ==="
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

echo "=== Smoke-тест 5/6: собранная игра (SagePlayer + проект из редактора) ==="
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

echo "=== Smoke-тест 6/6: E2E — игра с Lua-логикой создаётся В РЕДАКТОРЕ, играется и собирается в exe ==="
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

echo "=== Все smoke-тесты прошли ==="
