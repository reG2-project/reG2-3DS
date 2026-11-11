#!/bin/sh
# train_bench.sh <gta2_registry.ini> <out dir> [steps]: the train ride benchmark's files for the game folder
# (port/docs/3ds.md, Running). For each city (wil, ste, bil), by night (0h) and by day (12h): gta2_env_<city>_<time>.txt,
# to copy as gta2_env.txt, and the registry it names, the given one booting into that city at that hour.
set -e
REG=$1; OUT=$2; STEPS=${3:-4500}
[ -f "$REG" ] && [ -n "$OUT" ] || { echo "usage: $0 <gta2_registry.ini> <out dir> [steps]"; exit 1; }
mkdir -p "$OUT"
for city in wil ste bil; do
    for time in night day; do
        hour=0; [ $time = day ] && hour=12
        name=${city}_$time
        awk -v c=$city -v h="$(printf %08x $hour)" '
            /^\[HKLM\\SOFTWARE\\DMA Design Ltd\\GTA2\\Debug\]/ {
                print; print "skip_frontend = dword:00000001"; print "mapname = sz:" c ".gmp"
                print "stylename = sz:" c ".sty"; print "scriptname = sz:" c ".scr"; skip = 1; next }
            /^\[HKLM\\SOFTWARE\\DMA Design Ltd\\GTA2\\Screen\]/ { print; print "day_start = dword:" h; next }
            /^\[/ { skip = 0 }
            skip && NF { next }
            /^day_start = / { next }
            { print }' "$REG" > "$OUT/gta2_registry_bench_$name.ini"
        [ "$(grep -c '^skip_frontend = \|^day_start = ' "$OUT/gta2_registry_bench_$name.ini")" = 2 ] ||
            { echo "$REG lacks the Debug or Screen key"; exit 1; }
        printf 'GTA2_REGISTRY=gta2_registry_bench_%s.ini\nGTA2_3DS_PROF=150\nGTA2_TEST_TRAIN=60:%s\n' $name "$STEPS" \
            > "$OUT/gta2_env_$name.txt"
    done
done
ls "$OUT"
