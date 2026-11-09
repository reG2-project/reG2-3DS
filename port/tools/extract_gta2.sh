#!/bin/sh
# extract_gta2.sh GTA2INSTALLER.exe <out dir>: the game's data folder from the 2004 installer, without running it.
# Needs unshield. The installer's InstallShield files follow its program, each after "name\0path\0version\0size\0".
set -e
exe=$1; out=$2; tmp=$out/.cab
mkdir -p "$tmp"
for name in data1.hdr data1.cab data2.cab; do
    path="Disk1\\$name"
    at=$(grep -obaF "$path" "$exe" | head -n 1 | cut -d: -f1)
    size=$(tail -c +$((at + ${#path} + 2)) "$exe" | head -c 64 | tr '\0' '\n' | sed -n 2p)
    version=$(tail -c +$((at + ${#path} + 2)) "$exe" | head -c 64 | tr '\0' '\n' | sed -n 1p)
    tail -c +$((at + ${#path} + ${#version} + ${#size} + 4)) "$exe" | head -c "$size" > "$tmp/$name"
done
unshield -d "$tmp/x" x "$tmp/data1.cab" > /dev/null
mv "$tmp/x/App_Executables/data" "$out/data"
rm -rf "$tmp"
