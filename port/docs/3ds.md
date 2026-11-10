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
  official `devkitpro/devkitarm` with `clang-22`, its linker plugin (`llvm-22-linker-tools`) and `llvm-22`
  (`llvm-profdata-22`, for the PGO profile) added.
- ARM code (not Thumb) for the ARM11 MPCore with VFPv2 (`-mcpu=mpcore -mfpu=vfpv2`), -O2
  (RelWithDebInfo), strict alignment (with `-munaligned-access` clang merged stores into an STRD at an odd
  address, which the ARM11 faults on; Azahar does not check),
  full link time optimisation: the objects are LLVM bitcode, which devkitARM's ld compiles through
  clang's plugin (`LLVMgold.so`), and profile guided optimisation (Build). See Old 3DS, Speed.
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
writes them: `C3D_DrawElements` also scans the 96 vertex and 96 geometry uniforms and the dirty state on every
call. Driving in Azahar, that and the pages cut the batches' submission by 70 %.

The gbh_* layer hands a primitive over as its 3 or 4 corners (`IRenderer::DrawFan`; the OpenGL renderer
expands them into its list of triangles). citro3d writes them as 4 vertices: each four is drawn as the
triangles (0, 1, 2) and (0, 2, 3) of an index buffer written once at start, so a quad takes 4 vertices
rather than 6, and a triangle repeats its last corner (its second triangle is empty).

The game's textures are a cache in VRAM first and linear memory beyond: a budget of what VRAM has free at
start (5.3 MB) plus 8 MB of linear memory, least recently drawn evicted first and converted again on their next
draw. Without it, the textures a long session has drawn outgrew linear memory and showed white. The CPU writes an
upload for VRAM into a staging buffer in linear memory, copied by the GPU's DMA in citro3d's queue ahead of the
frame's draws (`GTA2_3DS_VRAM=0`: linear memory only). Design and measurements: `3ds_texture_memory.md`.

A texture is converted on its first draw: the gbh_* layer turns the palette into RGBA (`tex.convert`), the
renderer makes its levels (premultiplied, converted, in Morton order, each averaged into the next: `tex.upload`).
A slot's levels go straight to their place in the page, only the border is copied there (the 8x8 level, at an
odd place, is made apart and copied). The game's textures have their levels made on a worker thread on the
system core (core 1, of which the application gets 30 %, below the sound's thread there): the main thread
keeps the allocation (budget, eviction, slots) and queues a copy of the texels. Before `C3D_FrameEnd` it writes
itself those drawn in the frame that the worker has not done, oldest first while the worker takes the newest.
A job the worker is in the middle of is stopped and written again by the main thread rather than waited for:
the system core runs the application in slices, and in Azahar, which holds it to its 30 %, a 64x64 slot took
the worker 6.5 to 12 ms against 2.4 ms on the main thread, and a 32x32 job could wait 5 ms for its turn. The
thread that writes texture memory flushes it; storage is released or written again only once the worker has let
go of it (a stopped job may still write the texels the main thread wrote, the same values). `GTA2_3DS_UPLOAD_WORKER=0`
keeps every upload on the main thread.

`C3D_FrameEnd` flushes the data cache over the command list only (`GX_CMDLIST_FLUSH`), not over the whole linear
heap: every CPU write the GPU reads flushes its own range (the frame's vertices, the index buffer once, texels,
the map cache's meshes, the VRAM staging buffers and shadows). Azahar has no data cache, so a missing flush shows on
hardware only.

The effects' frames (code object 112: explosions, fire, smoke, sparks, blood; 205 sprites, the same in the three
styles, 1.3 MB with their mip levels) are listed at the city load and converted at the next scene, on the worker
in the background, rather than one per frame of the first explosions (`port_PreloadTexture`). The next scene,
as with skip_frontend the render options reach the renderer only as the frame loop starts, and their change
converts every texture again. What explosions and collisions convert: `3ds_texture_memory.md`.

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
(4 to 5 pixels high); the bottom screen is black in the frontend and shows the city's map in the pause menu.

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

While the pause menu is open, the bottom screen shows the whole city instead of the HUD
(`Minimap::UpdatePause`, `DrawPause`): the same layers and markers from the same textures, the map scaled
through its UVs at 1, 2 (the default), 4 or 8 pixels per block, centred on the player as the menu opens.
A marker off the screen goes on its edge, on the ray from the screen's middle. Dragging the stylus pans it
(`hidTouchRead`), held within the city, which stays in the middle at 1 pixel per block where it is narrower
than the screen. Two buttons bottom right, squares of the frame's tube around the pause font's `+` and its
bar for the `-` (the font has none), zoom on a press, sunk while held and dimmed at the last zoom; the zoom
stays for the next pause. Under 2 pixels per block the map is filtered, so a pixel is the mean of a block's
2x2 texels. It adds no texture: the two glyphs go in the markers' sheet. It is drawn in every frame in
which it changes, to follow the stylus. SDL's 3DS driver makes mouse events of touches only for a window full
screen on the bottom screen, which the game has none of, so touches reach neither the game nor the menu.

The bottom screen is drawn only when what it shows changed, in every other frame at most (15 per second at
30): a signature of the HUD's batches (vertices, textures and their uploads, states), its panels, the
brief's scroll and the minimap's state (the player's position to the pixel, heading in 64 steps, gang and
markers). A change of the minimap alone waits until 100 ms after the last draw (`kMinimapMs`): at 30 frames
per second it is drawn in every fourth frame (7.5 per second) rather than every other one, as its draw is most
of the screen's cost; any other change (numbers, the brief, its scroll and face) is drawn at once, with the
minimap as it then is. citro3d transfers and swaps a screen only when its target was drawn on, so otherwise
the screen keeps its last frame. The game leaves the bottom screen's HUD draws out of the frames in between
(`PortHudScreen::Drawn`, `IRenderer::HudScreenDue`). The brief's text, drawn glyph by glyph (136 draws for a long
one), is drawn by the game once: while its text stays the same, the renderer copies the vertices and batches it kept
(`PortHudScreen::Keep`, `IRenderer::KeepHudGroup`), unless one of their textures changed; the signature takes the
hashes of those batches' vertices made when they were kept (with a brief shown, the signature takes 0.03 ms per
frame in Azahar rather than 0.09 when it hashed them every time).

### Stereo

With the 3D slider up (`osGet3DSliderState`), the top screen shows a picture per eye (`gfxSet3D`), and the CPU
still draws once. The frame's commands from its start to the end of the top screen's draws (the world, the HUD left
on the top screen, the FPS overlay) are written once, as for one eye, and the GPU runs them twice: after a command
list of its own that sets the left eye's parallax, then, once the top target has been transferred to the left eye's
framebuffer, after their end has set the right eye's. citro3d's frame end then transfers the target to the right
eye's framebuffer with the bottom screen's (`C3DContextSetStereo` links the top target to `GFX_RIGHT`). One render
target serves both eyes, so no VRAM is taken. The second run starts from the state the first one left, so a stereo
frame writes at its start what its draws use and citro3d would otherwise leave out as unchanged: the shader program
(`shaderProgramConfigure`) and the vertex uniforms (about 2 KB of commands); BeginFrame and ApplyState set the rest at
the first draw.

The parallax is in the vertex shaders (`render.v.pica` and `map_cache.v.pica`): the eye is a fixed vertex attribute
(v4 and v2), written into the command buffer at the frame's start and at the end of the top screen's commands
(citro3d writes its fixed attributes at the next draw, which would be inside the commands that run twice). A vertex
at depth w (the game's 1 / rhw: camera z + 8 - z, `sprite.cpp`) moves along the screen by e (1 - w0 / w), so in clip
space by e (w - w0). The screen plane w0 is the player's depth, eased over a few frames
(`stereo_game.cpp`): the street around the player is at screen depth, what stands higher (buildings, roofs, the
player on a bridge) comes out, what lies lower goes in. The HUD stays at screen depth: the game draws it at w = 10000
(zone and car names, messages, arrows) and the port's 2D draws at w = 1 (the frontend, the CPU surface, the FPS
overlay), while the world lies between 2 and 8 walking and up to about 22 zoomed out, so the shaders move only w in
[1.01, 1000). Each eye moves by the slider times `GTA2_3DS_STEREO` pixels (2.5 by default, 0 keeps the screen 2D) per
unit of 1 - w0 / w: walking in bil, roofs come out by 4 to 7 pixels between the eyes at full slider. The frames are
in stereo only while the last one drew something at the world's depths: the frontend stays 2D (the parallax barrier
off), the pause menu shows over the city in stereo, at screen depth.

In Azahar, walking in bil, the draw takes 2.85 ms per frame and the present 0.39 with the slider at 0 and at its top
alike, as without the stereo: the second eye costs the CPU a few microseconds. The GPU draws the top screen twice and
transfers it once more; Azahar does not time the GPU, but on the console, where a frame took 4 to 5 ms of GPU time, that
makes about 8 to 10, inside the 33 ms of a frame. Azahar shows both eyes with its stereoscopic mode side by side
(`render_3d=1` in `qt-config.ini`), its depth (`factor_3d`) being the slider.

citro3d's library is built with short enums (GCC's default for `arm-none-eabi`), the port with int ones: a
`C3D_RenderTarget`'s fields after its frame buffer's enums are not where the port's compiler looks for them, so
`c3d_context.cpp` keeps the top target's transfer flags itself.

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

### Profile guided optimisation

The build optimises with the counts of the Linux port playing the game (`GTA2_3DS_PGO`, on by default;
`-DGTA2_3DS_PGO=OFF` builds without). `port/3ds/pgo/gta2.proftext` keeps them as text, which the image's
`llvm-profdata-22` indexes at build time whatever LLVM wrote it, and every file is compiled with
`-fprofile-instr-use`. The counts travel in the bitcode to the LTO link: inlining, block layout (the
taken path falls through, cold blocks go to the end of the function), hot functions in `.text.hot`, the
never run ones in `.text.unlikely` and optimised for size. The game behaves the same, only the code's
layout changes.

A function takes its counts only if its control flow is the profiled one (clang hashes it): code under
`#if __3DS__` keeps the static heuristics, and `port/src/prof.hpp`'s macros have the same conditions on
every platform for this. Of the 5,887 functions of the 3DS build, 5,519 take their counts, 23 do not match
(0.14 % of the calls: the DSP path of `compat/src/miles.cpp`, a few in `gbh_api.cpp` and `frame_loop.cpp`,
the x86 asm of `gtx_0x106C.cpp`, 3DS functions named as Linux ones) and 345 have none, nearly all in the 3DS's
own files. Functions the scenes never ran count as cold.

A cache simulation of the Linux port (16 KB 4-way L1 caches, its x86 code layout, the instruction
addresses also scaled by 1.3 for ARM code; a drive and a walk in ste other than the profiled scenes), per
logic step without and with PGO, the second figure with the functions in link order as on the 3DS:

| | I1 misses | I1, ARM scale | D1 misses | blocks run |
|---|---|---|---|---|
| drive | 24,363 to 19,935 / 18,629 | 33,596 to 27,024 / 26,517 | 9,326 to 9,212 / 9,315 | -2.6 % |
| walk | 19,251 to 15,364 / 14,291 | 26,954 to 20,731 / 20,262 | 8,754 to 8,802 / 8,736 | -2.5 % |

So 20 to 25 % fewer instruction cache misses in the logic, about 1 ms of a night drive's step on an Old 3DS
at 40 to 50 cycles a miss, and none fewer for data; the draw's shared code (the OpenGL renderer aside)
misses half as often. Per-50-step hashes of the cars, peds and `rand` are the same with and without, in
Azahar (walk and drive, day and night, up to 2,600 steps) and on Linux (2,000 steps, the sound queue
included), and so are Azahar's frozen frames. The sound
queue's own count can drift by a few samples after a while in Azahar: it asks the DSP which channels still
play, so it depends on timing, and the same build drifts between the day and the night run of a drive.
Azahar, which counts instructions, gives the step 8 % less time (2.75 to 2.52 ms driving by night). The
`.3dsx` grows by 90 KB.

After larger changes to the shared code, regenerate the profile (stale functions only lose their counts):

```bash
port/3ds/pgo/train.sh <game folder with data/> <work folder>
```

It builds the Linux port (`port/toolchain-linux32-clang.cmake` and its 32-bit dependencies; `SETUP_DEPS`
for `GTA2_SETUP_DEPS_DIR`) with `-fprofile-instr-generate`, plays six scripted scenes at once, headless and
silent (about 2 minutes): a fast car by night and by day and the tank firing into traffic in wil, the tank
in ste, a walk in ste, the frontend and the pause menu. It merges their counts into `gta2.proftext` with the
host's `llvm-profdata` (about 650 KB, 140 KB compressed in git).

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
0.37 s of decoded frames, topped up without the lock the game's sound calls take. Only the first open
of a file runs on the main thread, 30 to 60 ms with its header read in Azahar (an Old 3DS driving showed
25 to 78 ms spikes in the logic's sound part about every 5 s, most likely the opens and closes): the format
is kept, and the audio thread opens the file again and closes it, so a later open costs the game 0.1 ms
and a close nothing.

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
The port's log is `gta2_log.txt` there (stderr and SDL's log), or `gta2_log_<name>.txt` for a `.3dsx` not named `gta2`.
`gta2_env.txt` there sets environment variables, one `KEY=VALUE` per line: `GTA2_3DS_STATS=150` logs
memory and renderer times every 150 frames, `GTA2_LOOP_STATS=1` steps, frames and their times,
`GTA2_3DS_FPS=1` shows frames and logic steps per second, the main thread's CPU time per frame
(without its waits for the GPU and its sleeps) and the heap and linear memory in use in the top screen's corner,
over the last second and refreshed 4 times per second, `GTA2_UNCAPPED=1` draws as fast as it can (the logic
keeps its 30.3 steps per second, citro3d does not wait for the screens' refresh) to measure the headroom
(Azahar, bil: about 185 frames per second walking at noon, 165 driving through traffic, about 100 driving at dusk,
the same in New 3DS mode, which runs the same clock; Azahar leaves out the GPU's time),
`GTA2_AUDIO_DRIVER=dummy` runs the sound without the DSP, `GTA2_3DS_VRAM=0` keeps the textures in linear memory
(`GTA2_3DS_STATS` logs what VRAM holds, the DMA copies and the waits for them), `GTA2_3DS_UPLOAD_WORKER=0` makes the textures' levels on
the main thread (see Renderer; `GTA2_3DS_STATS` logs the worker's time and the jobs the main thread wrote), `GTA2_3DS_MINIMAP=wide` or `off` sets the
minimap's place (see Renderer), `GTA2_3DS_STEREO=<pixels>` the stereo's strength (see Stereo, 0 for none),
`GTA2_3DS_EYES=<frame>,...` writes the top screen's left and right framebuffers of those renderer frames to
`test/eyes_<frame>.bin` (BGR8 as the screen holds them, 240 by 400 each, the left eye's first; `port/3ds/eyes_png.py`
makes PNGs of them, side by side and red-cyan too). To boot into a city, the registry's `Debug` keys
`skip_frontend`, `mapname`, `stylename` and `scriptname` (see `Source/debug.cpp`).

`GTA2_3DS_PROF=150` (`port/3ds/prof_3ds.cpp`) logs every 150 frames of a game where the main thread's time goes.
Per logic step: the step and its parts (`Game::UpdateGame`'s services in order, the sound, one level deeper in
the cars and the peds, the sprite collisions). Per frame: the input reading, the event pump and the pad, the
sleeps, the draw (sprite sort, map, HUD, the gbh_* primitives with their lighting and their citro3d part, the
batches' submission), the bottom screen, `C3D_FrameEnd`, texture conversion and uploads, the waits for the top
screen's refresh (`wait.vsync`, see Pacing), for the bottom screen's after it (`wait.bottom`) and for the GPU apart,
and citro3d's own GPU and CPU times. Counts: primitives, draw calls,
vertices, texture binds, lights and the lights tried per vertex lit on the CPU, tiles, sprites, cars, peds, whether the
player drives, the steps per frame, the top screen's refreshes since the last frame (`refreshes`, 2 at FRAME LIMIT 30)
and the frames over 42 ms after the last (`late`). Each part gives its average, its most in one step or frame, its time without the parts under it,
and its calls. The timers' own cost, measured at the start, is taken out of the times and logged; one primitive
in `GTA2_3DS_PROF_SAMPLE` (default 4) is timed, with what it runs, and counted for that many. Without the
variable, a timer costs a test. `python3 port/3ds/prof_summary.py gta2_log.txt` sums a log up in tables,
walking and driving, day and night apart. The car visit is split by kind of car (parked, AI driven, moving
without an AI driver, trailers and trains) and by its parts (the sprite grid, despawning, doors and lights,
the rest), the physics by its collisions, forces, movement, ped scaring and skid marks, the peds by state;
counts give the map column reads, sprite collision tests and sprite grid cells walked per step.
`GTA2_3DS_PROF_PMU=1` adds the ARM11's two event counters (`svcControlPerformanceCounter`), read with each
tick: per part and step, a pair of events per block of lines in turn (data cache read misses and instruction
cache misses, the stall cycles on data and on instructions, data cache write misses and main TLB misses,
instructions and mispredicted branches), less what the timers themselves count; a scope then costs about
twice as much, and Azahar reads zeros. `GTA2_3DS_PROF_AB=logic` (or 1) runs the logic options of
`port/src/logic_opts.hpp` as set in one block and all off in the next, `GTA2_3DS_PROF_AB=mapcache` the map cache
(`GTA2_3DS_MAPCACHE`) the same way, `GTA2_3DS_PROF_AB=soundcore1` the sound pass on the system core in one block
and in the step in the next (whatever `GTA2_3DS_SOUND_CORE1`, the pass in flight joined before the change), and a
comma separated list of them in a cycle of four or eight blocks, every combination of states (`prof_summary.py
--options` groups the blocks by them). Off, the map cache keeps its built chunks for the next block on. The lines of `GTA2_3DS_PROF`, `GTA2_3DS_STATS` and `GTA2_LOOP_STATS` are
kept in memory and written 8 KB at a time by a thread of their own on the system core, below the sound's feed and the
texture worker, and the rest at exit (`port/3ds/log_3ds.cpp`): on an Old 3DS such a write to the SD card takes up to
a fifth of a second, which on the game's thread would stutter every 15 s. The `prof:` lines give the last write's size and time;
should the writer fall 32 KB behind, a `log:` line counts the lines dropped. Each block has a `prof place:` line:
the player's point in blocks at its start and end, his vehicle's model (-1 on foot), his speed in blocks per step
(average and most) and the camera's height (average, least, most: 1 standing, about 3.8 at an R-type's top speed).
A frame over `GTA2_3DS_PROF_SLOW` ms (45 by default, 0 for none) gets a `prof slow:` line, up to 8 a block: its
time, steps, the player's point, speed and camera height, its ten largest parts (step ones over its steps, nested
ones inside their parents) and a few counts. `prof_summary.py --blocks` shows the place per block, `--slow` the
slow frames.

Azahar (2126.2) runs the `.3dsx` without firmware or system files, in Old or New 3DS mode. It passes
no arguments, so the game folder there is always `sdmc:/3ds/gta2/`. The start
to the first city frame takes about 15 seconds there.

Tests: `test/pad_input.txt` presses buttons, one `<first frame> <last frame> <hex KEY_* mask>` per line
(frames as `port_FrameCount` counts them), for example `150 170 8` for START; the `KEY_CPAD_*` and
`KEY_CSTICK_*` bits push the circle pad and the C-stick all the way, and two more numbers push the circle pad
part of the way, as `hidCircleRead` gives it (up positive, about 154 at the end of the travel), for example
`600 700 0 -109 109` for up and left. With `KEY_TOUCH` (100000) the two numbers are the stylus's point on the
bottom screen, and two more its point on the last frame, for a drag: `900 960 100000 160 120 60 60`. `GTA2_TEST_CAR=<frame>:<model>[:<dx>:<dy>]`
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
| Y, X | previous, next weapon | previous, next weapon |
| D-pad left, right | specials 1 and 2 | the roof gun's turn (fire truck, tank, gun jeep), else specials 1 (horn, siren) and 2 |
| D-pad up, down | special 3, nothing | special 3, attack |
| START | pause menu, with the city's map on the bottom screen: drag it with the stylus, + and - zoom | |
| SELECT | back in menus | the next radio station (the PC's F1) |
| ZR, ZL (New 3DS) | attack, nothing | attack, nothing |
| C-stick (New 3DS) | MODERN: aims (TWIN STICK's aim) | MODERN: turns the roof gun |

CLASSIC D-PAD moves on the d-pad instead, and the circle pad takes what the d-pad did:

| Button | On foot | In a vehicle |
|---|---|---|
| D-pad | up, down forward, back; left, right turn; diagonals both | left, right steer; up special 3, down attack |
| Circle pad left, right | specials 1 and 2 | the roof gun's turn, else specials 1 and 2 |
| Circle pad up | special 3 | |
| L | next weapon | brake, reverse |

The other buttons are as above (R attack or accelerate, B, A, Y, X, START, SELECT, ZR).

CONTROLS (pause menu or OPTIONS) chooses MODERN, the default, CLASSIC, the original's tank movement, or
CLASSIC D-PAD, the same on the d-pad; the choice is `port_console_controls` in the registry (1, 0 and 2),
apart from the PC's `port_controls`. MODERN is TWIN STICK
(`port/src/twin_stick.cpp`): without an aim the player turns to the way he walks; while L is held his
facing stays the one he had at the press (`port_HoldFacing`), and the C-stick's aim comes first. Shots keep
the original's lock on a ped near the line of fire, except while the C-stick aims (`PortTwinStick::FreeAim`). CLASSIC reads
the circle pad as the original's arrows by the way it points past the dead zone, an 8-way gate: up or down
alone within 30 degrees of straight, left or right alone within 22.5, both in between, so a diagonal runs and
turns (a threshold on each axis misses the diagonals of a round gate). On foot, in either style, the circle
pad pushed less than 0.55 of its range walks at the peds' walk speed, with their walk (`PortTwinStick::WalkSpeed`
in `Char::UpdateWalking`), so the player can turn on the spot; further, he runs. The dead zone is 0.25, and the
circle pad gives about 0.96 at the end of its travel, diagonals too (Azahar: 154 of the 160 that SDL's 3DS
driver takes as full, 109 on each axis on a diagonal), so a run is reached a little past half way (not in CLASSIC D-PAD, where the d-pad runs). In MODERN and
CLASSIC only the circle pad moves, so that the d-pad keeps the specials; L on foot in CLASSIC D-PAD gives the
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
Uploads in a crash and explosion scenario (Azahar, Old 3DS mode, main thread per frame while driving, the
first block left out): the tank from `GTA2_TEST_CAR` firing its shells into traffic by day (24 blocks), and a car
driven into traffic at night (7 blocks). `tex.*` is the average per frame, its worst block's average, and the
worst frame:

| | tank, by day | car, at night |
|---|---|---|
| before | 0.22 ms, 0.93, 5.7 | 0.67 ms, 1.39, 16.5 |
| slots written in place, effects preloaded, worker off | 0.20 ms, 0.56, 4.9 | 0.56 ms, 1.04, 12.5 |
| the same with the worker | 0.16 ms, 0.35, 4.9 | 0.46 ms, 0.73, 9.1 |

The worker wrote 72 and 64 % of the jobs (busy at most 5 % of the time); the frames over 42 ms stay at 3 %,
Azahar's own hitches. Azahar is 4 to 6 times optimistic on the console, so its milliseconds are a guide only.

The draw's primitives: on an Old 3DS driving at night (`GTA2_3DS_PROF`, 26 fps), the draw took 14.9 ms of CPU
per frame, 11 ms of it in its 346 primitives (235 tiles, 51 sprites), about 30 us each, nearly all in the
port's gbh_* layer and citro3d backend. The profiler weighs on those parts there: in each timed primitive (one
in 8) every part reads the system tick, a system call, and the cache misses that follow count in the parts;
`GTA2_3DS_PROF_SAMPLE=100000` leaves the primitives untimed when only the totals matter. In Azahar (frozen
frames, primitives untimed), cutting the path at points split a day drive's draw (4.7 ms, 373 primitives)
into the game's own code 2.4 ms, the gbh_* layer 0.9 ms (the calls down to `RenderVertsUnturned`, the window
test and the texture lookup about 0.16 ms each, the corners and state 0.43) and citro3d 1.5 ms, half of it the
draw calls. Since then the profiler's sections cost a test when it is off (they were a call each, seven per
primitive), `LogCall` returns at once without a command log, `DrawTile`'s turn and flips are one corner
permutation, a primitive reaches the renderer as its corners (`IRenderer::DrawFan`) without arrays on the
stack (`-ftrivial-auto-var-init` cleared about 500 bytes per primitive), `OneToOne` compares instead of
dividing, the rare paths (trace, command log, soft shadows, lighting) are out of line, `NearLights` skips
the lights below the first asked for, a quad is 4 vertices instead of 6 and unlit vertices carry no world
position. The draw went from 3.46 to 3.12 ms walking by day, 3.63 to 3.32 by night, 5.15 to 4.64 driving by
day and 5.43 to 4.94 by night (9 to 10 % in Azahar, which has no cache model), both screens the same to the
pixel.

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

The map (`draw.map`, about 10 ms a frame on the console driving at night, 300 primitives with the sprites drawn
between its layers). `GTA2_3DS_MAPCACHE` (on unless 0, `port/3ds/map_cache.cpp`; on an Old 3DS, driving at night, draw.map 11.0 to 8.6 ms on average and 17.9 to 10.2 in the worst block, walking 6.9 to 7.2) builds the flat lids of each
16x16 block chunk once into a mesh in linear memory, drawn at the end of their layer by a vertex shader of its own
(`map_cache.v.pica`), a draw call per chunk and atlas page: a lid only overlaps the faces of nearer blocks of its
layer, which the walk draws first, so the picture stays the same. Animated or remapped tiles, textures not converted
yet, chunks that a light past the shader's 44 reaches and the turned view stay with the game. The walk then leaves out
of its draw list the cells whose `RenderBlockAt` would draw nothing, or nothing but such a lid (a bit per cell and
layer kept with the chunk, on the heap). Azahar, frozen frames in ste, `draw.map` in ms:

| | game | lids cached | and the cells left out | cells walked |
|---|---|---|---|---|
| driving at night | 2.17 | 1.81 | 1.67 | 9 of 528 |
| driving by day | 1.93 | 1.63 | 1.48 | 9 of 528 |
| driving at night, elsewhere | 2.49 | 2.21 | 2.10 | 27 of 496 |
| standing at night | 1.69 | 1.37 | 1.26 | 40 of 560 |
| driving by day, elsewhere | 2.27 | | 1.93 | 27 of 496 |
| standing by day | 1.52 | | 1.17 | 40 of 560 |
| a car's lamps near (lids left to the game) | 2.41 | | 2.44 | 110 of 500 |

The pictures are those of the game's own path, at most 27 pixels 1 off in a channel (the lids' projection). A built chunk takes 14 KB of linear memory in these
scenes (2 or 3 chunks drawn), at most 48 are kept. Left to the game, and why: the walls of the walked cells, 9 to 22
primitives a frame here in 4 to 9 (chunk, layer, page) groups, 0.06 to 0.38 ms in Azahar with the slopes' sides (the
frame without any wall, `skip_left` and the like in the registry). A mesh of them would need the walk's order
where faces overlap (dominance order per quadrant around the camera, the camera's row and column walked) and
would still cost a draw call per group, 7 to 13 us in Azahar against about 12 for a wall through the gbh_* layer:
at best 0.15 ms of Azahar's 0.38, for several hundred lines. Slopes and diagonals draw 0 to 5 primitives a frame.
The meshes' own cost is 0.19 ms a frame in Azahar, half of it the first draw after the switch of shader program.

On the console (night drive, `GTA2_3DS_PROF_AB=mapcache`) the cache took 76 lids a frame from the game but saved
only 0.3 ms of `draw.map`, and its worst blocks were worse. `draw.map.cache` times it per frame: the checks of the
chunks' tile mappings and texture versions, the builds (blocks, quads, sort, mesh), the test of the lights past the
shader's, and the draws (the game's batch, the switch to the mesh program, the draw calls, the switch back, both
switches updating citro3d's context themselves so that their uploads show there); `mc.*` count the chunks checked,
built, drawn and left to the game, the rows left for a light, the lids drawn, taken and left out of the walk. Its
log line showed where the time went: a build took 5.1 ms (7.2 at most) in one frame, 5 to 22 of them in 150 frames
driving; the draws 0.6 ms a frame; the checks and light tests 0.3; and at night 3 chunk layers a frame went back to
the game whole for a light past the shader's 44. Now a build reads the blocks once, resolves a lid's tile once and
sorts 8-byte keys (Azahar: 4.4 ms, now 1.2, at most 1.3); such a light leaves to the game only the rows it may
reach, which it lights on the CPU as before; after the first draw of a state through citro3d (whose context update
walks every uniform of both shader stages, 11 to 17 us a draw in Azahar), a layer's draws write the texture, the
chunk's two uniform rows and buffer offset, and C3D_DrawArrays' commands themselves; and the lights are looked up
once per chunk and frame, for the chunks on screen. Azahar, `draw.map` in ms: frozen at night in ste with 47 lights,
1.93 without the cache, 2.36 with it before, 1.85 now; the scripted night drive 1.87 without it, 1.75 now (before,
slower than without it in most blocks). With the shader at 32 lights (a test build) the frozen frames are the
game's to the pixel. Left: two program switches per layer with lids, about 20 us each in Azahar, and a draw through
citro3d per state and layer.

The cache simulator (the Linux port, see the logic's memory traffic), zoomed out on a fast drive, per frame: the
walk's draw list misses little (123 data, 52 instruction cache misses); `RenderBlockAt`'s map reads (base table,
columns, blocks) about 730 data misses, about 0.13 ms at 50 cycles; the rest of `draw.map` is the gbh_* path per
primitive: data misses on the texture lookups (300), the lights (sqrt table 280, the light grid's clear 255) and the
vertices, and above all instruction misses (9,300 per frame on x86 without PGO). No cheap change in the walk itself;
the per-primitive path is what the cache saves.

The bottom screen's CPU time per frame (`GTA2_3DS_STATS`, walking, Old 3DS mode): 0.38 to 0.42 ms
(0.45 to 0.51 at most) when it was drawn every frame without a minimap; with the minimap and drawn only
on a change, 0.05 ms standing (5 frames of 150 drawn, for the clock), 0.18 to 0.44 ms walking (up to 75
of 150), and 0.65 to 0.87 ms in a frame that draws it. The frame rate stays at 30.3 in bil and ste. With
the territory edges and icons and the game's sprites (ste, the same walk on both builds): 0.029 ms
standing against 0.027, 0.19 against 0.17 walking with the same frames drawn, 0.69 to 0.74 ms in a frame
that draws it against 0.59 to 0.66.

The logic step on an Old 3DS (`GTA2_3DS_PROF`, driving at night, 26 fps): 7.6 ms, of it cars 3.9 (the
visit of the 63 or so cars 1.2, the AI of about 15 of them 1.0, the physics of 17 1.6), peds 1.6, sound
1.1 (the entities 0.9, with the spikes above), collisions 0.5. Three costs that change nothing in the game
are gone. Fix16's division, `(a << 14) / b` in 64 bits, went through the C library's 64-bit divide (the
ARM11 has no divide instruction), 150 to 330 times a step and frame; it is the VFP's double division now,
exact as `|a << 14| < 2^53` (2e8 random and edge pairs; in the scripted drive 581,000 divisions, all the
same as the 64-bit one). The sound pass ran ten handlers for each car, though about 140 of the 150 are
past every distance six of them check (20 blocks at most) and those keep no state before that check; for
them only the four that do run, and their column's roof test (for the volume) is not made, but for
trains. The stream opens and closes moved off the main thread (Sound). In Azahar, the same lockstep drive
(2600 steps, night): the step 2.36 to 2.22 ms, the physics 0.34 to 0.30, the sound entities 0.43 to
0.37. The checks: a hash of every car's and ped's position, rotation and speed, of `rand()`'s state and
of every sample the sound queues, every 50 steps, the same in both builds over the drive and the walks,
and the same frozen screenshots.

Left as they were. The visit of a parked car spends a quarter of its time taking the car out of the
sprite grid's cells and putting it back (0.12 ms per step in Azahar for about 100 cars): keeping such a
car in place and moving its links to the front of their cells, as the removal and the re-add leave them,
saved 0.03 ms but changed the order of the collision sounds, so it was dropped; the rest of the visit is
the game's own state (doors, lights, despawning). The physics and the collisions have no other obvious
cost of this kind (30 square roots a step, through the VFP; the grid's tests visit the cells under each
sprite only); on hardware they are 6 times slower than in Azahar against 4 for the rest, which points to
memory access (the map's columns and blocks, the grid's lists), which Azahar does not model.

The logic's memory traffic. In Azahar, the lockstep night drive visits about 125 cars a step: 103 parked
(0.45 ms), 15 AI driven, 6 trailers and trains (0.3 ms, the railway's map reads), and reads the map's
columns 2800 times (`GetBlock` 1000, `GetEffectiveBlock` 560, `GetBlockClamped` 500, the roof test 140,
the railway 125), walks 3000 sprite grid cells and tests 90 sprite pairs; per object the console is about
5 times slower than Azahar for the visit and the peds, 7 for the AI and the physics. Two options cut that
traffic without changing a result (`port/src/logic_opts.hpp`, each off unless its variable is 1, as on the
console neither gains anything measurable: the night drive's logic 9.82 ms per step with both off, 9.90 with both on):
`GTA2_3DS_PREFETCH` (`port/src/prefetch.cpp`) has the car, ped and object pool walks ask, while they update
an item, for the item 3 ahead, what the one 2 ahead points to (sprite, physics, AI, a ped's Char) and the
next one's collision shape or sprite, each pointer read from memory asked for one item before (PLD; Azahar
shows nothing). `GTA2_3DS_GRIDKEEP` keeps a parked car in the sprite grid through its visit: for a car
without physics that is not despawning, crushing, towing or carrying anything, not a truck cab or TV van
and not about to wreck, nothing between the removal and the re-add uses the grid, so its links are moved
to the front of their cells, which is what removing and adding it leaves (a row that is not as expected is
redone the game's way); in Azahar 25 % fewer grid cells walked and the parked cars' visit 0.45 to 0.41 ms.
The lockstep hashes (cars, peds, `rand()`, sound) match over the four scenes. The sound queue's hash depends
on the host's timing, as the game asks the mixer whether a sample still plays: the same logic can shift it
by a few samples (a run with both options off did once, the event counters' slower step does), while the
cars, peds and `rand()` never move. On the console, `GTA2_3DS_PROF_AB=1` with both options at 1 and
`GTA2_3DS_PROF_PMU=1` compares them on and off in one session, with the cache misses and stall cycles of each part.

Phases over all cars. A cache simulation of the Old 3DS's L1 (16 KB of code) on the Linux port puts most of the
step's instruction misses in the cars: each car's AI and physics run 8 to 18 KB of code, which the next car's
evicts. Running each phase for every car before the next (all the AI, then all the physics) is not exact, as
a car's update reads what the cars before it wrote in the same step: the car is out of the sprite grid while
it updates and back in at its new place after, the AI's probes (`FindCollidingSpritePreferType`) see the earlier
cars there, a collision sets the other car's `mCarInCollision`, which its AI reads later in that step and then
clears, and the collision pushes and damages it before its AI runs. The scripted Linux drive and walk (2000
steps each) have 364 and 367 AI probe hits on a car already moved in that step, and 17 and 31 collisions
flagging a car still to come. Both phases draw from `rand()` (the AI 4 and 6 times, the physics 265 and 48; none
of the AI's draws followed a physics draw of the same step there, but nothing bounds it). The peds are worse:
the ped AI and its movement (`Ped::UpdateCharB4`) pass per ped globals (`gDistanceToTarget`, `gTargetSearchMode`)
and draw 24 and 7 numbers a step driving, 20 of the AI's after a movement draw. A test build that runs the cars' AI first
(each car out of the grid and back around it) changes the game state between steps 350 and 400 of the drive.
Even inexact, the split buys little: with the AI and the physics each in a pass of its own the step's
instruction misses fall 23 % driving and 15 % walking, against 31 % for the exact code layout (PGO), and its
data misses rise 12 and 9 %.

The system core cannot take the car physics either: each car's physics is a link in that chain (the next
cars' AI, the peds it runs over, the objects, explosions and particles it makes, `rand()`, the collision sound
queue), and the rest of the step reads the cars where the physics left them, so any overlap changes the
results. Only work on state that nothing writes while it runs could go there, joined before the next step,
and at the application's 30 % of that core it runs 3 to 5 times slower than here (the texture worker).

The sound pass is such work. It runs at the end of the step (`Game::ExecuteGame`, after `UpdateGame`) and reads
the cars, peds, objects, sprites, cranes, crushers, weapons, the map's columns and zones, the game camera and the
step's collision queue; outside its own tables it writes only what nothing reads before the next step: the events
it takes from the cars, peds and weapons, the crime report queue, the ped's scream timer, the CarInfo globals the
physics sets again before use, and the map's zone iterator. At FRAME LIMIT 30 the draw writes none of what it
reads (it moves no sprite, the CHINATOWN view turns the view camera and the pass uses the game camera), makes no
`rand()` draw and no sound call. `GTA2_3DS_SOUND_CORE1` (`port/3ds/sound_job.cpp`, on unless 0; about 0.5 to 2 ms less per step on an Old 3DS) keeps the
start of `sound_obj::Service` in the step (its 5 `rand()` draws, as newlib keeps the generator's state per thread,
the pause test, the frame counter, the view's turn) and runs `ServiceSoundEffects` on a thread of the system core,
with the main thread's FPSCR, while the frame draws; it is joined before the next step, before a window message
(focus releases the sound), after a step that exits, and at any frame loop action but a draw. It stays in the step
while paused, with the pause menu open (its draw plays sounds and changes settings), when the draws interpolate
(they move the sprites) and outside the port's frame loop. The pass can allocate DSP buffers and open a stream on
that thread, so the linear heap's calls and the path resolver's caches are under locks. The lockstep scenes
(Azahar, Old 3DS mode; cars, peds and `rand()` every 50 steps, walk 2100 steps, drive 2600, day and night) match
throughout. The sound queue's hash is one of two sequences, which part from step 600 walking and 1800 driving:
two runs of the plain build of the same scene gave both, and each run with the option gave one of them.
The main thread's step went from 2.50 to 2.20 ms walking by day, 2.55 to 2.17 by night, 2.55 to 2.23 driving by
day and 2.59 to 2.20 by night (the sound part 0.38 to 0.025 ms). On the system core the pass started 0.6 ms after
its hand-off on average (at most 4.6 to 8 ms), ran 0.67 to 0.71 ms (at most 5.8 walking, 32 to 34 driving, a
station's first open), and the join waited in none of 1650 steps walking and 2 of 2250 driving (at most 21 ms).
On a console the pass takes about 1 ms of the step: 3 to 5 ms there, plus the wait for its turn, inside a draw of
14 ms. That needs a console to confirm.

### Pacing

The screens refresh at 59.83 Hz (4481136 ticks of the system clock), so FRAME LIMIT 30 is a frame every 2
refreshes, 29.9 per second, while the original steps its logic every 33 ms, 30.3 per second. On the 3DS the
frame loop runs on the top screen's refresh rather than on that timer (`NextOnRefresh` in
`port/src/frame_loop.cpp`, the waits in `c3d_context.cpp`): it waits for the refresh (citro3d's frame counters),
begins a frame every 2 refreshes (FRAME LIMIT 30; every refresh at 60, a test value the menu leaves out) and
steps the logic every 2 refreshes, 29.9 steps per second, 1.3 % slower than the original, as console games
run on their screen's clock. Waiting for the refresh is the frame loop's only sleep, and the frame's first draw
then does not wait again. citro3d's frame end hook holds the frame until the refresh after its start before it
goes to the GPU, so that it shows 2 refreshes after its start whatever its cost. A frame that ends late, but
within its next slot, begins at the next refresh and the delay is dropped (no catch-up step: the game slows down
by that much); after a longer stall the steps catch up, 4 at most. The refreshes during HOME or sleep are not
counted. `GTA2_UNCAPPED` keeps the timer. The PC port is unchanged: this code is 3DS only.

With the timer, the frame's start drifted across the refresh, and the wait for the refresh fell between the
step and the draw: about every 2.6 s a frame came 1 refresh after the last and one 3 after, with 2 steps.
An Old 3DS driving at night (`GTA2_3DS_PROF`) ran 29.3 frames per second, 1.03 steps per frame, 6 % of frames late,
with 10.0 ms of `wait.vsync` and 2.2 of `loop.next` per frame. In Azahar, the drive at night, with busy loops
in each step and draw (a measurement build) standing for the console's times or half of them:

| Azahar, drive at night | Frames/s | Steps per frame | Frames off their 2 refreshes | Frame start after the refresh |
|---|---|---|---|---|
| timer, console-like (+6.4 ms a step, +6.0 a draw) | 29.4 | 1.03 | 4 % | 10.7 ms (0 to 16.7) |
| refresh, console-like | 29.6 | 1.00 | 2 % | 0.01 ms |
| timer, +3.5 and +3.5 ms | 30.2 | 1.01 | 3 % (1 refresh or 3) | 8.7 ms |
| refresh, +3.5 and +3.5 ms | 29.9 | 1.00 | 0 % | 0.01 ms |

The frames still off their slot took longer than 2 refreshes (steps or flips of 25 to 40 ms in Azahar, the same
with either pacing). At FRAME LIMIT 60 (allowed in a measurement build) the frames run at 59.1 per
second and the logic at 29.92 steps; after `GTA2_TEST_SUSPEND` the second after the resume runs 31 steps.
On the console, `GTA2_3DS_PROF`: `refreshes` about 2.00 and `steps` 1.00 per frame, `late` near the share of
frames whose work passes 33 ms, `loop.next` near 0 and its sleep in `wait.vsync`, `wait.bottom` near 0 (the
bottom screen's refresh comes with the top's: if not, it is the time waiting for both costs).

## Next steps

- HUD: the zone and car name banners, pickup texts and big messages on the bottom screen too; a
  second pager; multiplayer scores; bonus stages.
- Menus: the port's own menus (pause, options) with a larger font scale on the 3DS, or on the bottom
  screen at 1:1.
- Sound on hardware: the DSP's channels (clicks on stops, OpenDSP's load with 20 voices), sleep.
- Textures in VRAM on hardware: the GPU's time against `GTA2_3DS_VRAM=0`, the DMA copies' cost, no stale or
  wrong texels after slots are taken again.
- Real Old 3DS timings: Azahar's are optimistic for memory bound code (no cache model). If the
  hardware is short, the next candidates are the per-vertex lighting at night and the draw calls.
- Real hardware: a 3DS or New 3DS with custom firmware.
