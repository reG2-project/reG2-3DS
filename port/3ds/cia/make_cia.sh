#!/bin/sh
# make_cia.sh <build dir> <makerom> <bannertool> [out.cia]: packs the build's gta2.elf, with its gta2.smdh (the
# .3dsx's title and icon) and this folder's banner, into a CIA for the HOME Menu (gta2.cia in the build dir).
# The CIA's title version is the build's GTA2_VERSION x.y.z (x and y at most 63, z at most 15).
set -e
if [ $# -lt 3 ]; then
    echo "usage: $0 <build dir> <makerom> <bannertool> [out.cia]" >&2
    exit 1
fi
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=$1; MAKEROM=$2; BANNERTOOL=$3; OUT=${4:-$BUILD/gta2.cia}
for f in "$BUILD/gta2.elf" "$BUILD/gta2.smdh" "$BUILD/CMakeCache.txt"; do
    [ -f "$f" ] || { echo "$f missing: build the .3dsx first" >&2; exit 1; }
done
VERSION=$(sed -n 's/^GTA2_VERSION:STRING=//p' "$BUILD/CMakeCache.txt")
MAJOR=$(echo "$VERSION" | cut -d. -f1)
MINOR=$(echo "$VERSION" | cut -d. -f2)
MICRO=$(echo "$VERSION" | cut -d. -f3 | sed 's/[^0-9].*//')
if ! [ "$MAJOR" -le 63 ] 2> /dev/null || ! [ "$MINOR" -le 63 ] 2> /dev/null || ! [ "$MICRO" -le 15 ] 2> /dev/null; then
    echo "GTA2_VERSION $VERSION does not fit a CIA's title version" >&2
    exit 1
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
"$BANNERTOOL" makebanner -i "$HERE/banner.png" -a "$HERE/banner.wav" -o "$TMP/banner.bnr"
"$MAKEROM" -f cia -o "$OUT" -target t -rsf "$HERE/gta2.rsf" -elf "$BUILD/gta2.elf" \
    -icon "$BUILD/gta2.smdh" -banner "$TMP/banner.bnr" -ver $((MAJOR * 1024 + MINOR * 16 + MICRO))
echo "$OUT"
