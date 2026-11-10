#!/bin/sh
# train.sh <game folder> <work folder>: rewrites gta2.proftext, the 3DS build's PGO profile (port/docs/3ds.md, Build).
# Builds the Linux port (port/toolchain-linux32-clang.cmake) with clang's instrumentation, plays the scenarios below
# headless and silent at the same time, and merges their counts. <game folder> holds the PC game's data/.
# SETUP_DEPS: GTA2_SETUP_DEPS_DIR for the Linux build; LLVM_PROFDATA: the host's llvm-profdata.
set -e
P=$(cd "$(dirname "$0")" && pwd)
PORT=$(cd "$P/../.." && pwd)
GAME=$(cd "$1" && pwd)
W=$(mkdir -p "$2" && cd "$2" && pwd)
B=$W/build
NICE="nice -n 19 ionice -c3"
[ -d "$GAME/data" ] || { echo "no data/ in $GAME"; exit 1; }

flags="-O2 -g -DNDEBUG -fprofile-instr-generate"
$NICE cmake -S "$PORT" -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PORT/toolchain-linux32-clang.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGTA2_SETUP_DEPS_DIR="$SETUP_DEPS" \
    "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=$flags" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=$flags" \
    "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -fprofile-instr-generate" > "$W/cmake.log"
$NICE ninja -C "$B" > "$W/build.log"

# play <name> <map> <day start hour> <last step> <GTA2_INPUT> [env...]: one run in a game folder of its own; map
# "frontend" starts in the menus. Inputs count logic steps in the game, drawn frames in the frontend (test_input.cpp).
play() {
    D=$W/runs/$1 map=$2 day=$3 last=$4 input=$5
    shift 5
    rm -rf "$D"; mkdir -p "$D/game/test"
    ln -s "$GAME/data" "$D/game/data"
    sed "s/ste\./$map./g; s/^day_start = .*/day_start = dword:$(printf %08x "$day")/" "$P/registry.ini" > "$D/game/gta2_registry.ini"
    [ "$map" = frontend ] && sed -i '/GTA2\\Debug\]/,/^$/d' "$D/game/gta2_registry.ini"
    env GTA2_DIR="$D/game" SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy GTA2_AUDIO_DRIVER=dummy GTA2_NO_OPENAL=1 \
        GTA2_NO_MESSAGEBOX=1 LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=2 GTA2_LOG_FILE="$D/log.txt" \
        GTA2_EXIT_AT="$last" GTA2_INPUT="$input" LLVM_PROFILE_FILE="$D/%p.profraw" "$@" $NICE "$B/gta2" > "$D/stdout.txt" 2>&1
}
turns="400:Left:40,600:Right:40,800:Left:60,1000:Right:40,1200:Left:60,1400:Right:40,1600:Left:60,1800:Right:40"
turns="$turns,2000:Left:60,2200:Right:40,2400:Left:60,2600:Right:40,1500:Space:20,2500:Space:20"
fire=$(seq 400 90 2300 | sed 's/$/:Left Ctrl:20/' | paste -sd,)
walk="300:Up:1200,420:Left:15,560:Right:20,700:Left:15,840:Right:20,980:Left:15,1120:Right:20,1260:Left:15"
walk="$walk,500:Left Ctrl:10,650:Left Ctrl:10,900:Space:5,1100:Left Ctrl:10,1300:Space:5"
menus="300:Down,330:Down,360:Up,390:Up,420:Return,480:Down,510:Up,540:Return"
pause="900:Escape,950:Down,980:Down,1010:Up,1060:Escape,1100:Up:300,1200:Left:20"
rm -rf "$W/runs"
play wil_drive_night wil 0 3000 "250:Up:2750,$turns" GTA2_TEST_CAR=100:47:0:-1.5 &
play wil_drive_day wil 12 1800 "250:Up:1550,$turns" GTA2_TEST_CAR=100:47:0:-1.5 &
play wil_tank_night wil 0 2400 "250:Up:2150,$turns,$fire" GTA2_TEST_CAR=100:54:-1.5:0 &
play ste_tank_night ste 0 2400 "250:Up:2150,$turns,$fire" GTA2_TEST_CAR=100:54:-1.5:0 &
play ste_walk_night ste 0 1500 "$walk" &
play frontend_pause frontend 0 1500 "$menus,$pause" &
wait
for d in "$W"/runs/*; do
    ls "$d"/*.profraw > /dev/null 2>&1 || { echo "no profile from $d"; exit 1; }
done
${LLVM_PROFDATA:-llvm-profdata} merge --text -o "$P/gta2.proftext" "$W"/runs/*/*.profraw
ls -l "$P/gta2.proftext"
