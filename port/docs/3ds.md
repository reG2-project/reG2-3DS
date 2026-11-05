# A Nintendo 3DS port

Status: proof of concept on the `port/3ds` branch. The game builds with devkitARM into a `.3dsx`, boots
in the Azahar emulator straight into a city (the debug keys below), draws it with a citro3d renderer and
takes the 3DS buttons, on Old and New 3DS (Old 3DS figures below). No sound yet: Azahar needs the
console's DSP firmware (`sdmc:/3ds/dspfirm.cdc`), which is not distributed.

## What helps

- The 3DS is 32-bit (ARM11), so the game's structures keep their original sizes and the data
  files load as they are. It does not depend on the 64-bit work (`64bit.md`).
- SDL3 has an official 3DS backend (buttons, circle pad, audio, threads, files), so the port's
  platform layer (`compat/`) builds as it is.
- GTA2 is light: the PC version targeted a Pentium 200 with 32 MB of RAM.

## Layout: shared and 3DS-only

Everything the Linux build compiles is shared, except the OpenGL renderer and the first-start setup:

| Part | 3DS build |
|---|---|
| `Source/`, `test.cpp` (the game) | shared |
| `port/src/` (menus, settings, frame loop, view, ...) | shared |
| `port/compat/` (Win32 and DirectX on SDL3) | shared |
| `port/renderer/` gbh_* layer, `vid_system`, options, capture | shared |
| `port/renderer/gl_*`, `sdl_gl_context.cpp` (OpenGL ES 3) | replaced by `port/3ds/c3d_*` |
| `port/setup/` (first start, libarchive, unshield) | replaced by `port/3ds/platform_3ds.cpp` |

`port/3ds/` holds the rest: the build image (`Dockerfile`), the toolchain file, the SDL3 build script, the
CMake project (it globs `port/src`, `port/compat/src` and `port/renderer`, so new files on `port/m0` come
along), the citro3d backend and its vertex shader, start-up, and scripted buttons for tests.

## Seams in shared files

Each is small and makes sense on `port/m0` too:

- `renderer/vid_system.hpp`: the renderer is a `BackendRenderer` typedef, `C3DRenderer` on the 3DS
  (one `#if`).
- `renderer/gl_context.hpp`, `sdl_gl_context.cpp`, `compat/src/window.cpp`: `GLWindowFlags()`, the
  window flags the context needs (`SDL_WINDOW_OPENGL`, none for citro3d).
- `compat/src/main.cpp`: newlib calls constructors without `argc`/`argv` (one `#elif`).
- `compat/src/video_port.cpp`: a screen under 480 lines draws from 480 lines of its shape, so the 400x240
  top screen gets an 800x480 game scaled by half rather than a letterboxed 640x480.
- `Source/gtx_0x106C.cpp`, `Source/SpriteRenderer.cpp`, `Source/3rdParty/mss.h`: the x86 inline asm
  stays under `_M_IX86`/`__i386__`, with C versions (or none, for the profiler's rdtsc) elsewhere. The
  VC6 build compiles the same code.

## Toolchain

- clang compiles (`--target=armv6k-none-eabihf`), as on Linux: the game needs its MSVC extensions.
  devkitARM's GCC links, with newlib, libstdc++ and libctru. The image of `port/3ds/Dockerfile` is the
  official `devkitpro/devkitarm` with `clang-22` added.
- GCC for ARM uses short enums, clang int-sized ones: SDL3 is built with `-fno-short-enums`
  (`build_sdl3.sh`), since the port shares SDL's structs. libctru and citro3d keep short enums; the
  renderer only passes their enums by value.
- `-fshort-wchar` as on Linux (`compat/src/wstr.cpp` provides the wide string functions).
- The main thread's stack is 2 MB (`__stacksize__`; libctru's default is 32 KB).

## Renderer

`C3DRenderer` implements `GLRenderer`'s interface with citro3d, the classic look only (no per-pixel
lighting, shadows or bloom). The game's vertices are in logical pixels with 1/w; the vertex shader
outputs `(x*w, y*w, z*w, w)` so the PICA200 interpolates textures with perspective. Textures arrive
as RGBA8 from the gbh_* layer and are stored in the smallest exact PICA format (RGB565, RGBA4,
RGB5A1, else RGBA8), padded to a power of two. The CPU surface is composited through an RGB5A1
texture. One frame of vertices lives in linear memory; texture frees wait for the GPU.

## Build

```bash
docker pull devkitpro/devkitarm
docker build -t gta2-3ds-build port/3ds
# SDL3 from its source release (3.4.18 tested), into a prefix:
docker run --rm -u $(id -u):$(id -g) -v $PWD:$PWD -v $SDL:$SDL gta2-3ds-build \
    port/3ds/build_sdl3.sh $SDL/SDL3-3.4.18 $SDL/sdl3
docker run --rm -u $(id -u):$(id -g) -v $PWD:$PWD -v $SDL:$SDL -w $PWD gta2-3ds-build sh -c \
    "cmake -S port/3ds -B build_3ds -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/port/3ds/toolchain-3ds-clang.cmake \
     -DSDL3_DIR=$SDL/sdl3/lib/cmake/SDL3 -DCMAKE_BUILD_TYPE=RelWithDebInfo && ninja -C build_3ds"
```

The result is `build_3ds/gta2.3dsx` (about 2.3 MB).

## Running

The game folder is `sdmc:/3ds/gta2/`: `data/` (without `GTAudio` and `Movie`, about 65 MB),
`gta2_registry.ini`, `player/`, `test/`. The port's log is `gta2_log.txt` there (stderr and SDL's log).
`gta2_env.txt` there sets environment variables, one `KEY=VALUE` per line: `GTA2_3DS_STATS=150` logs
memory and renderer times every 150 frames, `GTA2_LOOP_STATS=1` steps, frames and their times,
`GTA2_AUDIO_DRIVER=dummy` runs the sound without the DSP. To boot into a city, the registry's `Debug` keys
`skip_frontend`, `mapname`, `stylename` and `scriptname` (see `Source/debug.cpp`).

Azahar (2126.2) runs the `.3dsx` without firmware or system files, in Old or New 3DS mode. The start
to the first city frame takes about 15 seconds there.

Tests: `test/pad_input.txt` presses buttons, one `<first frame> <last frame> <hex KEY_* mask>` per line
(frames as `port_FrameCount` counts them), for example `150 170 8` for START.

## Buttons

SDL3's 3DS gamepad mapping and the port's default pad bindings give, by position: B attack, A jump,
X enter or exit, Y special 1, L and R the weapons, START the pause menu, SELECT back, the d-pad and the
circle pad move; ZL and ZR (New 3DS) are specials 2 and 3, and brake and accelerate in vehicles. Tested
in Azahar with scripted presses: the d-pad walks and START pauses. The menus show the Xbox glyphs.

## Old 3DS

### Memory

The Old 3DS gives an application 64 MB (memory type 0), New 3DS 124 MB. A `.3dsx` gets the memory of
the process the Homebrew Launcher runs in: started by title takeover, an application's, so 64 MB on an Old
3DS; other entry points can give less (not checked on hardware). The port is designed for 64 MB. Azahar
gives a `.3dsx` more (memory type 2, 96 MB, in Old 3DS mode), so the port fixes its heaps itself
(`platform_3ds.cpp`) and the emulator holds it to them: 40 MB heap and 16 MB linear heap, plus the 3.2 MB
image (code, data, bss) and the 2 MB stack, 61 MB in all. Start-up fails if the system cannot give that.

Measured in Azahar's Old 3DS mode with `GTA2_3DS_STATS` (peaks over a minute and a half of walking):

| | Heap | Linear heap (textures, vertices, commands) |
|---|---|---|
| Frontend | 21.8 MB | 5.7 MB |
| Frontend, then wil | 21.8 MB (15.9 MB in game) | 7.8 MB |
| wil | 15.9 MB | 5.9 MB |
| ste | 16.0 MB | 6.7 MB |
| bil | 16.1 MB | 6.8 MB |
| bil with sound (SDL's dummy driver) | 22.2 MB | 6.8 MB |

New 3DS mode gives the same figures: the game's needs do not depend on the model. The sound bank of a
city (6 MB) is the only addition still to come with the DSP.

### Speed

Azahar counts instructions rather than ARM11 cycles and charges system calls too much, so its times are
a rough guide only; New and Old 3DS modes run the same emulated clock. At 100 % (`GTA2_LOOP_STATS`,
FRAME LIMIT 30 ORIGINAL, walking):

| | Steps/s | Frames/s | Logic step | Draw (with the wait for the GPU) |
|---|---|---|---|---|
| wil | 30.3 | 30.3 | 1.1 ms | 14.0 ms (7.3) |
| ste | 30.3 | 30.2 | 1.9 ms | 13.3 ms (7.4) |
| bil | 30.3 | 30.2 | 2.4 ms | 14.5 ms (8.0) |
| bil with sound | 30.4 | 29.0 | 3.4 ms | 20.8 ms (12.9) |
| bil at 50 % clock | 30.3 | 21 to 25 | 4.9 ms | 15 to 20 ms |

Before the fixes below bil drew 15 frames per second. A sampling profile through Azahar's GDB stub
found the time in the port, not the game: the car lamps scanned delta images for every car in the
pool every frame (now cached, cars off screen skipped), the CPU surface was refilled and scanned every
frame (now only after a frame drew on it), and SDL's gamepad reads take a lock per call, a system call
each on the 3DS (left as is: about 1 to 2 % on hardware by the profile's estimate). Logic runs at the
original's 30.3 steps per second throughout. The second core is not used.

## Next steps

- Sound: test on hardware or with a dumped `dspfirm.cdc` (SDL3's 3DS audio uses NDSP). A day.
- Real Old 3DS timings: Azahar's are optimistic for memory bound code (no cache model). If the
  hardware is short, the next candidates are the per-vertex lighting at night and SDL's pad reads.
- The menus' button glyphs and prompts for the 3DS layout; the bottom screen for the map or the HUD.
- The game folder from the `.3dsx`'s own folder rather than a fixed `sdmc:/3ds/gta2`.
- Real hardware: a 3DS or New 3DS with custom firmware.
