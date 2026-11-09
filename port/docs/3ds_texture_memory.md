# 3DS texture memory

The 3DS renderer (`port/3ds/c3d_renderer.cpp`) keeps the game's textures in VRAM while it has room and in
linear memory beyond it (see VRAM below); the port's own textures stay in linear memory. Figures below are from
Azahar in Old 3DS mode, scripted walking and driving with the pause menu every 50 s (`GTA2_3DS_STATS`, and
per-frame logs from a measurement build), with linear memory only and a 16 MB linear heap unless said otherwise.

## Why a cache

The game registers every texture of its style when a city loads (`TextureCache::LoadStyleTextures`: 1024
tiles and every sprite in every remap) and frees them only when the city ends. The gbh_* layer converts a
texture on its first draw, so what is resident grows with what the player has seen: in bil, 1.3 MB of
textures after a minute, 8.6 MB after ten, 11.7 MB after 21 minutes, when the heap was full. From then on
uploads failed and those textures (tiles, sprites, the menus' letters and button glyphs) drew as white
rectangles for good.

## Design

A texture cache with a fixed budget and least recently used eviction, as Direct3D's managed pool or a
streaming texture cache:

- The gbh_* layer marks the textures it can convert again (`IRenderer::SetEvictable`): the game's tiles,
  sprites and remaps, and the frontend's images. The port's own textures (minimap, HUD backdrop, FPS
  overlay, CPU surface) stay resident.
- The evictable textures form a list in least recently drawn order; a draw moves its texture to the end.
- An upload that would take them over the budget (what VRAM has free at start, 5.3 MB, plus an 8 MB share of
  linear memory, `GTA2_3DS_TEXTURE_BUDGET=<KB>`) frees from the front first, wherever it is. Textures drawn in the current frame are never freed (the GPU reads them after the frame
  ends); those of earlier frames are safe, since `C3D_FrameBegin` waits for the GPU.
- Should the allocation still fail (no free block big enough), more are freed until it fits. If nothing is
  left to free, the texture draws untextured for that frame and is tried again on the next.
- An evicted texture is converted again on its next draw (`IRenderer::Evicted`, checked by `Resolve`).
- Evictable 64x64 textures with mip levels live in a slot of a 512x512 atlas page of their format (see
  Renderer in `3ds.md`). A slot is an entry of the list as a texture is, and counts for its share of its page
  (80x80 texels of the 512x512: 1.7 times what a texture of its own takes). Eviction frees the slot; an upload
  takes a free slot, else adds a page. A page goes once it has no slot left in use, so what the budget does
  not see is the free slots of partly used pages: at most 0.66 MB per format (RGB565, RGB5A1 and RGBA4 in
  practice). A slot freed in the current frame is not taken again before the next. Driving in ste for
  90 s: 8.3 MB of textures in linear memory, against 5.8 MB with a texture of its own for each.

The 16 MB: 4.7 MB taken before any texture (1.5 MB of vertices, citro3d's 1 MB command buffer, the
screens' framebuffers, audio), 0.65 MB of the port's own textures in game (1.7 MB with the CPU surface in
the frontend), the 8 MB budget, and about 2.9 MB of headroom. Peak use with the budget full: 13.5 MB.

## VRAM

VRAM is 6 MB next to the GPU, which samples it faster than linear memory (FCRAM). It holds the two render
targets (top 400x240 and bottom 320x240, RGBA8, no depth buffer: 0.66 MB); the screens' framebuffers are in
linear memory (`gfxInitDefault`). That leaves 5.3 MB, which the game's textures take first:

- Atlas pages (0.68 MB each in 16 bits) and the game's textures of their own from 1 KB to 512 KB are allocated
  in VRAM while it has room, else in linear memory as before. VRAM pages come first when a slot is picked. The
  port's own textures, the CPU surface and `LockTiled` textures, which the CPU writes in place, stay in linear
  memory.
- The CPU does not write VRAM: an upload's texels are made (on the upload worker or the main thread) in a
  staging buffer in linear memory of its own, and once written the main thread queues the GPU's DMA copy
  (`GX_TextureCopy`) into citro3d's GX queue. citro3d runs that queue at `C3D_FrameEnd`, in order, so the copies
  land before the frame's command list draws. Uploads drawn in the frame are written by its end as before
  (Renderer in `3ds.md`), so their copies make the frame; the others' are queued in the frame their job is
  collected.
- A slot's levels 0 and 1 (80 and 40 texels square with the border) are whole 8x8 tiles: each is one copy of a
  rectangle of tile rows. Levels 2 and 3 (20 and 10) share tiles with the next slots, so a VRAM page keeps them
  in a shadow in linear memory (40 KB in 16 bits), written in place, and copies it whole once per frame when a
  slot changed. A texture of its own is one copy.
- Staging buffers are freed once the GPU is done with their copies (the next `C3D_FrameBegin`); storage still
  waits for the GPU as before, so the GPU never samples a texture whose copy has not run nor one already freed.
- citro3d's queue holds 32 entries a frame (it stops the program past that). When a copy would leave fewer than
  8 for citro3d's own (the command list, the screens' transfers and clears), the queue is run and waited for in
  the frame (`waits` in `GTA2_3DS_STATS`). The game's uploads begin the frame first, so copies always join one.
  The renderer reaches the queue as citro3d's context's first member (`__C3D_Context`), checked at start.
- libctru's VRAM allocator takes its pools as all free again once bank A has no free block left (its first
  block is how it tells it has been set up): in a test, textures were then placed over the top screen's target.
  A 128-byte hole between two blocks kept at start, smaller than any texture put there, always leaves one.
- One budget for both: what VRAM has free at start plus an 8 MB share of linear memory, 13.3 MB, one least
  recently drawn list across both. `GTA2_3DS_VRAM=0` keeps everything in linear memory with the 8 MB budget.

A 21 minute drive in bil (Azahar, Old 3DS, `GTA2_TEST_CAR`, steering by turns, the upload worker on):

| | linear memory only | VRAM first |
|---|---|---|
| textures resident, most | 9.9 MB | 15.2 MB, 5.5 MB of it in VRAM |
| first eviction | 1.7 min | 13.3 min |
| evictions | 989 | 356 |
| linear memory in use, most (of 22 MB) | 22.2 MB | 22.1 MB |
| DMA copies per frame, waits for the queue | | 0.03, 9 (at the city load) |
| `tex.upload` (main thread), slowest frame | 10.2 ms | 12.9 ms |

Linear memory runs as full in both: the budget counts slots by their share of a page, and the 8 MB share ends up
as about 10 MB of pages. Once eviction starts, VRAM keeps 4.2 to 4.3 MB of textures and 1.2 MB in holes no page
fits (pages go only when their last slot does), so linear memory takes the difference. With a 0.5 MB linear share
(`GTA2_3DS_TEXTURE_BUDGET=512`, 5.8 MB in all) eviction starts after 30 s, VRAM holds 4.7 to 4.9 MB and linear
memory peaks at 16.8 MB: eviction works across both. Azahar times neither the GPU nor the DMA: the gain in GPU time
(5.7 ms per frame on an Old 3DS with linear memory only) and the copies' cost are to be measured on the console.

## Explosions and collisions

What the two scenarios of Speed in `3ds.md` convert after the city load, in 100 s (each upload counted at its
size in linear memory):

| | tank firing into traffic | car into traffic at night |
|---|---|---|
| car damage (re-made on each new dent) | 105, 2.3 MB | 100, 2.7 MB |
| peds' frames in their remaps (fleeing, burning) | 668, 1.5 MB | 507, 1.1 MB |
| the HUD's rolling digits, re-made on each step of a roll | 520, 1.3 MB | 390, 0.9 MB |
| effects (explosion, fire, smoke, sparks, blood) | 64, 0.4 MB | 59, 0.4 MB |
| tiles | 2 | 103, 1.1 MB |
| other sprites and remaps | 44, 0.4 MB | 65, 0.7 MB |

An explosion's frames came one per frame of the explosion (a 128x128 one 3.1 ms in Azahar). The effects'
205 frames (code object 112, the same in the three styles) are now converted after the city load, on the upload
worker: 1.3 MB with their mip levels, of which 0.9 MB more stays resident than without (in ste after two
minutes, 7.6 MB of textures against 6.7). They join the least recently drawn list as the load ends, so those
never drawn are the first to go once the budget is reached; those an explosion draws stay near its end. The
rest cannot be preloaded: car damage and the digits change, and the peds' frames are 25 000 textures. Their
levels are made on the upload worker (Renderer in `3ds.md`). The copies of the texels queued for the worker
take up to 1.8 MB of heap for the preload, which moves the heap's top 3 MB up (arena 23 MB against 20).

## Measurements

Resident textures by kind with the 8 MB budget, with the size without mip levels (a quarter less) in
brackets:

| | bil, 19 min | ste, 12 min |
|---|---|---|
| tiles | 2.9 MB (2.2) | 0.9 MB (0.6) |
| sprites | 1.5 MB (1.1) | 0.8 MB (0.6) |
| car remaps | 1.3 MB (1.0) | 0.4 MB (0.3) |
| ped remaps | 1.4 MB (1.1), 670 textures | 2.0 MB (1.5), 898 textures |
| car damage | 1.0 MB (0.8) | 0.9 MB (0.7) |
| fonts, digits, other remaps | 0.1 MB | 0.1 MB |
| total | 8.2 MB, evictions since 11 min | 5.0 MB, no eviction |

Eviction and frame times (frames from the city on; Azahar's 30 fps limit makes the median 33.4 ms):

| | bil, 8 MB | ste, 8 MB | bil, 3 MB | ste, 3 MB |
|---|---|---|---|---|
| evictions per minute | 34 | 0 | 173 | 42 |
| re-uploads per minute | 7 (32 KB) | 0 | 80 (469 KB) | 16 (43 KB) |
| worst eviction and re-upload in a frame | 3.1 ms | | 14.5 ms | 6.4 ms |
| mean fps | 30.1 | 30.0 | 30.1 | 30.0 |
| frames over 40 ms | 1.4 % | 0.8 % | 1.0 % | 0.8 % |
| of those, with an eviction or re-upload | 14 of 495 | 0 | 31 of 156 | 9 of 148 |

At 8 MB most evicted textures never come back (34 evictions, 7 re-uploads per minute), and a re-upload
costs what a first upload does (a 64x64 sprite with its mip levels about 0.8 ms). Even a 3 MB budget
keeps the mean at 30 fps; its worst frames reach 50 ms. The slowest frames in every run, 480 to 530 ms
each time the pause menu opens, did not come from textures (35 uploads, 9.4 ms) but from file access
(see Speed in `3ds.md`).

Dropping the mip levels of sprites and remaps (keeping them for tiles) would save about 1.3 MB of the 8 in
bil: eviction would start a few minutes later, but the resident set keeps growing with what is seen, so
the budget is reached either way.

## Other engines

citro3d's and devkitPro's examples load their textures once (`Tex3DS_TextureImport`, `C3D_TexInit`) and
free them at exit; games built for the console usually ship their textures in the GPU's own formats (ETC1
among them) and load them with the scene. Neither has to bound textures converted on demand as here. OpenLara's
3DS port puts its textures in VRAM first and in linear memory when VRAM is full, as here.

## Later: ETC1

ETC1 is 4 bits per texel (ETC1A4 8 with alpha), against 16 for RGB565 and RGBA4. In a comparison of the
game's tiles and sprites it held up very well, better than RGBA4 on car gradients. The plan: ETC1 for
tiles and cars, encoded once per city from the player's own data and cached on the SD card, with the
cache above kept as the safety net. Tiles and car remaps take 4.2 MB of the 8 in bil; in ETC1 the tiles
would take a quarter of their size, the cars (ETC1A4, with alpha) half.
