# The 3DS build environment: devkitARM, libctru and citro3d, plus clang for the game's MSVC extensions.
FROM devkitpro/devkitarm
RUN apt-get update && apt-get install -y --no-install-recommends clang-22 llvm-22-linker-tools && rm -rf /var/lib/apt/lists/*
