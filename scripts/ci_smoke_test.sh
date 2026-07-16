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

echo "=== Smoke-тест 1/3: Sandbox (рендер сцены + скриптинг) ==="
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

echo "=== Smoke-тест 2/3: SageEditor (self-test: проект+сцена+undo/redo+play) ==="
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

echo "=== Smoke-тест 3/3: плагины редактора (example_stats) ==="
if ! grep -q "Загружен плагин: Example Stats" "${EDITOR_LOG}"; then
    echo "ОШИБКА: плагин example_stats не загрузился (нет строки 'Загружен плагин' в логе)"
    cat "${EDITOR_LOG}"; exit 1
fi
echo "OK: плагин example_stats загрузился и выгрузился без падения"

echo "=== Все smoke-тесты прошли ==="
