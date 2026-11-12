#!/bin/sh
# ci.sh <build|cia> <work dir>: the 3DS release build, from the repository's root, as .github/workflows/3ds.yml runs it.
# build, in the port/3ds/Dockerfile image: SDL3 and OpenDSP into <work dir>/deps (cacheable, with <work dir>/dl),
# then the game, and reg2.3dsx in <work dir>/out. cia, on x86_64 Linux with glibc 2.39 or later (the prebuilt
# makerom and bannertool): reg2.cia from that build. $GTA2_VERSION, if set, is the version (v0.2.0 gives 0.2.0).
set -eu
STAGE=$1; W=$2
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/versions.sh"

fetch() {
    mkdir -p "$W/dl"
    echo "$SOURCES" | while read -r name url sum; do
        [ "$name" = "$1" ] || continue
        if ! echo "$sum  $W/dl/$name" | sha256sum -c --status 2> /dev/null; then
            curl -sSLf --retry 3 -o "$W/dl/$name" "$url"
            echo "$sum  $W/dl/$name" | sha256sum -c
        fi
    done
}

case $STAGE in
build)
    D=$W/deps
    if [ ! -f "$D/.complete" ]; then
        rm -rf "$D" "$W/src"
        mkdir -p "$W/src"
        fetch SDL3-3.4.18.tar.gz
        tar -xzf "$W/dl/SDL3-3.4.18.tar.gz" -C "$W/src"
        sh "$HERE/build_sdl3.sh" "$W/src/SDL3-3.4.18" "$D/sdl3"
        git init -q "$W/src/OpenDSP"
        git -C "$W/src/OpenDSP" fetch -q --depth 1 "$OPENDSP_REPO" "$OPENDSP_COMMIT"
        git -C "$W/src/OpenDSP" checkout -q FETCH_HEAD
        sh "$HERE/build_opendsp.sh" "$W/src/OpenDSP" "$D/opendsp"
        rm -rf "$W/src" "$D/sdl3/build" "$D/opendsp/build"
        touch "$D/.complete"
    fi
    cmake -S "$HERE" -B "$W/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$HERE/toolchain-3ds-clang.cmake" \
        -DSDL3_DIR="$D/sdl3/lib/cmake/SDL3" -DOPENDSP_DIR="$D/opendsp" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        ${GTA2_VERSION:+"-DGTA2_VERSION=${GTA2_VERSION#v}"}
    ninja -C "$W/build"
    mkdir -p "$W/out"
    cp "$W/build/gta2.3dsx" "$W/out/reg2.3dsx"
    cp "$W/build/LICENSE.OpenDSP.txt" "$W/out/"
    ;;
cia)
    fetch makerom-v0.19.0-ubuntu_x86_64.zip
    fetch bannertool-1.2.3-linux.tar.gz
    T=$W/tools
    rm -rf "$T"
    mkdir -p "$T"
    unzip -q -j "$W/dl/makerom-v0.19.0-ubuntu_x86_64.zip" -d "$T"
    tar -xzf "$W/dl/bannertool-1.2.3-linux.tar.gz" -C "$T" --strip-components=1
    chmod +x "$T/makerom" "$T/bannertool"
    mkdir -p "$W/out"
    sh "$HERE/cia/make_cia.sh" "$W/build" "$T/makerom" "$T/bannertool" "$W/out/reg2.cia"
    ;;
*)
    echo "usage: $0 <build|cia> <work dir>" >&2
    exit 1
    ;;
esac
