#!/usr/bin/env bash
# Собирает игру под Linux и упаковывает в готовый к раздаче архив:
# dist/<GameName>-<version>-linux-x64.tar.gz
#
# Использование:
#   ./scripts/build_linux.sh              # имя игры = TheBoat (по умолчанию)
#   ./scripts/build_linux.sh MyOtherGame  # собрать другую игру этим же движком
set -e

cd "$(dirname "$0")/.."  # переходим в корень engine/

GAME_NAME="${1:-TheBoat}"
VERSION=$(cat VERSION 2>/dev/null || echo "0.0.0")
BUILD_DIR="build-linux"
PACKAGE_NAME="${GAME_NAME}-${VERSION}-linux-x64"
PACKAGE_DIR="dist/linux/${PACKAGE_NAME}"

echo "=== Собираю ${GAME_NAME} v${VERSION} под Linux ==="

cmake -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DGAME_NAME="${GAME_NAME}"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "=== Упаковываю ==="
rm -rf "${PACKAGE_DIR}"
mkdir -p "${PACKAGE_DIR}"
cp "${BUILD_DIR}/${GAME_NAME}" "${PACKAGE_DIR}/"
cp -r assets "${PACKAGE_DIR}/"

cd dist/linux
tar -czf "${PACKAGE_NAME}.tar.gz" "${PACKAGE_NAME}"
cd - > /dev/null

echo ""
echo "Готово: dist/linux/${PACKAGE_NAME}.tar.gz"
echo "Запуск: tar -xzf ${PACKAGE_NAME}.tar.gz && cd ${PACKAGE_NAME} && ./${GAME_NAME}"
