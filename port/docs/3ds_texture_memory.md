# 3DS texture memory

The 3DS renderer (`port/3ds/c3d_renderer.cpp`) keeps every texture in linear memory, which the port fixes at
16 MB (`platform_3ds.cpp`). Figures below are from Azahar in Old 3DS mode, scripted walking and driving with
the pause menu every 50 s (`GTA2_3DS_STATS`, and per-frame logs from a measurement build).

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
- An upload that would take them over the budget (8 MB, `GTA2_3DS_TEXTURE_BUDGET=<KB>`) frees from the
  front first. Textures drawn in the current frame are never freed (the GPU reads them after the frame
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
among them) and load them with the scene. Neither has to bound textures converted on demand as here. The PICA200 can
also sample from VRAM (6 MB, of which the two render targets take 0.7 MB): a second tier there, filled by
the GPU's DMA from linear memory, would be the next step should 8 MB fall short.

## Later: ETC1

ETC1 is 4 bits per texel (ETC1A4 8 with alpha), against 16 for RGB565 and RGBA4. In a comparison of the
game's tiles and sprites it held up very well, better than RGBA4 on car gradients. The plan: ETC1 for
tiles and cars, encoded once per city from the player's own data and cached on the SD card, with the
cache above kept as the safety net. Tiles and car remaps take 4.2 MB of the 8 in bil; in ETC1 the tiles
would take a quarter of their size, the cars (ETC1A4, with alpha) half.
