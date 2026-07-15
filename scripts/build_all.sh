#!/usr/bin/env bash
# Собирает готовые пакеты под Linux и Windows одной командой.
#
# Использование:
#   ./scripts/build_all.sh
#   ./scripts/build_all.sh MyOtherGame
set -e
cd "$(dirname "$0")"

GAME_NAME="${1:-TheBoat}"

./build_linux.sh "${GAME_NAME}"
echo ""
./build_windows.sh "${GAME_NAME}"

echo ""
echo "=== Все пакеты готовы ==="
echo "Linux:   dist/linux/"
echo "Windows: dist/windows/"
