#!/bin/sh
# build_sdl3.sh <SDL3 source dir> <install prefix>: SDL3 for the 3DS, inside the port/3ds/Dockerfile image.
# -fno-short-enums: the game is built with clang's int-sized enums, and shares SDL's structs (SDL_AudioSpec).
set -e
SRC=$1; PREFIX=$2; B=$PREFIX/build
CFLAGS="$CFLAGS -fno-short-enums" cmake -S "$SRC" -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DSDL_SHARED=OFF -DSDL_STATIC=ON \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
ninja -C "$B"
cmake --install "$B"
