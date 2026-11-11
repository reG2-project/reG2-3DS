#!/bin/sh
# make_cia.sh <build dir> <makerom> <bannertool> [out.cia]: packs the build's gta2.elf, with its gta2.smdh (the
# .3dsx's title and icon) and this folder's banner, into a CIA for the HOME Menu (gta2.cia in the build dir).
set -e
if [ $# -lt 3 ]; then
    echo "usage: $0 <build dir> <makerom> <bannertool> [out.cia]" >&2
    exit 1
fi
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=$1; MAKEROM=$2; BANNERTOOL=$3; OUT=${4:-$BUILD/gta2.cia}
for f in "$BUILD/gta2.elf" "$BUILD/gta2.smdh"; do
    [ -f "$f" ] || { echo "$f missing: build the .3dsx first" >&2; exit 1; }
done
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
"$BANNERTOOL" makebanner -i "$HERE/banner.png" -a "$HERE/banner.wav" -o "$TMP/banner.bnr"
"$MAKEROM" -f cia -o "$OUT" -target t -rsf "$HERE/gta2.rsf" -elf "$BUILD/gta2.elf" \
    -icon "$BUILD/gta2.smdh" -banner "$TMP/banner.bnr"
echo "$OUT"
