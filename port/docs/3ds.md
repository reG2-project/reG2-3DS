# A Nintendo 3DS port

Status: proof of concept on the `port/3ds` branch. The game builds with devkitARM into a `.3dsx`, boots
in the Azahar emulator straight into a city (the debug keys below), draws it with a citro3d renderer and
takes the 3DS buttons, on Old and New 3DS (Old 3DS figures below). The HUD and a minimap are on the bottom
screen.
Sound plays through OpenDSP, a free replacement for the console's DSP firmware (optional, see Sound).

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
lighting, shadows or bloom). The game's lights and the port's lamps (`port/src/lights.cpp`) light the
vertices, at the blocks' corners, in the gbh_* layer; a headlight's beam is a point light a third of its range
ahead there, as narrow beams would flicker on and off the corners. In bil at dusk, driving through traffic in
Azahar's Old 3DS mode, the lamps cost 1.3 ms per frame (draw 14.3 to 15.6 ms). The game's vertices are in logical pixels with 1/w; the vertex shader
outputs `(x*w, y*w, z*w, w)` so the PICA200 interpolates textures with perspective. Textures arrive
as RGBA8 from the gbh_* layer and are stored in the smallest exact PICA format (RGB565, RGBA4,
RGB5A1, else RGBA8), padded to a power of two. The gbh_* layer tells the format from each palette
entry's bits (`RTexelBits`), and the frontend's TGA images are known to be opaque 5-6-5, so only the
port's few own textures are scanned; each level is then written in one
pass that premultiplies, converts, pads, writes Morton order and averages the next level into a
scratch buffer. The CPU surface is composited through an RGB5A1 texture. One frame of vertices lives
in linear memory; texture frees wait for the GPU.

The top screen is drawn at 400x240: the game's 800x480 scaled by half, with no vertex offset and bilinear
filtering, so the game's 1:1 quads (menus, text) come out as the exact 2x2 average of their texels.
Blended textures are premultiplied at upload, so filtered edges take no colour from transparent texels.
The game's tiles, sprites and fonts carry mip levels made on the CPU at upload (a third more texture
memory, 0.2 to 0.6 MB in the cities), sampled trilinearly, except 1:1 draws and the HUD; the world then
neither shimmers nor drops texels. What is lost against a 2x2 supersampled image: polygon edges are not
antialiased (the PICA200 has no MSAA), the overhead wires (lines in transparent tiles) break up in
places, and walls seen at a steep angle go soft (isotropic mip selection). The top screen fills about
0.15 to 0.28 Mpixels per frame with overdraw, a small part of the PICA200's rated 4 pixels per clock.

The game keeps its 480 lines rather than drawing at 400x240: its menus are laid out for 640x480, its
fonts are bitmaps of a fixed size, and its camera shows the area of a 480-line screen. At 400x240 the
menus and HUD would need new layouts and smaller fonts, and the world would show a quarter of the area
unless the camera zoomed out (sampling textures at half size again).

The HUD is on the bottom screen at its own pixel size. `Hud::DrawGui` marks its element groups
(`port/src/hud_screen.*`, `HUD_GROUP` in `Source/Hud.cpp`); a renderer with a HUD screen
(`IRenderer::SetHudGroup`, `HudScreenWidth`) takes those draws, in 640x480 layout pixels, and places each
group in a panel of a fixed grid on the 320x240 screen (`kHudPanels`); the mission brief wraps to its
width and scrolls when longer. `port/3ds/hud_theme.*` draws a recessed panel behind each part drawn that
frame, over a dark backdrop made once per city from its most common plain pavement tile (one 8 KB
texture, 2 draws per frame). The world stays on the top screen, with the zone and car names, pickup
texts, big messages and arrows. The frontend and the pause menu stay on the top screen, legible but small
(4 to 5 pixels high); the bottom screen is black there.

The bottom screen's last row holds the brief on the left and a minimap on the right (`port/3ds/minimap*`):
the tube frame E of the design study (`port/docs/minimap`), north up, 21x21 blocks around the player at 4
pixels per block. It is meant to be fair, so it shows little, and every colour and icon on it comes from
the city's style:

- the ground in the mean colour of each block's lid tile, lighter as the block stands taller, with the
  study's rims and shadows; where no block has a lid, and around the city, the mean colour of the city's
  water tiles;
- the gangs the respect meters show: each territory's edge (the map's gang zones) dashed in the gang's
  colour, with a shade outside it, so a territory shows before the player walks in; the territory the
  player is in under a light tint of that colour. A gang's colour is the bright half of the arrow that
  points to its phones (user sprite `arrow`, `Hud_Arrow::DrawArrow`);
- the player's arrow, along the ped's (or its car's) heading: a heading a moves by (sin a, cos a), so 0
  faces down the map;
- inside a territory, that gang's phones, under the rules of the game's own phone arrows
  (`Hud_Arrow::CheckVisibility`): only inside the territory, not on a mission or in an arrow blocker zone,
  a mission phone only once the player has the respect it asks for, answered phones left out. A mission
  phone is the pip of the game's phone arrows (user sprites 8 to 10: yellow, green, red), the info phone
  the gang's respect meter icon (user sprite 63 + arrow). A phone off the map is the game's phone arrow
  in the gang's colour on the map's edge, turned towards it, with its pip (none for the info phone), as
  the top screen draws it;
- outside every territory, each gang's respect meter icon at the nearest block of its territory, or on
  the map's edge in its direction.

Nothing else: no pickups, hidden weapons, secret tokens, frenzies, garages, churches or mission targets.
Arrows and icons are drawn at half size, as the bottom screen draws the HUD, pips at full size. The only
colours of the port's own are shades (black with alpha: rims, shadows, the tube's shadow, the edges'
shade) and the frame and player arrow, original art embedded at build time from the study's PNGs by
`embed_png.py`.

The map is built once per city from the map and tiles in memory (about 200 ms in Azahar's Old 3DS mode,
330 KB of heap while it runs) into a 512x512 RGB565 texture filled in place, 2x2 texels per block (512 KB
of linear memory). The territory edges are a 256x256 RGBA4 texture (128 KB, built when the gangs are set
up, about 20 ms), the light tint over the player's territory a 256x256 A8 texture (64 KB) drawn in the
gang's colour and filled again when the player changes territory (2 to 3 ms) from each block's gang,
kept in the textures' order (64 KB of heap). The frame and the player's arrow are a 32 KB atlas, the
game's sprites for the markers a 16 KB sheet made per city. The game's
headers and the renderer's clash, so `minimap_game.cpp` reads the game and `minimap.cpp` draws.
`GTA2_3DS_MINIMAP=wide` gives the map the whole row while no brief shows; `off` leaves it out and gives the
row to the brief.

The bottom screen is drawn only when what it shows changed, at most every other frame (15 per second at
30): a signature of the HUD's batches (vertices, textures and their uploads, states), its panels, the
brief's scroll and the minimap's state (the player's position to the pixel, heading in 64 steps, gang and
markers). citro3d transfers and swaps a screen only when its target was drawn on, so otherwise the screen
keeps its last frame.

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

The result is `build_3ds/gta2.3dsx` (about 2.3 MB). Its Homebrew Launcher icon (48x48) is drawn from plain
shapes by `make_icon.py` at build time (python3 is in the image), so no game art is involved.

### Sound

The console's DSP firmware (`dspfirm.cdc`) is Nintendo's and not distributed. OpenDSP
(https://github.com/Cellenseres/OpenDSP, BSD-2) replaces it: a library with its own DSP image, loaded
by `opendspInit()` in `platform_3ds.cpp` before SDL opens the audio, SDL's ndsp calls unchanged. Build
it with `port/3ds/build_opendsp.sh <OpenDSP source> <prefix>` and pass `-DOPENDSP_DIR=<prefix>`; ship
`LICENSE.OpenDSP.txt` (next to the `.3dsx` in the build) with it. Without it the port asks libctru for
`sdmc:/3ds/dspfirm.cdc` as other homebrew does.

Azahar plays it with HLE audio (the default, which emulates Nintendo's firmware and only hashes the
image) and with LLE (Teakra runs OpenDSP's code, at about a quarter of real time). OpenDSP costs nothing over Nintendo's path.

The port mixes the game's sounds on the CPU (`compat/src/miles.cpp`) at the DSP's 32728 Hz, and SDL's
audio thread runs on the system core (core 1, 30 % of it, `APT_SetAppCpuTimeLimit`) on both models:
SDL 3.4.18 does that on the New 3DS only, `build_sdl3.sh` applies `sdl3_audio_syscore.patch` for the Old.
Mixing takes about 1.9 ms per frame in bil (5 voices), off the game's core; SDL's conversion, ndsp and
the DSP cost nothing measurable. bil walks at 30 frames per second with sound. Two mixer changes made
that possible (bit-exact with the previous output): the limiter scanned its 64-frame lookahead for every
output frame (a quarter of the Old 3DS core), and each voice now has a loop per sample format with a
steady-state path. Azahar's GDB stub only shows the main thread: the audio time was measured with tick
counters and a lowest-priority thread counting idle time on core 0. OpenDSP was tested by its author on a New 3DS with Luma3DS; whether a stock DSP module accepts its
unsigned image, and its sleep handling, need a console.

## Running

The game folder is the `.3dsx`'s own folder when it holds `data/fstyle.sty` (from the path the
Homebrew Launcher passes as `argv[0]`, which libctru sets before the constructors), else `sdmc:/3ds/gta2/`:
`data/` (without `GTAudio` and `Movie`, about 65 MB), `gta2_registry.ini`, `player/` (the saves), `test/`.
Without the data in either, the top screen lists the folders tried and the files expected until START.
The game's own error boxes (a missing city file, for example), which SDL cannot show on the 3DS, appear on
the bottom screen until START (`--wrap=SDL_ShowSimpleMessageBox`) rather than the game closing silently.
The port's log is `gta2_log.txt` there (stderr and SDL's log).
`gta2_env.txt` there sets environment variables, one `KEY=VALUE` per line: `GTA2_3DS_STATS=150` logs
memory and renderer times every 150 frames, `GTA2_LOOP_STATS=1` steps, frames and their times,
`GTA2_3DS_FPS=1` shows frames and logic steps per second, the main thread's CPU time per frame
(without its waits for the GPU and its sleeps) and the heap and linear memory in use in the top screen's corner,
over the last second and refreshed 4 times per second, `GTA2_UNCAPPED=1` draws as fast as it can (the logic
keeps its 30.3 steps per second, citro3d does not wait for the screens' refresh) to measure the headroom
(Azahar, bil: about 185 frames per second walking at noon, 165 driving through traffic, 100 driving at dusk
with the lamps, the same in New 3DS mode, which runs the same clock; Azahar leaves out the GPU's time),
`GTA2_AUDIO_DRIVER=dummy` runs the sound without the DSP, `GTA2_3DS_MINIMAP=wide` or `off` sets the
minimap's place (see Renderer). To boot into a city, the registry's `Debug` keys
`skip_frontend`, `mapname`, `stylename` and `scriptname` (see `Source/debug.cpp`).

Azahar (2126.2) runs the `.3dsx` without firmware or system files, in Old or New 3DS mode. It passes
no arguments, so the game folder there is always `sdmc:/3ds/gta2/`. The start
to the first city frame takes about 15 seconds there.

Tests: `test/pad_input.txt` presses buttons, one `<first frame> <last frame> <hex KEY_* mask>` per line
(frames as `port_FrameCount` counts them), for example `150 170 8` for START; the `KEY_CPAD_*` and
`KEY_CSTICK_*` bits push the circle pad and the C-stick all the way. `GTA2_TEST_CAR=<frame>:<model>[:<dx>:<dy>]`
puts a car of that model (54 the tank) beside the player at that frame, who then gets in.

## Buttons

SDL3's 3DS gamepad mapping and the port's default pad bindings give, by position: B attack, A jump,
X enter or exit, Y special 1, L and R the weapons, START the pause menu, SELECT back, the d-pad and the
circle pad move; ZL and ZR (New 3DS) are specials 2 and 3, and brake and accelerate in vehicles. In the
fire truck, the tank and the gun jeep, Y held with left or right turns the roof gun (the original's
SPECIAL 1 with a turn). In the menus B or START selects and A or SELECT goes back, and the hints name
the buttons as the console prints them (`PortPlatform::PadButtonName`).

The settings menu leaves out what the 3DS lacks (`PortPlatform::Has` in `port/src/platform.hpp`, answered
in `platform_3ds.cpp`): the window, display modes and vsync, frame limits above 30, BRIGHTNESS (the
renderer has no gamma), SAFE AREA, the OpenGL renderer's options, the CHINATOWN camera with its rotation and
tilt, HUD SCALE (the HUD has its own screen), 3D SOUND (no OpenAL Soft) and the bindings: the pad keeps its
default bindings, stored ones are ignored. VIEW keeps CAMERA ZOOM, GRAPHICS CAR HEADLIGHTS and LIGHT
EFFECTS, on by default: the port's lamps are lit per vertex (see Renderer).
CONTROLS is there on the New 3DS only, told apart at run time (`APT_CheckNew3DS`): TWIN STICK moves with
the circle pad and aims with the C-stick (SDL's right stick), which also turns a roof gun, and ZR attacks,
as the PC's right stick and RT; CLASSIC stays the default. The Old 3DS, without a second stick, has
CLASSIC only. Tested in Azahar with scripted presses: the pause menu's pages, Y with left turning the
tank's gun, and on the New 3DS TWIN STICK set in the menu and the C-stick turning it.

## HOME, sleep and exit

SDL's event pump calls `aptMainLoop` once per game frame (the game's `PeekMessage`), between citro3d
frames: HOME and the power button open the HOME Menu there, closing the lid sleeps there, and the
suspend and sleep hooks of citro3d (GPU state) and ndsp (the DSP) do the rest. The port's own hook
(`platform_3ds.cpp`, run there on the main thread) calls `port_Suspend` (`compat/src/kernel.cpp`): the
game's clocks (`timeGetTime`, the frame loop's) leave out the time away and SDL's audio device pauses, so
the game and its sound go on where they stopped, without catch-up steps. Nothing is pumped during a city
load, so HOME waits for its end. Closing from the HOME Menu makes `aptMainLoop` return false, SDL sends a
quit event and the game shuts down as on a window close (`GTA2_EXIT_AT` tests the same path). The 3D
slider does nothing: `gfxInit` leaves the top screen in 2D mode. `osSetSpeedupEnable` turns on the New
3DS's 804 MHz and L2 cache.

Azahar ignores HOME without a HOME Menu title and has no sleep control, so `GTA2_TEST_SUSPEND=<frame>,<seconds>`
calls the hook's suspend and resume around a wait at that frame, from the event pump. In bil with sound
(8 s): the HUD clock and the steps stop, the second after the resume runs 30 steps, the sound is silent
through the wait and comes back without a click, and the game draws 30 frames per second again. The real
hooks, sleep with the lid, and the GPU and DSP state across them need a console.

A `.cia` (a HOME Menu icon without the Homebrew Launcher) would need `makerom` and `bannertool`, which
the devkitPro image lacks, an `.rsf` with a unique ID and a 256x128 banner with a sound; it would get no
`argv[0]` and so use `sdmc:/3ds/gta2/`. Not done: little gain over the `.3dsx`.

## Old 3DS

### Memory

The Old 3DS gives an application 64 MB (memory type 0), New 3DS 124 MB. Every `.3dsx` starts through
Luma3DS's loader (hb:ldr), whether from the Homebrew Launcher Loader title, a Rosalina title takeover or
a chainload from hbmenu: it patches the title's header to the console's current memory mode, so a `.3dsx`
gets 64 MB on an Old 3DS (124 MB on a New 3DS), never an extended mode; only Luma's autoboot setting
`autoboot_3ds_app_mem_type` changes it until the next reboot. The image (3.2 MB) and both heaps come out
of that 64 MB, about 60.8 MB being free at start. A CIA could ask for 72, 80 or 96 MB (dev system
modes): the console then reboots into that mode at launch and exit, and the system's memory shrinks
(HOME Menu and applets limited); not worth it here.

Azahar gives a `.3dsx` more (memory type 2, 96 MB, in Old 3DS mode), so the port fixes its heaps
(`platform_3ds.cpp`) and the emulator holds it to them: 40 MB heap, out of which libctru takes the 2 MB
stack, and 16 MB linear heap, 59.2 MB in all with the image, which leaves 4.8 MB free on an Old 3DS. If
the system cannot give that (the Homebrew Launcher in applet mode, a bigger image), libctru 2.7.0 would
stop at start-up (`svcBreak` in `allocateHeaps.c`, before `main`); the port replaces that function
(`__system_allocateHeaps`, weak) and then takes smaller heaps, enough for an error screen with the sizes.
Leaving the heap at 0 (all that is left) would panic on a New 3DS with libctru 2.7.0 (more than the 96 MB
heap area).

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
city (6 MB) is the only addition still to come with the DSP. The minimap adds its 752 KB of linear memory
and 64 KB of heap.

### Speed

Azahar charges each instruction a fixed number of ARM11 cycles from a table (no caches, memory latency
or interlocks) and system calls too much, so its times are a rough guide only; its New 3DS mode runs the
same 268 MHz clock, and `cpu_clock_percentage` only scales the table. No 3DS emulator models Old 3DS
timing more closely (Panda3DS uses an older table, Mikage and Corgi3DS count one per instruction): only
hardware gives real figures. At 100 % (`GTA2_LOOP_STATS`,
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
frame (now only after a frame drew on it, and not at all in game, where nothing writes it), and SDL's
gamepad reads take a lock per call, a system call each on the 3DS (now each button and axis is read
once per update, and the pad bindings once from the registry). Night lighting tests each primitive's
vertices against the lights in its reach only. Logic runs at the original's 30.3 steps per second
throughout. A second profile (walking and driving, day and night) put the main thread's CPU time at
12 to 14 ms per frame in Azahar: about 3 ms logic, 5 to 7 ms drawing, 3 to 4 ms input polling.
Texture uploads went from up to six passes per texture to one per level, 2.5 to 3 times faster
in Azahar (a 64x64 sprite 2.3 to 0.84 ms, a 128x128 one 9.2 to 3.1 ms; upload time in a frame of a
long drive at most 7 to 10 ms instead of 19 to 29). Remaining leads: one pad update per frame, fewer draw calls (about 100 per frame) with a tile atlas. The system core runs the sound (see Sound).

On the 3DS any per-frame file access is expensive: each path component of an `fopen`, `access` or
`stat` is a request to the FS service (tens of milliseconds in Azahar). The pause menu and the
frontend's OPTIONS page drew at 3 frames per second because the port's menus checked every frame
whether the language's `data/?.gxt` existed; each language is now looked up once, and the pause menu
draws at the 30 fps limit.

The bottom screen's CPU time per frame (`GTA2_3DS_STATS`, walking, Old 3DS mode): 0.38 to 0.42 ms
(0.45 to 0.51 at most) when it was drawn every frame without a minimap; with the minimap and drawn only
on a change, 0.05 ms standing (5 frames of 150 drawn, for the clock), 0.18 to 0.44 ms walking (up to 75
of 150), and 0.65 to 0.87 ms in a frame that draws it. The frame rate stays at 30.3 in bil and ste. With
the territory edges and icons and the game's sprites (ste, the same walk on both builds): 0.029 ms
standing against 0.027, 0.19 against 0.17 walking with the same frames drawn, 0.69 to 0.74 ms in a frame
that draws it against 0.59 to 0.66.

## Next steps

- HUD: the zone and car name banners, pickup texts and big messages on the bottom screen too; a
  second pager; multiplayer scores; bonus stages.
- Menus: the port's own menus (pause, options) with a larger font scale on the 3DS, or on the bottom
  screen at 1:1.
- Sound on hardware: OpenDSP's DSP load and sleep, the Old 3DS's audio thread on the system core.
- Real Old 3DS timings: Azahar's are optimistic for memory bound code (no cache model). If the
  hardware is short, the next candidates are the per-vertex lighting at night and SDL's pad reads.
- Real hardware: a 3DS or New 3DS with custom firmware.
