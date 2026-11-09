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
  official `devkitpro/devkitarm` with `clang-22` and its linker plugin (`llvm-22-linker-tools`) added.
- ARM code (not Thumb) for the ARM11 MPCore with VFPv2 (`-mcpu=mpcore -mfpu=vfpv2`), -O2
  (RelWithDebInfo), strict alignment (with `-munaligned-access` clang merged stores into an STRD at an odd
  address, which the ARM11 faults on; Azahar does not check), and
  full link time optimisation: the objects are LLVM bitcode, which devkitARM's ld compiles through
  clang's plugin (`LLVMgold.so`). See Old 3DS, Speed.
- GCC for ARM uses short enums, clang int-sized ones: SDL3 is built with `-fno-short-enums`
  (`build_sdl3.sh`), since the port shares SDL's structs. libctru and citro3d keep short enums; the
  renderer only passes their enums by value.
- `-fshort-wchar` as on Linux (`compat/src/wstr.cpp` provides the wide string functions).
- The main thread's stack is 2 MB (`__stacksize__`; libctru's default is 32 KB).

## Renderer

`C3DRenderer` implements `GLRenderer`'s interface with citro3d, the classic look only (no per-pixel
lighting, shadows or bloom). The game's lights and the port's LIGHT EFFECTS (`port/src/lights.cpp`: fires,
explosions, muzzle flashes and the like, point lights as the original's) light the vertices, at the blocks'
corners. The port's beams (headlights, brake lights) are left out there: at the corners
they would flicker on and off, and headlights lit as point lights looked poor. The vertex shader
(`render.v.pica`) does the lighting: the frame's first 44 lights are its uniforms (two of the PICA200's 96
vectors each), set once per change, and the gbh_* layer hands it each lit vertex's world position, unlit colour
and shade (`IRenderer::MaxShaderLights`). It sums them as `LightVertex` does (falloff, ceiling fade, ambient and
its rounding, floor and clamp); the CPU only tests a lit primitive against the lights past the 44th, and lights
one in their reach itself, as it does every primitive while a beam is on. The shader's floats keep 16
mantissa bits (positions are taken relative to the first light) and its distance comes from `rsq` rather than
the original's 12-bit square root table: in Azahar, which computes in 32-bit floats, a frozen night frame
differs from the CPU's lighting in 10 to 62 of its 96000 pixels, by one level. Each lit vertex runs
28 + 17 per light instructions (unlit ones 2 more than before): with the console's night drive (180 lit
primitives of 6 vertices, 44 lights) about 0.8 million per frame, some 0.8 ms of the four vertex units at one
instruction per clock, against the 4.5 ms of GPU time it measured per frame (Azahar does not time the GPU). The lights the CPU tests a
primitive against come from a 16x16 grid of the map cells they reach, built once per change. The game's
vertices are in logical pixels with 1/w; the vertex shader
outputs `(x*w, y*w, z*w, w)` so the PICA200 interpolates textures with perspective. Textures arrive
as RGBA8 from the gbh_* layer and are stored in the smallest exact PICA format (RGB565, RGBA4,
RGB5A1, else RGBA8), padded to a power of two. The gbh_* layer tells the format from each palette
entry's bits (`RTexelBits`), and the frontend's TGA images are known to be opaque 5-6-5, so only the
port's few own textures are scanned; each level is then written in one
pass that premultiplies, converts, pads, writes Morton order and averages the next level into a
scratch buffer. The CPU surface is composited through an RGB5A1 texture. The screens' framebuffers are RGB (`GSP_BGR8_OES`, SDL sets RGBA8): an LCD
ignores alpha, but Azahar hands it on to its window, where pixels drawn with alpha 0 (the menus' black panels,
the frontend's black) let what is behind show through. One frame of vertices lives
in linear memory; texture frees wait for the GPU.

Consecutive primitives with the same texture, state and scissor make one draw call. The evictable 64x64
textures with mip levels (the map's tiles, sprites in 64 texel slots) share 512x512 atlas pages, one set per
texel format, so a run of different tiles is one draw call too: driving, 40 % fewer draw calls. A slot is 80x80,
the texels and a border that repeats each mip level's edge texels, one texel wide at 8x8, so filtering reads
what it would read from a texture of their own clamped to its edges (in Azahar, at most 1 off in a channel on
0.02 % of the pixels, from the coordinates' precision). Pages of 1024 cut a little more (53 %) but left up to
2.6 MB unused per format and ran linear memory out in ste. A draw that differs from the previous one only by
its texture writes the texture unit's registers and the draw into the command buffer itself, as citro3d
writes them: `C3D_DrawArrays` also scans the 96 vertex and 96 geometry uniforms and the dirty state on every
call. Driving in Azahar, that and the pages cut the batches' submission by 70 %.

The game's textures are a cache in linear memory: an 8 MB budget, least recently drawn evicted first and
converted again on their next draw. Without it, the textures a long session has drawn outgrew the 16 MB
and showed white. Design and measurements: `3ds_texture_memory.md`.

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
  the map's edge in its direction;
- the church where the player saves, at the middle of the area of each `DO_SAVE_GAME` command of the
  city's script (found once per script among its commands, one per city), or on the map's edge in its
  direction, also during missions. Its icon is the pause menu font's `+` (`gStylePauseFont`) cut into a
  Latin cross: arms one texel shorter than the stem is wide, the foot twice as long, with a shade around.

Nothing else: no pickups, hidden weapons, secret tokens, frenzies, garages or mission targets.
Arrows and icons are drawn at half size, as the bottom screen draws the HUD, pips and the cross at full
size. The only colours of the port's own are shades (black with alpha: rims, shadows, the tube's shadow,
the edges' shade, the cross's) and the frame and player arrow, original art embedded at build time from
the study's PNGs by `embed_png.py`. The player arrow is drawn turned and filtered, so the atlas keeps a
clear column between it and the frame.

The map is built once per city from the map and tiles in memory (about 200 ms in Azahar's Old 3DS mode,
330 KB of heap while it runs) into a 512x512 RGB565 texture filled in place, 2x2 texels per block (512 KB
of linear memory). The territory edges are a 256x256 RGBA4 texture (128 KB, built when the gangs are set
up, about 20 ms), the light tint over the player's territory a 256x256 A8 texture (64 KB) drawn in the
gang's colour and filled again when the player changes territory (2 to 3 ms) from each block's gang,
kept in the textures' order (64 KB of heap). The frame and the player's arrow are a 32 KB atlas, the
game's sprites for the markers a 32 KB RGBA8 sheet made per city (the cross's shade has partial alpha).
The game's headers and the renderer's clash, so `minimap_game.cpp` reads the game and `minimap.cpp` draws.
`GTA2_3DS_MINIMAP=wide` gives the map the whole row while no brief shows; `off` leaves it out and gives the
row to the brief.

The bottom screen is drawn only when what it shows changed, in every other frame at most (15 per second at
30): a signature of the HUD's batches (vertices, textures and their uploads, states), its panels, the
brief's scroll and the minimap's state (the player's position to the pixel, heading in 64 steps, gang and
markers). citro3d transfers and swaps a screen only when its target was drawn on, so otherwise the screen
keeps its last frame. The game leaves the bottom screen's HUD draws out of the frames in between
(`PortHudScreen::Drawn`, `IRenderer::HudScreenDue`). The brief's text, drawn glyph by glyph (136 draws for a long
one), is drawn by the game once: while its text stays the same, the renderer copies the vertices and batches it kept
(`PortHudScreen::Keep`, `IRenderer::KeepHudGroup`), unless one of their textures changed.

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
by `opendspInit()` in `platform_3ds.cpp` before the sound starts, the ndsp calls unchanged. Build
it with `port/3ds/build_opendsp.sh <OpenDSP source> <prefix>` and pass `-DOPENDSP_DIR=<prefix>`; ship
`LICENSE.OpenDSP.txt` (next to the `.3dsx` in the build) with it. Without it the port asks libctru for
`sdmc:/3ds/dspfirm.cdc` as other homebrew does.

Azahar plays it with HLE audio (the default, which emulates Nintendo's firmware and only hashes the
image) and with LLE (Teakra runs OpenDSP's code, at about a quarter of real time). OpenDSP costs nothing over Nintendo's path.

The DSP mixes the game's sounds (`3ds/dsp_voices.cpp`, behind `compat/src/miles.cpp`): each sample the game
starts plays on one of ndsp's 24 channels, from the sound bank itself, with linear interpolation as the PC
mixer. The game's pitch and engine changes set the channel's rate, its volume and pan the channel's mix, and
a loop is two wave buffers, the sound up to the loop's end and then the loop again, looping (the game uses
loop counts 0 and 1 only). The DSP ramps a gain change over its next frame (160 samples, 4.9 ms, a voice's
first frame aside), so a stop sets the gain to 0 and frees the channel 3 frames later. Unsigned 8-bit sounds
(`mhSample`'s, the police radio's words) are made signed in a copy, as the DSP plays signed ones only. A channel is taken
as a sound starts, so the game's 16 channels, its voice sample, the pause menu's and two streams share the
24: a drive with crashes uses up to 20 at once, fading ones included; when none is free the one faded out
longest is taken, else the sound does not play. The CPU mixer has a limiter, the DSP none: the game's mix
passes full scale rarely and by little (peaks of 1.2 in the frontend, 1.24 in a drive with crashes, 1.31
with the tank's shells, under 0.1 % of the frames over 0.99), and the DSP's soft clipping (`NDSP_CLIP_SOFT`,
which OpenDSP applies as x - x^3 / 6.75, flat from 1.5) takes them; the master volume stays at the game's.
The CPU mixer (at the DSP's 32728 Hz, SDL's audio thread) plays instead with `GTA2_3DS_CPU_MIX=1` or another
SDL driver (`GTA2_AUDIO_DRIVER=dummy`).

The streams keep their CPU decode. A thread on the system core (core 1, of which the application gets 30 %,
`APT_SetAppCpuTimeLimit`, as SDL's audio thread had) decodes them ahead every 10 ms and hands each its
channel's frames at the file's own rate, in a ring of 4 wave buffers of 1024 frames (0.19 s at 22050 Hz); it
also notes the sounds that ended and frees the channels faded out. SDL opens no audio device then.
`GTA2_3DS_STATS` logs that thread's share of the time and the channels in use every 5 s (`dsp:`).

The frontend plays `GTAudio/D.wav` (PCM, 22050 Hz stereo; `A.wav` on the credits page) and the radio the
stations `1.wav` to `12.wav` and their `a` variants (IMA ADPCM), all as streams (`AIL_open_stream`). A stream
takes 64 KB: 32 KB of the file read at once, about every 1.5 s for a station and 0.37 s for the menu music, and
0.37 s of decoded frames, topped up without the lock the game's sound calls take. Opening a
station on the main thread takes 30 to 60 ms the first time a file is opened, 13 to 21 ms after: over
2000 frames the radio adds one frame over 60 ms per station change (82 ms), none without the radio.

The system core's time in Azahar (Old 3DS mode, per 5 s, the same scripted runs): with the CPU mixer, the
audio thread took 16 to 17 % in the frontend with its music, 31 to 50 % driving a police car through traffic
with the radio, 28 to 35 % with the tank's shells; on hardware the Old 3DS ran out of its 30 % there and
crackled. With the DSP the thread takes 6 to 7.5 % with a stream (the frontend's music or a station: its 32
KB reads, about 14 ms each in Azahar, and decoding) and 0.1 % without. Captures of both paths through
Azahar's HLE audio (SDL's disk driver) match in pitch (spectra at a frequency scale of 1.000), loudness (0.2
to 0.4 dB louder, the limiter gone) and balance (within 0.1 dB). Azahar's HLE audio clips hard where OpenDSP
clips softly (62 to 1365 samples over full scale in 100 s; one in a run with LLE audio, which runs OpenDSP's
code) and does not ramp gains: a crude count of abrupt steps finds 2 to 3 times more there than with the CPU
mixer's fades (10 against 1 in the frontend, 35 against 14 driving), stops cut at a DSP frame. OpenDSP ramps
them; Nintendo's firmware, used when OpenDSP does not start, needs a console to tell.

OpenDSP was tested by its author on a New 3DS with Luma3DS; whether a stock DSP module accepts its
unsigned image, and its sleep handling, need a console.

## Running

The game folder is the `.3dsx`'s own folder when it holds `data/fstyle.sty` (from the path the
Homebrew Launcher passes as `argv[0]`, which libctru sets before the constructors), else `sdmc:/3ds/gta2/`:
`data/` (without `Movie`: about 65 MB without `GTAudio`, 355 MB with it), `gta2_registry.ini`, `player/` (the saves), `test/`.
The frontend's music and the car radio need `data/GTAudio` (the CD's 22 WAVs): without it the game runs silent there.
From the 2004 Rockstar Classics installer, `port/tools/extract_gta2.sh GTA2INSTALLER.exe <dir>` (sh and
`unshield`) writes that `data/` without running the installer; on Windows, install the game and copy its `data/`.
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
(Azahar, bil: about 185 frames per second walking at noon, 165 driving through traffic, about 100 driving at dusk,
the same in New 3DS mode, which runs the same clock; Azahar leaves out the GPU's time),
`GTA2_AUDIO_DRIVER=dummy` runs the sound without the DSP, `GTA2_3DS_MINIMAP=wide` or `off` sets the
minimap's place (see Renderer). To boot into a city, the registry's `Debug` keys
`skip_frontend`, `mapname`, `stylename` and `scriptname` (see `Source/debug.cpp`).

`GTA2_3DS_PROF=150` (`port/3ds/prof_3ds.cpp`) logs every 150 frames of a game where the main thread's time goes.
Per logic step: the step and its parts (`Game::UpdateGame`'s services in order, the sound, one level deeper in
the cars and the peds, the sprite collisions). Per frame: the input reading, the event pump and the pad, the
sleeps, the draw (sprite sort, map, HUD, the gbh_* primitives with their lighting and their citro3d part, the
batches' submission), the bottom screen, `C3D_FrameEnd`, texture conversion and uploads, the wait for the screens'
refresh and the wait for the GPU apart, and citro3d's own GPU and CPU times. Counts: primitives, draw calls,
vertices, texture binds, lights and the lights tried per vertex lit on the CPU, tiles, sprites, cars, peds, and whether the
player drives. Each part gives its average, its most in one step or frame, its time without the parts under it,
and its calls. The timers' own cost, measured at the start, is taken out of the times and logged; one primitive
in `GTA2_3DS_PROF_SAMPLE` (default 4) is timed, with what it runs, and counted for that many. Without the
variable, a timer costs a test. `python3 port/3ds/prof_summary.py gta2_log.txt` sums a log up in tables,
walking and driving, day and night apart. The lines of `GTA2_3DS_PROF` and `GTA2_3DS_STATS` are kept in memory
and written 8 KB at a time, and at exit (`port/3ds/log_3ds.cpp`): on the console each write to the SD card takes
milliseconds, and the unbuffered stderr made dozens per block, a frame of almost a second every 150.

Azahar (2126.2) runs the `.3dsx` without firmware or system files, in Old or New 3DS mode. It passes
no arguments, so the game folder there is always `sdmc:/3ds/gta2/`. The start
to the first city frame takes about 15 seconds there.

Tests: `test/pad_input.txt` presses buttons, one `<first frame> <last frame> <hex KEY_* mask>` per line
(frames as `port_FrameCount` counts them), for example `150 170 8` for START; the `KEY_CPAD_*` and
`KEY_CSTICK_*` bits push the circle pad and the C-stick all the way, and two more numbers push the circle pad
part of the way, as `hidCircleRead` gives it (up positive, about 154 at the end of the travel), for example
`600 700 0 -109 109` for up and left. `GTA2_TEST_CAR=<frame>:<model>[:<dx>:<dy>]`
puts a car of that model (54 the tank) beside the player at that frame, who then gets in.

## Buttons

The 3DS has a layout of its own (`PortPlatform::kConsoleControls`, `ConsoleDown` in `compat/src/pad.cpp`),
the same buttons on foot and in a vehicle; SDL's 3DS mapping gives the buttons by position:

| Button | On foot | In a vehicle |
|---|---|---|
| Circle pad | MODERN: walk that way (TWIN STICK's move), pushed a little walks slowly; CLASSIC: the original's arrows | steer; CLASSIC: up and down also accelerate and brake |
| R | attack | accelerate |
| L | MODERN: held, the player keeps his facing (backs off while firing with R) | brake, reverse |
| B | jump | handbrake |
| A | get in | get out |
| Y, X | specials 1 and 2 | specials 1 (horn, siren) and 2 |
| D-pad left, right | previous, next weapon | the roof gun's turn (fire truck, tank, gun jeep), else the weapons |
| D-pad up, down | special 3, nothing | special 3, attack |
| START | pause menu | |
| SELECT | back in menus | the next radio station (the PC's F1) |
| ZR, ZL (New 3DS) | attack, nothing | attack, nothing |
| C-stick (New 3DS) | MODERN: aims (TWIN STICK's aim) | MODERN: turns the roof gun |

CLASSIC D-PAD moves on the d-pad instead, and the circle pad takes what the d-pad did:

| Button | On foot | In a vehicle |
|---|---|---|
| D-pad | up, down forward, back; left, right turn; diagonals both | left, right steer; up special 3, down attack |
| Circle pad left, right | previous, next weapon | the roof gun's turn, else the weapons |
| Circle pad up | special 3 | |
| L | next weapon | brake, reverse |

The other buttons are as above (R attack or accelerate, B, A, Y, X, START, SELECT, ZR).

CONTROLS (pause menu or OPTIONS) chooses MODERN, the default, CLASSIC, the original's tank movement, or
CLASSIC D-PAD, the same on the d-pad; the choice is `port_console_controls` in the registry (1, 0 and 2),
apart from the PC's `port_controls`. MODERN is TWIN STICK
(`port/src/twin_stick.cpp`): without an aim the player turns to the way he walks; while L is held his
facing stays the one he had at the press (`port_HoldFacing`), and the C-stick's aim comes first. Shots go
where he faces, without the original's lock on a ped near the line of fire (CLASSIC keeps it). CLASSIC reads
the circle pad as the original's arrows by the way it points past the dead zone, an 8-way gate: up or down
alone within 30 degrees of straight, left or right alone within 22.5, both in between, so a diagonal runs and
turns (a threshold on each axis misses the diagonals of a round gate). On foot, in either style, the circle
pad pushed less than 0.55 of its range walks at the peds' walk speed, with their walk (`PortTwinStick::WalkSpeed`
in `Char::UpdateWalking`), so the player can turn on the spot; further, he runs. The dead zone is 0.25, and the
circle pad gives about 0.96 at the end of its travel, diagonals too (Azahar: 154 of the 160 that SDL's 3DS
driver takes as full, 109 on each axis on a diagonal), so a run is reached a little past half way (not in CLASSIC D-PAD, where the d-pad runs). In MODERN and
CLASSIC only the circle pad moves, so that the d-pad keeps the weapons; L on foot in CLASSIC D-PAD gives the
next weapon without leaving the d-pad or R. The original's SPECIAL 1 with left or right still turns a roof gun. In the menus B
or START selects and A or SELECT goes back; the hints name the buttons as the console prints them
(`PortPlatform::PadButtonName`).

The settings menu leaves out what the 3DS lacks (`PortPlatform::Has` in `port/src/platform.hpp`, answered
in `platform_3ds.cpp`): the window, display modes and vsync, frame limits above 30, BRIGHTNESS (the
renderer has no gamma), SAFE AREA, the OpenGL renderer's options, the CHINATOWN camera with its rotation and
tilt, HUD SCALE (the HUD has its own screen), 3D SOUND (no OpenAL Soft) and the bindings: the pad keeps its
default bindings, stored ones are ignored. VIEW keeps CAMERA ZOOM, GRAPHICS LIGHT EFFECTS, on by default
and lit per vertex (see Renderer); CAR HEADLIGHTS is off.
AIM CAMERA is there on the New 3DS only, told apart at run time (`APT_CheckNew3DS`), with its C-stick.

## HOME, sleep and exit

SDL's event pump calls `aptMainLoop` once per logic step in game (just before the step reads its inputs,
`port_PumpForStep`) and at each of the game's `PeekMessage` elsewhere, between citro3d frames: HOME and the power button open the HOME Menu there, closing the lid sleeps there, and the
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
(`platform_3ds.cpp`) and the emulator holds it to them: 34 MB heap, out of which libctru takes the 2 MB
stack, and 22 MB linear heap, which holds the sound the DSP plays, 59.2 MB in all with the image, which
leaves 4.8 MB free on an Old 3DS. If
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
| ste driving with the radio, the DSP's sound | 19.8 MB | 14.2 MB |

New 3DS mode gives the same figures: the game's needs do not depend on the model. The rows above the last
are from before the DSP's sound and the minimap: the game's sound bank buffer (6.1 MB, the cities' banks
and the frontend's load into it) moved from the heap to the linear heap, by `AIL_mem_alloc_lock`, and the
heap split by as much, as the DSP reads linear memory only. The pause menu's sounds (from `FSTYLE.RAW`) go
there too, and each stream's 4 buffers take 16 KB. The minimap adds its 768 KB of linear memory and 64 KB
of heap.

Going back to the frontend after a game is the tight spot: the frontend allocates while the heap is in
the pieces the city left (19 MB free in a 26.5 MB arena, the largest piece 4 to 6.5 MB, 7.5 MB still in
use). Its 26 TGA backgrounds took 17 MB at 32 bits per texel, in blocks up to 1.2 MB, and after a long
session that failed ("Couldn't allocate memory for targa file"). They are kept at their 16 bits and
expanded for the upload only: the frontend takes 12.4 MB (20 MB of heap in use in all, against 28.6) and
fits in what the city freed. With the heap cut to 35 MB in a test build, four game and frontend cycles
keep 7.3 MB free at the top. With the 34 MB heap and the bank in the linear heap, four frontend and wil
cycles peak at 19.8 MB of heap in an arena of 20.1 MB that stays the same from the first city on (about 12
MB never touched at the top), and 12.7 MB of the 22 MB linear heap.

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
gamepad reads take a lock per call, a system call each on the 3DS (now a button or axis is read
again only after a gamepad event for it, and the pad bindings once from the registry). Night lighting
took 7.5 ms per frame on an Old 3DS driving (11 ms in the worst blocks; 48 lights, 180 lit primitives, each
tested against every light, then its vertices against the 7 or so in reach): in Azahar, binning the lights in
a grid took the tests from 4400 to 470 per frame and the lighting from 1.75 to 1.21 ms, the vertex shader
the rest of it to 0.2 ms (the draw 5.8 to 4.4 ms per frame). Logic runs at the original's 30.3 steps per second
throughout. A second profile (walking and driving, day and night) put the main thread's CPU time at
12 to 14 ms per frame in Azahar: about 3 ms logic, 5 to 7 ms drawing, 3 to 4 ms input polling.
The input time went to the pad reads: SDL's event pump runs about three times a frame (the game's
message loop polls before each step, each frame drawn and each wait), and every SDL gamepad read looks
the pad and its joystick up in SDL's object table, whose lock costs three `svcArbitrateAddress` calls
(22 us each in Azahar, 70 us a read). Now a button or axis is read again only after a gamepad event for
it (an axis also at each pump until its first event: SDL takes its first value without one), and the
key bindings only while a key is held. In ste, in a car then walking (`GTA2_3DS_FPS`'s CPU time): 61
SDL reads a frame became 9, `svcArbitrateAddress` calls 187 became 42, input polling 4.0 ms became 0.75
and the main thread 11.4 ms became 7.9. Most of it is Azahar's system call cost: on hardware perhaps
0.35 to 0.7 ms a frame. The console then measured the pump at 0.7 to 0.8 ms a frame (`GTA2_3DS_PROF`,
2.2 pumps a frame, 0.4 ms of it the pad's update): in game it now runs once per logic step, just before the step
reads its inputs, and no longer after each draw and wait, which read none. In Azahar, bil driving: 3.0 pumps a
frame became 1.0, and their time 0.85 ms became 0.28.
Texture uploads went from up to six passes per texture to one per level, 2.5 to 3 times faster
in Azahar (a 64x64 sprite 2.3 to 0.84 ms, a 128x128 one 9.2 to 3.1 ms; upload time in a frame of a
long drive at most 7 to 10 ms instead of 19 to 29). Remaining leads: fewer draw calls (about 100 per frame) with a tile atlas. The system core runs the sound (see Sound).

Build options, the same walk, three runs each (main thread CPU per frame, logic step): 7.91 and 3.43 ms
without them; 7.80 and 3.30 with unaligned access (clang's bare metal default goes byte by byte where it
cannot prove alignment, left out: see Toolchain); 7.33 and 2.90 with full LTO (ThinLTO 7.38 and 2.96),
the build kept. The draw time with its wait for the GPU stays at 12.4 ms, and the `.3dsx` grows from
2.69 to 2.78 MB with the inlining across files. Azahar has no cache model, so the larger code may cost
more on hardware than it shows here: the options are worth checking on a console (`GTA2_LOOP_STATS`).

On the 3DS any per-frame file access is expensive: each path component of an `fopen`, `access` or
`stat` is a request to the FS service (tens of milliseconds in Azahar). The pause menu and the
frontend's OPTIONS page drew at 3 frames per second because the port's menus checked every frame
whether the language's `data/?.gxt` existed; each language is now looked up once, and the pause menu
draws at the 30 fps limit.

The frame loop's snapshot before each step (`loop.beforestep`, `port/src/interpolation.cpp`) walked all
5031 sprites of the pool: 3.2 ms per frame of a night drive on an Old 3DS (0.29 in Azahar). At FRAME LIMIT
30 nothing is drawn between two steps, so no snapshot is taken (0.00 ms); above it, only the sprites of
the sprite grids around the drawn view are kept (Azahar uncapped: 0.29 to 0.04 ms a step, the frame's
interpolation 0.63 to 0.09 ms).

File access in single frames counts too (Azahar, Old 3DS mode, frame times from a measurement build).
A request costs 17 to 33 ms, a case-insensitive directory scan (`data\audio` for `data/Audio`) 33 to
56 ms, a registry save 35 to 55 ms. Opening the pause menu took 980 ms the first time and 510 ms after:
it loaded `fstyle.sty` (250 ms), the first time the menu sounds (160 to 300 ms), and the game's FREEZE
vocal took 120 to 190 ms to find. The frontend style and sounds now load with the city (3.7 MB of heap
in game), `port_resolve_path` keeps the components and directory listings it found, the registry is not
saved for a value written unchanged and, while a settings menu is open, only as it closes. The opening
frame now takes 127 ms the first time (20 ms of it the menu's first texture uploads) and 68 ms after; a
setting's step 85 ms before, now under 40. Closing takes under 40 ms, or 76 to 82 ms when a setting
changed (the one save). The frontend's OPTIONS page took 223 ms on its first frame, now 113 (55 ms of
it uploads).

The HUD (`draw.hud`) took 1.0 to 1.5 ms a frame on the console, and 4.6 ms in the blocks with a mission brief
shown, at most 7.2 ms in a frame. Timed per element in Azahar (bil, at night): the brief 1.7 to 1.9 ms for its 136
glyphs, each a primitive through the gbh_* layer and a batch of its own; the bottom screen's other parts (stats,
clock, gang bars, health) about 0.5 ms for 32 primitives; the top screen's (arrows, zone and car names) under 0.1
ms but while a name shows. All of it was drawn every frame for a bottom screen redrawn in every other frame at
most, when it changed. The game now leaves the bottom screen's draws out of the frames between, and draws the
brief's text once (see Renderer). Azahar, bil: standing with a long brief, 1.6 to 1.8 ms became 0.32 (at most
3.3 ms in a frame, now 0.66); without it 0.37 became 0.21; driving 0.44 became 0.24 (at most 1.6, now 0.75).

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
- Sound on hardware: the DSP's channels (clicks on stops, OpenDSP's load with 20 voices), sleep.
- Real Old 3DS timings: Azahar's are optimistic for memory bound code (no cache model). If the
  hardware is short, the next candidates are the per-vertex lighting at night and the draw calls.
- Real hardware: a 3DS or New 3DS with custom firmware.
