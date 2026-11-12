# The 3DS build environment: devkitARM, libctru and citro3d, plus clang for the game's MSVC extensions.
FROM devkitpro/devkitarm:20260610@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5
RUN apt-get update && apt-get install -y --no-install-recommends clang-22 llvm-22-linker-tools && rm -rf /var/lib/apt/lists/*
# llvm-profdata-22, for the profile of port/3ds/pgo.
RUN apt-get update && apt-get install -y --no-install-recommends llvm-22 && rm -rf /var/lib/apt/lists/*
