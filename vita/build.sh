#!/bin/bash
# Builds r6sv.vpk with VitaSDK. From Windows:  wsl bash vita/build.sh
set -e
export VITASDK=${VITASDK:-/usr/local/vitasdk}
export PATH=$VITASDK/bin:$PATH
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD=${BUILD:-$HOME/r6sv-vita-build}  # outside /mnt: much faster than building on the Windows drive
cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j"$(nproc)"
cp "$BUILD/r6sv.vpk" "$SRC/r6sv.vpk"
echo "built $SRC/r6sv.vpk"
