# The 3DS build environment: devkitARM, libctru and citro3d, plus clang for the game's MSVC extensions.
FROM devkitpro/devkitarm
RUN apt-get update && apt-get install -y --no-install-recommends clang-22 llvm-22-linker-tools && rm -rf /var/lib/apt/lists/*
# llvm-profdata-22, for the profile of port/3ds/pgo.
RUN apt-get update && apt-get install -y --no-install-recommends llvm-22 && rm -rf /var/lib/apt/lists/*
