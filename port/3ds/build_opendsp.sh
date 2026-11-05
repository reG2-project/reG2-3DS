#!/bin/sh
# build_opendsp.sh <OpenDSP source dir> <install prefix>: libopendsp.a, the DSP firmware that replaces the console's
# dspfirm.cdc (https://github.com/Cellenseres/OpenDSP, BSD-2), inside the port/3ds/Dockerfile image.
# The firmware and its tables are OpenDSP's prebuilt ones: rebuilding them needs its teak-llvm toolchain.
set -e
SRC=$1; PREFIX=$2; B=$PREFIX/build
rm -rf "$B"
mkdir -p "$B" "$PREFIX/lib" "$PREFIX/include"
cp -r "$SRC/library" "$SRC/firmware" "$B/"
make -C "$B/library" -o ../firmware/mp3_tables.bin
cp "$B/library/lib/libopendsp.a" "$PREFIX/lib/"
cp -r "$B/library/include/." "$PREFIX/include/"
cp "$SRC/LICENSE" "$PREFIX/LICENSE"
