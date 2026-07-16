#!/usr/bin/env bash
# Собирает игру под Linux и упаковывает в готовый к раздаче архив:
# dist/<GameName>-<version>-linux-x64.tar.gz
#
# Использование:
#   ./scripts/build_linux.sh              # имя игры = Sandbox (по умолчанию)
#   ./scripts/build_linux.sh MyOtherGame  # собрать другую игру этим же движком
set -e

cd "$(dirname "$0")/.."  # переходим в корень engine/

GAME_NAME="${1:-Sandbox}"
VERSION=$(cat VERSION 2>/dev/null || echo "0.0.0")
BUILD_DIR="build-linux"
PACKAGE_NAME="${GAME_NAME}-${VERSION}-linux-x64"
PACKAGE_DIR="dist/linux/${PACKAGE_NAME}"

echo "=== Собираю ${GAME_NAME} v${VERSION} под Linux ==="

# Имя игрового таргета задаётся в его games/<name>/CMakeLists.txt (sage_add_game),
# поэтому -DGAME_NAME больше не нужен — просто собираем нужный таргет по имени.
cmake -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" -j"$(nproc)" --target "${GAME_NAME}"

echo "=== Упаковываю ==="
# Бинарник игры и её ассеты лежат вместе в build/games/<name>/ (ассеты кладёт
# туда POST_BUILD-шаг sage_add_game) — пакуем прямо оттуда.
EXE=$(find "${BUILD_DIR}" -type f -name "${GAME_NAME}" | head -1)
if [ -z "${EXE}" ]; then
    echo "Ошибка: не найден собранный бинарник ${GAME_NAME} в ${BUILD_DIR}"
    exit 1
fi
EXE_DIR=$(dirname "${EXE}")
rm -rf "${PACKAGE_DIR}"
mkdir -p "${PACKAGE_DIR}"
cp "${EXE}" "${PACKAGE_DIR}/"
cp -r "${EXE_DIR}/assets" "${PACKAGE_DIR}/"

cd dist/linux
tar -czf "${PACKAGE_NAME}.tar.gz" "${PACKAGE_NAME}"
cd - > /dev/null

echo ""
echo "Готово: dist/linux/${PACKAGE_NAME}.tar.gz"
echo "Запуск: tar -xzf ${PACKAGE_NAME}.tar.gz && cd ${PACKAGE_NAME} && ./${GAME_NAME}"
