// The renderer's side of the map cache (map_cache.cpp), in types both the game's and the renderer's headers take.
#pragma once

#include <stddef.h>
#include <stdint.h>

struct RTexture;
struct STexture;

namespace MapCacheMesh
{
// A mesh vertex: local (x, y) on a plane of constant depth, grey shaded, lit as gbh_api.cpp lights the game's tiles.
struct Vertex
{
    uint8_t x, y;
    uint8_t shade; // the colour (shade, shade, shade, 255)
    uint8_t pad;
    float u, v; // in the texture's page
};

struct Draw
{
    const Vertex* pVerts;
    int32_t first, count;
    RTexture* pTex;
    bool blend; // RS_BLEND_ALPHA, else opaque
    bool noMip;
    // Logical pixels = local * scale + (x0, y0), at 1 / rhw; local (0, 0) is at world (wx, wy, wz).
    float scale, x0, y0, rhw;
    float wx, wy, wz;
};

struct TexturePlace
{
    float su, sv, ou, ov; // page u = u * su + ou, page v = ov - v * sv, as C3DRenderer::DrawTriangles maps them
    const void* bind;     // draws with the same bind, blending and premultiplied can share a draw call
    bool premultiplied;
    uint32_t version; // changes when the texels move
};

// The frame begun if it had not; false when the renderer cannot take meshes (none, or the view turned).
bool BeginFrame(uint32_t* pFrame);
// gbh_api.cpp's texture for a tile, as gbh_DrawTile would resolve it.
RTexture* TileTexture(STexture* pTexture, bool alpha);
// False when the texture has no storage. Marks it drawn in this frame, so it is not evicted before the meshes.
bool Texture(RTexture* pTex, TexturePlace* pPlace);
// Whether draws with FLAG_LIGHTING are lit now (the shader's lights uploaded if so).
bool Lit();
// Whether the shader cannot light the box as gbh_api.cpp would: a light past its own reaches it.
bool NeedsCpuLight(float x0, float y0, float z0, float x1, float y1, float z1);
// In order after the gbh_* draws so far.
void DrawAll(const Draw* pDraws, int32_t count, bool lit);
// Counts the textures freed: one a mesh points at may be gone.
uint32_t FreedTextures();

// Linear memory for meshes, flushed for the GPU once written.
Vertex* Alloc(size_t count);
void Free(void* p);
void Flush(const void* p, size_t bytes);
uint32_t LinearFreeKb();
uint64_t Ticks();
// C3DRenderer::TakeMeshTicks.
void TakeDrawTicks(uint64_t* pTicks);
float TicksToMs(uint64_t ticks);
} // namespace MapCacheMesh
