#!/usr/bin/env bash
# Build custom-clipboard. Produces ./custom-clipboard in the project root.
set -euo pipefail

cd "$(dirname "$0")"

QT_MODULES="Qt6Widgets Qt6DBus Qt6Sql"
KGA_INC="${KF6_GLOBALACCEL_INCLUDE_DIR:-/usr/include/KF6/KGlobalAccel}"

missing=0
for m in $QT_MODULES; do
    if ! pkg-config --exists "$m"; then
        echo "ERROR: missing dev package for $m" >&2
        echo "  Arch: sudo pacman -S qt6-base   (provides Qt6Widgets/Qt6DBus/Qt6Sql)" >&2
        missing=1
    fi
done

if [ ! -f "$KGA_INC/kglobalaccel.h" ]; then
    echo "ERROR: KGlobalAccel headers not found at $KGA_INC" >&2
    echo "  Arch: sudo pacman -S kglobalaccel" >&2
    echo "  Custom prefix: set KF6_GLOBALACCEL_INCLUDE_DIR to the header directory" >&2
    missing=1
fi

if ! command -v g++ >/dev/null 2>&1; then
    echo "ERROR: g++ not found.  Arch: sudo pacman -S gcc" >&2
    missing=1
fi

[ "$missing" -eq 0 ] || exit 1

CXXFLAGS="-std=c++17 -O2 -fPIC -Wall -Wextra -Wno-unused-parameter"
QT_CFLAGS="$(pkg-config --cflags $QT_MODULES)"
QT_LIBS="$(pkg-config --libs $QT_MODULES)"

# shellcheck disable=SC2086
g++ $CXXFLAGS src/main.cpp -o custom-clipboard \
    $QT_CFLAGS -I"$KGA_INC" $QT_LIBS -lKF6GlobalAccel

echo "built: $(pwd)/custom-clipboard"
