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

echo "=== Smoke-тест 1/5: Sandbox (рендер сцены + скриптинг) ==="
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

echo "=== Smoke-тест 2/5: SageEditor (self-test: проект+сцена+undo/redo+play) ==="
if [ ! -x "${EDITOR_EXE}" ]; then
    echo "ОШИБКА: не найден собранный бинарник ${EDITOR_EXE}"
    exit 1
fi
EDITOR_LOG="${SCRATCH_DIR}/editor.log"
EDITOR_SHOT="${SCRATCH_DIR}/editor.png"
STATUS=0
( cd "$(dirname "${EDITOR_EXE}")" && rm -rf selftest_project && \
  run_headless env SAGE_EDITOR_SELFTEST=1 SAGE_SCREENSHOT_AT_FRAME=10 SAGE_SCREENSHOT_PATH="${EDITOR_SHOT}" \
      "./$(basename "${EDITOR_EXE}")" ) > "${EDITOR_LOG}" 2>&1 || STATUS=$?
if [ ${STATUS} -ne 0 ]; then
    echo "ОШИБКА: SageEditor завершился с кодом ${STATUS}"; cat "${EDITOR_LOG}"; exit 1
fi
if ! grep -q "SELFTEST: PASS" "${EDITOR_LOG}"; then
    echo "ОШИБКА: self-test редактора не прошёл (нет 'SELFTEST: PASS' в логе)"
    cat "${EDITOR_LOG}"; exit 1
fi
echo "OK: SageEditor self-test прошёл"

echo "=== Smoke-тест 3/5: плагины редактора (example_stats) ==="
if ! grep -q "Загружен плагин: Example Stats" "${EDITOR_LOG}"; then
    echo "ОШИБКА: плагин example_stats не загрузился (нет строки 'Загружен плагин' в логе)"
    cat "${EDITOR_LOG}"; exit 1
fi
echo "OK: плагин example_stats загрузился и выгрузился без падения"

echo "=== Smoke-тест 4/5: TestGame (боевая игра: автопилот собирает монеты и проходит портал) ==="
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
              "Font] Загружен шрифт" "TESTGAME: physics backend"; do
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

echo "=== Smoke-тест 5/5: собранная игра (SagePlayer + проект из редактора) ==="
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

echo "=== Все smoke-тесты прошли ==="
