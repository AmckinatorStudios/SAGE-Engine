#!/usr/bin/env bash
# Кросс-компилирует игру под Windows (.exe) из Linux и упаковывает в
# готовый к раздаче архив: dist/<GameName>-<version>-windows-x64.zip
#
# Требует: sudo apt install g++-mingw-w64-x86-64
#
# Использование:
#   ./scripts/build_windows.sh
#   ./scripts/build_windows.sh MyOtherGame
set -e

cd "$(dirname "$0")/.."

GAME_NAME="${1:-TheBoat}"
VERSION=$(cat VERSION 2>/dev/null || echo "0.0.0")
BUILD_DIR="build-windows"
PACKAGE_NAME="${GAME_NAME}-${VERSION}-windows-x64"
PACKAGE_DIR="dist/windows/${PACKAGE_NAME}"

if ! command -v x86_64-w64-mingw32-g++ &> /dev/null; then
    echo "Ошибка: не найден кросс-компилятор x86_64-w64-mingw32-g++"
    echo "Установи: sudo apt install g++-mingw-w64-x86-64"
    exit 1
fi

echo "=== Собираю ${GAME_NAME} v${VERSION} под Windows (кросс-компиляция) ==="

cmake -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DGAME_NAME="${GAME_NAME}"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "=== Упаковываю ==="
rm -rf "${PACKAGE_DIR}"
mkdir -p "${PACKAGE_DIR}"
cp "${BUILD_DIR}/${GAME_NAME}.exe" "${PACKAGE_DIR}/"
cp -r assets "${PACKAGE_DIR}/"

cd dist/windows
zip -r -q "${PACKAGE_NAME}.zip" "${PACKAGE_NAME}"
cd - > /dev/null

echo ""
echo "Готово: dist/windows/${PACKAGE_NAME}.zip"
echo "Просто распаковать и запустить ${GAME_NAME}.exe — DLL не нужны (статическая линковка)"
