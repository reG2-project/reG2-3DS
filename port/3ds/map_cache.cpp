#include "../src/map_cache.hpp"
#include "../src/prof.hpp"
#include "map_cache_mesh.hpp"
#include "Camera.hpp"
#include "MapRenderer.hpp"
#include "TextureCache.hpp"
#include "block_face.hpp"
#include "debug.hpp"
#include "gtx_0x106C.hpp"
#include "map_0x370.hpp"
#include "winmain.hpp"
#include <algorithm>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

void Log3DS_Printf(const char* format, ...) __attribute__((format(printf, 1, 2)));

// MapRenderer::Draw's projection of the layer's top (MapRenderer.cpp).
EXTERN_GLOBAL(Fix16, gScreenScaleTop);
EXTERN_GLOBAL(Fix16, gInvDepthTop);
EXTERN_GLOBAL(Fix16, gScreenScaleBottom);
EXTERN_GLOBAL(Fix16, gInvDepthBottom);

namespace PortMapCache
{
namespace
{
typedef MapCacheMesh::Vertex MeshVertex;

const s32 kChunkShift = 4;
const s32 kChunkSize = 1 << kChunkShift;
const s32 kChunks = 256 >> kChunkShift;
const s32 kLayers = 7;
// Built chunks kept, least recently drawn dropped first: 12 to 17 KB of linear memory each in ste.
const s32 kMaxBuilt = 48;
const u32 kLogEvery = 150;
const u32 kFlagLighting = 0x8000;
// Chunk builds per frame.
const s32 kBuildsPerFrame = 1;
// Frames between looks at the textures of the lids a chunk left to the game.
const u32 kPartialRebuild = 30;
// Lights past the shader's that may reach a chunk before it is left to the game whole.
const s32 kMaxFarLights = 32;
const s32 kFarSets = 16;

struct Group
{
    s32 layer;
    s32 pass; // kLidPass, or 0 for walls
    s32 first, count;
    RTexture* pTex;
    bool blend;
    // Vertices before the group's first quad of each row (its quads are in row order), and after its last.
    s32 rows[kChunkSize + 1];
};

struct TexUse
{
    RTexture* pTex;
    u32 version;
};

struct TileUse
{
    u16 logical;
    s16 physical;
};

struct MissingTexture
{
    STexture* pTex;
    bool alpha;
};

struct Chunk
{
    bool built;
    bool dirty;
    u8 layers;      // a bit per layer with cached lids or walls
    u8 wallLayers;  // a bit per layer with cached walls
    u8 drawn;       // the layers the cache draws in drawnFrame
    u8 walls;       // and of them, those it draws the walls of
    u16 farRows;    // the current layer's rows left to the game: a light past the shader's may reach them
    u32 drawnFrame;
    u32 usedFrame;
    u32 checkedFrame;
    bool valid;
    // Textures of lids left to the game, not converted yet: built again once one is.
    std::vector<MissingTexture> missing;
    u32 builtFrame;
    MeshVertex* pVerts;
    u32 bytes;
    std::vector<Group> groups; // by layer
    std::vector<TexUse> textures;
    std::vector<TileUse> tiles;
    std::vector<u16> lids; // [layer][y][x], 0 where the cache has no lid
    // A bit per cell (x) of each row: RenderBlockAt would draw more than a lid the cache has, or more than the
    // lid and walls it has (walkWalls, while it draws the walls).
    u16 walk[kLayers][kChunkSize];
    u16 walkWalls[kLayers][kChunkSize];
};

struct FarSet
{
    s32 chunk;
    s32 count;
    f32 lights[kMaxFarLights * 4];
};

bool sOn;
bool sRead;
bool sWalls;
bool sWallsRead;
bool sWallsBuilt; // sWalls when the chunks were built
s32 sLayer;
bool sFrameOn; // the cache takes lids in this frame
bool sLit;
u32 sFrame;
u32 sFreed;
u32 sColours;
s32 sBuilt;
s32 sBuildsLeft;
u32 sMeshBytes; // linear memory of the built chunks
s32 sMinCx, sMinCy, sMaxCx, sMaxCy;
FarSet sFarSets[kFarSets];
s32 sFarSetCount;
Chunk sChunks[kChunks][kChunks];
u32 sAnimated[1024 / 32];
std::vector<void*> sPendingFree;
std::vector<MapCacheMesh::Draw> sDraws;

// Per kLogEvery frames.
u32 sLogFrames, sBuilds, sDrops, sQuadsDrawn, sWallsDrawn, sDrawCalls, sMismatches, sCpuLit, sInvalid, sCells, sSkipped;
uint64_t sBuildTicks, sBuildTexTicks, sBuildMax, sDrawTicks, sBeginTicks;

void DropChunk(Chunk& c)
{
    if (c.pVerts)
    {
        // The GPU may still read it in this frame.
        sPendingFree.push_back(c.pVerts);
    }
    if (c.built)
    {
        sBuilt--;
        sDrops++;
    }
    sMeshBytes -= c.bytes;
    c.bytes = 0;
    c.pVerts = 0;
    c.built = false;
    c.dirty = false;
    c.layers = 0;
    c.wallLayers = 0;
    c.drawn = 0;
    c.walls = 0;
    c.valid = false;
    c.groups.clear();
    c.textures.clear();
    c.tiles.clear();
    c.missing.clear();
    std::vector<u16>().swap(c.lids);
    memset(c.walk, 0xFF, sizeof(c.walk));
    memset(c.walkWalls, 0xFF, sizeof(c.walkWalls));
}

void DropAll()
{
    for (s32 cy = 0; cy < kChunks; cy++)
    {
        for (s32 cx = 0; cx < kChunks; cx++)
        {
            DropChunk(sChunks[cy][cx]);
        }
    }
    memset(sAnimated, 0, sizeof(sAnimated));
}

void DropOldest()
{
    Chunk* pOld = 0;
    for (s32 cy = 0; cy < kChunks; cy++)
    {
        for (s32 cx = 0; cx < kChunks; cx++)
        {
            Chunk& c = sChunks[cy][cx];
            if (c.built && c.usedFrame != sFrame && (!pOld || c.usedFrame < pOld->usedFrame))
            {
                pOld = &c;
            }
        }
    }
    if (pOld)
    {
        DropChunk(*pOld);
    }
}

// The block goes to RenderFlatBlock (MapRenderer::RenderBlockAt), whose lid is draw_lid's plain square.
bool FlatBlock(const gmp_block_info* pBlock)
{
    const u8 slope = GET_SLOPE_BITS(pBlock->mSlopeType);
    return !(slope >= DIAGONAL_WALL_UP_LEFT && slope <= DIAGONAL_WALL_DOWN_RIGHT) &&
           !(slope >= TRIANGULAR_SIDES_DIAGONAL_UP_LEFT && slope <= TRIANGULAR_SIDES_DIAGONAL_DOWN_RIGHT) &&
           !(slope >= PARTIAL_BLOCK_LEFT && slope <= PARTIAL_CENTRE_BLOCK) && !(slope > 0 && slope < DIAGONAL_WALL_UP_LEFT);
}

// gbh_api.cpp's DrawTileImpl without FLAG_TILE_UV: the half texel inset square (lo..hi), turned and flipped by the
// flags. With 0..kTileTexMax, SetUVCorners' corners.
void TileUvs(u32 flags, f32 lo, f32 hi, f32 out[4][2])
{
    f32 uvs[4][2] = {{lo, lo}, {hi, lo}, {hi, hi}, {lo, hi}};
    f32 cur[4][2];
    memcpy(cur, uvs, sizeof(cur));
    static const int kRot90[4] = {3, 0, 1, 2};
    static const int kRot270[4] = {1, 2, 3, 0};
    const int* pRot = 0;
    u32 drawFlags = flags;
    switch (flags & 0x60)
    {
        case 0x20:
            pRot = kRot90;
            break;
        case 0x40:
            drawFlags = flags ^ 0x18;
            break;
        case 0x60:
            pRot = kRot270;
            break;
    }
    if (pRot)
    {
        for (int i = 0; i < 4; i++)
        {
            cur[i][0] = uvs[pRot[i]][0];
            cur[i][1] = uvs[pRot[i]][1];
        }
        memcpy(uvs, cur, sizeof(uvs));
    }
    if (drawFlags & 8)
    {
        static const int kFlip[4] = {1, 0, 3, 2};
        for (int i = 0; i < 4; i++)
        {
            cur[i][0] = uvs[kFlip[i]][0];
            cur[i][1] = uvs[kFlip[i]][1];
        }
        if (drawFlags & 0x10)
        {
            memcpy(uvs, cur, sizeof(uvs));
        }
    }
    if (drawFlags & 0x10)
    {
        static const int kFlip[4] = {3, 2, 1, 0};
        for (int i = 0; i < 4; i++)
        {
            cur[i][0] = uvs[kFlip[i]][0];
            cur[i][1] = uvs[kFlip[i]][1];
        }
    }
    memcpy(out, cur, sizeof(cur));
}

// A draw call per layer, pass, page, blending and premultiplication (the key); in a group, the quads in the cells'
// order.
struct QuadOrder
{
    const void* bind;
    u16 layerKey; // layer << 8 | pass << 4 | key
    u16 index;    // in sFaces
};

bool OrderBefore(const QuadOrder& a, const QuadOrder& b)
{
    if ((a.layerKey >> 4) != (b.layerKey >> 4))
    {
        return a.layerKey < b.layerKey;
    }
    if (a.bind != b.bind)
    {
        return a.bind < b.bind;
    }
    return a.layerKey != b.layerKey ? a.layerKey < b.layerKey : a.index < b.index;
}

enum Side
{
    kLeft,
    kRight,
    kTop,
    kBottom,
    kLid
};

// A face of a flat block the mesh may take: its layer, row, column, side and word; far for the flat pairs' faces,
// drawn on the cell's other side with SetUVCorners' UVs.
struct Face
{
    u8 z, ly, lx, side;
    bool far;
    u16 word;
};

// A tile (logical tile and colour key) as a build resolves it; pTex null when it stays with the game, none when it
// maps to no texture (drawn by neither).
struct Look
{
    u32 build;
    u16 key;
    bool none;
    RTexture* pTex;
    MapCacheMesh::TexturePlace place;
};

const s32 kLooks = 128;
Look sLooks[kLooks];
u32 sBuildStamp;
std::vector<Face> sFaces;
const s32 kRing = kChunkSize + 2;
// Per layer, the chunk's cells and the ring around them: kGameOther when the game draws a lid or a slope there,
// kGameWalls when it draws their walls (the walk draws both in a cell it does not leave out).
const u8 kGameOther = 1, kGameWalls = 2;
u8 sGrid[kLayers][kRing][kRing];
// A bit per side with a word, of the same cells.
u8 sSides[kLayers][kRing][kRing];
std::vector<QuadOrder> sOrder;

const Look& LookUp(Chunk& c, u16 logical, bool colourKey)
{
    const u16 key = (u16)(logical | (colourKey ? 0x400u : 0u));
    Look& l = sLooks[(logical ^ (colourKey ? 0x55u : 0u)) & (kLooks - 1)];
    if (l.build == sBuildStamp && l.key == key)
    {
        return l;
    }
    l.build = sBuildStamp;
    l.key = key;
    l.pTex = 0;
    const s16 physical = gGtx_0x106C->GetTile(logical);
    const u16 textureIdx = (u16)physical;
    l.none = !textureIdx;
    bool seen = false;
    for (size_t t = 0; t < c.tiles.size() && !seen; t++)
    {
        seen = c.tiles[t].logical == logical;
    }
    if (!seen)
    {
        const TileUse use = {logical, physical};
        c.tiles.push_back(use);
    }
    if (l.none)
    {
        return l;
    }
    STexture* pSTex = gTextureCache_15D8->GetTexture(textureIdx);
    const uint64_t tt = MapCacheMesh::Ticks();
    RTexture* pTex = pSTex ? MapCacheMesh::TileTexture(pSTex, colourKey) : 0;
    const bool placed = pTex && MapCacheMesh::Texture(pTex, &l.place);
    sBuildTexTicks += MapCacheMesh::Ticks() - tt;
    if (!placed)
    {
        seen = !pSTex;
        for (size_t t = 0; t < c.missing.size() && !seen; t++)
        {
            seen = c.missing[t].pTex == pSTex && c.missing[t].alpha == colourKey;
        }
        if (!seen)
        {
            const MissingTexture m = {pSTex, colourKey};
            c.missing.push_back(m);
        }
        return l;
    }
    l.pTex = pTex;
    seen = false;
    for (size_t t = 0; t < c.textures.size() && !seen; t++)
    {
        seen = c.textures[t].pTex == pTex;
    }
    if (!seen)
    {
        const TexUse use = {pTex, l.place.version};
        c.textures.push_back(use);
    }
    return l;
}

const s32 kLidPass = 1;

// RenderFlatBlock's faces of the block, in its order: the flat pairs' first, then the sides.
void BlockFaces(const gmp_block_info* pBlock, u8 z, u8 lx, u8 ly)
{
    const u16 words[4] = {pBlock->mLeft, pBlock->mRight, pBlock->mTop, pBlock->mBottom};
    for (s32 pair = 1; pair >= 0; pair--)
    {
        for (u8 s = kLeft; s < kLid; s++)
        {
            const u16 word = words[s], other = words[s ^ 1];
            const bool otherFlat = other && (other & block_face::flat);
            if (word && (pair ? otherFlat : (!otherFlat || (word & block_face::flat))))
            {
                const Face f = {z, ly, lx, s, pair != 0, (u16)(word | (pair ? block_face::flat : 0))};
                sFaces.push_back(f);
            }
        }
    }
}

// A wall's plane, in cells from the chunk's corner: x for the left and right sides, y for the top and bottom.
s32 Plane(const Face& f)
{
    return (f.side < kTop ? f.lx : f.ly) + ((f.side & 1) ^ (f.far ? 1 : 0));
}

// Whether the game draws what a ray to the wall may meet in its layer before the wall (a ray stays less than a block
// in a layer): in the cell the wall faces, or a lid or slope in that cell's two neighbours along the wall.
bool GameInFront(const u8 (*pCells)[kRing], const Face& f)
{
    const s32 front = Plane(f) + (f.side & 1);
    const s32 along = f.side < kTop ? f.ly : f.lx;
    for (s32 k = 0; k < 3; k++)
    {
        const u8 cell = f.side < kTop ? pCells[along + k][front] : pCells[front][along + k];
        if (cell & (k == 1 ? kGameOther | kGameWalls : kGameOther))
        {
            return true;
        }
    }
    return false;
}

// Build's step with the renderer: each face's texture.
struct Resolved
{
    RTexture* pTex;
    bool none;
    MapCacheMesh::TexturePlace place;
};
std::vector<Resolved> sResolved;

// A chunk's mesh in three steps: Scan (the map's blocks), the faces' textures, then Decide and the quads (the faces
// the game keeps, the walk's bits) from those alone, so that all but the textures could run ahead on another core.
void Scan(Chunk& c, s32 x0, s32 y0)
{
    // Map_0x370::GetBlock, a column at a time.
    const gmp_compressed_map_32* pMap = gMap_0x370->mpDmap;
    for (s32 ly = -1; ly <= kChunkSize; ly++)
    {
        u16 rows[kLayers] = {};
        for (s32 lx = -1; lx <= kChunkSize; lx++)
        {
            const s32 x = x0 + lx, y = y0 + ly;
            const bool inside = lx >= 0 && lx < kChunkSize && ly >= 0 && ly < kChunkSize;
            const gmp_col_info* pCol = x >= 0 && x <= 255 && y >= 0 && y <= 255
                                           ? reinterpret_cast<const gmp_col_info*>(&pMap->mpColumn[pMap->mBase[y][x]])
                                           : 0;
            for (s32 z = 0; z < kLayers; z++)
            {
                u8& cell = sGrid[z][ly + 1][lx + 1];
                cell = 0;
                sSides[z][ly + 1][lx + 1] = 0;
                if (!pCol || z >= pCol->mHeight || z < pCol->mOffset)
                {
                    continue;
                }
                const gmp_block_info* pBlock = &pMap->mBlock[pCol->mBlockd[z - pCol->mOffset]];
                sSides[z][ly + 1][lx + 1] = (u8)((pBlock->mLeft ? 1 : 0) | (pBlock->mRight ? 2 : 0) | (pBlock->mTop ? 4 : 0) |
                                                 (pBlock->mBottom ? 8 : 0));
                const bool flat = FlatBlock(pBlock);
                const u16 lid = pBlock->mLid;
                const bool sides = pBlock->mLeft || pBlock->mRight || pBlock->mTop || pBlock->mBottom;
                if (!inside)
                {
                    // Another chunk's, drawn by the game in some frames.
                    cell = (lid || !flat ? kGameOther : 0) | (sides ? kGameWalls : 0);
                    continue;
                }
                const u16 logical = lid & block_face::tile_idx_mask;
                const bool take = lid && flat && !(sAnimated[logical >> 5] & (1u << (logical & 31)));
                if (take)
                {
                    const Face f = {(u8)z, (u8)ly, (u8)lx, kLid, false, lid};
                    sFaces.push_back(f);
                }
                rows[z] |= (u16)(flat && !sides && (!lid || take) ? 0 : 1u << lx);
                if (!flat || (lid && !take))
                {
                    cell = kGameOther;
                }
                else if (sWallsBuilt)
                {
                    BlockFaces(pBlock, (u8)z, (u8)lx, (u8)ly);
                }
            }
        }
        for (s32 z = 0; z < kLayers && ly >= 0 && ly < kChunkSize; z++)
        {
            c.walk[z][ly] = rows[z];
        }
    }
}

// The cells whose walls the mesh takes: those where the game draws nothing, and nothing in front of their walls.
void Decide(Chunk& c)
{
    for (size_t i = 0; i < sFaces.size(); i++)
    {
        const Face& f = sFaces[i];
        const u16 logical = f.word & block_face::tile_idx_mask;
        const bool animated = (sAnimated[logical >> 5] >> (logical & 31)) & 1;
        // A flat pair's face on the plane of the next cell's side the same way round: the walk's order shows there.
        const s32 behind = f.far ? Plane(f) + 1 - (f.side & 1) : 0;
        const s32 along = (f.side < kTop ? f.ly : f.lx) + 1;
        const bool coplanar = f.far && (((f.side < kTop ? sSides[f.z][along][behind] : sSides[f.z][behind][along]) >> f.side) & 1);
        if ((!sResolved[i].pTex && !sResolved[i].none) || (f.side != kLid && animated) || coplanar)
        {
            sGrid[f.z][f.ly + 1][f.lx + 1] |= f.side == kLid ? kGameOther : kGameWalls;
            c.walk[f.z][f.ly] |= (u16)(f.side == kLid ? 1u << f.lx : 0);
        }
    }
    for (bool changed = true; changed;)
    {
        changed = false;
        for (size_t i = 0; i < sFaces.size(); i++)
        {
            const Face& f = sFaces[i];
            u8& cell = sGrid[f.z][f.ly + 1][f.lx + 1];
            if (f.side != kLid && !(cell & (kGameOther | kGameWalls)) && GameInFront(sGrid[f.z], f))
            {
                cell |= kGameWalls;
                changed = true;
            }
        }
    }
    for (s32 z = 0; z < kLayers; z++)
    {
        for (s32 ly = 0; ly < kChunkSize; ly++)
        {
            u16 row = 0;
            for (s32 lx = 0; lx < kChunkSize; lx++)
            {
                row |= (u16)(sGrid[z][ly + 1][lx + 1] ? 1u << lx : 0);
            }
            c.walkWalls[z][ly] = row;
        }
    }
}

void Build(Chunk& c, s32 cx, s32 cy)
{
    PORT_PROF(kDrawMapCacheBuild);
    PORT_PROF_COUNT(kMcBuilds, 1);
    const uint64_t t0 = MapCacheMesh::Ticks();
    if (c.built)
    {
        DropChunk(c);
    }
    while (sBuilt >= kMaxBuilt)
    {
        const s32 before = sBuilt;
        DropOldest();
        if (sBuilt == before)
        {
            break;
        }
    }
    c.lids.assign(kLayers * kChunkSize * kChunkSize, 0);
    c.missing.clear();
    c.builtFrame = sFrame;
    sFaces.clear();
    sBuildStamp++;
    PORT_PROF_SECTION(kDrawMapCacheBuildBlocks);
    Scan(c, cx << kChunkShift, cy << kChunkShift);
    PORT_PROF_SECTION(kDrawMapCacheBuildQuads);
    sResolved.resize(sFaces.size());
    for (size_t i = 0; i < sFaces.size(); i++)
    {
        const Look& look = LookUp(c, sFaces[i].word & block_face::tile_idx_mask, (sFaces[i].word & block_face::flat) != 0);
        sResolved[i].pTex = look.pTex;
        sResolved[i].none = look.none;
        sResolved[i].place = look.place;
    }
    Decide(c);
    static f32 sUvs[kLid + 1][8][2][4][2];
    static bool sUvsMade;
    if (!sUvsMade)
    {
        const s32* const kFlags[kLid + 1] = {MapRenderer::kLeftSideFlags, MapRenderer::kRightSideFlags, MapRenderer::kTopSideFlags,
                                             MapRenderer::kBottomSideFlags, MapRenderer::kLidFlags};
        for (s32 s = 0; s <= kLid; s++)
        {
            for (s32 i = 0; i < 8; i++)
            {
                TileUvs(kFlags[s][i], 0.5f, 63.499901f, sUvs[s][i][0]);
                TileUvs(kFlags[s][i], 0.0f, 1048575.0f / 16384.0f, sUvs[s][i][1]);
            }
        }
        sUvsMade = true;
    }
    for (size_t n = 0; n < sFaces.size(); n++)
    {
        const Face& f = sFaces[n];
        const bool lid = f.side == kLid;
        if (!sResolved[n].pTex || (!lid && sGrid[f.z][f.ly + 1][f.lx + 1]))
        {
            continue;
        }
        const u32 key = ((f.word & block_face::flat) ? 1u : 0u) | (sResolved[n].place.premultiplied ? 2u : 0u);
        const QuadOrder o = {sResolved[n].place.bind, (u16)((f.z << 8) | ((lid ? kLidPass : 0) << 4) | key), (u16)n};
        sOrder.push_back(o);
        if (lid)
        {
            c.lids[(f.z * kChunkSize + f.ly) * kChunkSize + f.lx] = f.word;
        }
        c.layers |= (u8)(1u << f.z);
        c.wallLayers |= (u8)(lid ? 0 : 1u << f.z);
    }
    PORT_PROF_SECTION(kDrawMapCacheBuildSort);
    std::sort(sOrder.begin(), sOrder.end(), OrderBefore);
    PORT_PROF_SECTION(kDrawMapCacheBuildMesh);
    if (!sOrder.empty())
    {
        const size_t bytes = sOrder.size() * 6 * sizeof(MeshVertex);
        c.pVerts = MapCacheMesh::Alloc(sOrder.size() * 6);
        if (!c.pVerts)
        {
            c.layers = 0;
            c.wallLayers = 0;
            std::fill(c.lids.begin(), c.lids.end(), 0);
            memset(c.walk, 0xFF, sizeof(c.walk));
            memset(c.walkWalls, 0xFF, sizeof(c.walkWalls));
        }
        else
        {
            const u8 shades[kLid] = {gpMapRenderer->mColourT1, gpMapRenderer->mRightColour, gpMapRenderer->mColourT3,
                                     gpMapRenderer->mColourT2};
            // gTileVerts' corners: along the wall (or x for a lid), y for a lid, and on the layer's bottom.
            static const u8 kAlong[kLid + 1][4] = {{0, 0, 1, 1}, {0, 0, 1, 1}, {0, 1, 1, 0}, {0, 1, 1, 0}, {0, 1, 1, 0}};
            static const u8 kLow[kLid + 1][4] = {{1, 0, 0, 1}, {0, 1, 1, 0}, {1, 1, 0, 0}, {0, 0, 1, 1}, {0, 0, 0, 0}};
            static const s32 kTri[6] = {0, 1, 2, 0, 2, 3};
            for (size_t i = 0; i < sOrder.size(); i++)
            {
                const QuadOrder& o = sOrder[i];
                const Face& f = sFaces[o.index];
                const MapCacheMesh::TexturePlace& place = sResolved[o.index].place;
                const bool lid = f.side == kLid;
                const f32(*uv)[2] = sUvs[f.side][f.word >> 13][f.far ? 1 : 0];
                const u8 shade = lid ? gpMapRenderer->GetColour((f.word >> 10) & 3) : shades[f.side];
                const s32 plane = lid ? 0 : Plane(f);
                MeshVertex corners[4];
                for (s32 k = 0; k < 4; k++)
                {
                    MeshVertex& v = corners[k];
                    v.x = (u8)(lid ? f.lx + kAlong[kLid][k] : f.side < kTop ? plane : f.lx + kAlong[f.side][k]);
                    v.y = (u8)(lid ? f.ly + (k >> 1) : f.side < kTop ? f.ly + kAlong[f.side][k] : plane);
                    v.shade = shade;
                    v.bottom = kLow[f.side][k];
                    v.u = uv[k][0] * (1.0f / 64.0f) * place.su + place.ou;
                    v.v = place.ov - uv[k][1] * (1.0f / 64.0f) * place.sv;
                }
                for (s32 k = 0; k < 6; k++)
                {
                    c.pVerts[i * 6 + k] = corners[kTri[k]];
                }
                if (i == 0 || o.layerKey != sOrder[i - 1].layerKey || o.bind != sOrder[i - 1].bind)
                {
                    Group g = {f.z, lid ? kLidPass : 0, (s32)(i * 6), 0, sResolved[o.index].pTex, (f.word & block_face::flat) != 0, {}};
                    c.groups.push_back(g);
                }
                // Per row first, summed into the vertices before each row below.
                Group& g = c.groups.back();
                g.rows[f.ly + 1] += 6;
                g.count += 6;
            }
            for (size_t i = 0; i < c.groups.size(); i++)
            {
                Group& g = c.groups[i];
                for (s32 r = 1; r <= kChunkSize; r++)
                {
                    g.rows[r] += g.rows[r - 1];
                }
            }
            MapCacheMesh::Flush(c.pVerts, bytes);
            c.bytes = (u32)bytes;
            sMeshBytes += c.bytes;
        }
    }
    sOrder.clear();
    PORT_PROF_END();
    c.built = true;
    c.dirty = false;
    c.valid = true;
    c.checkedFrame = sFrame;
    sBuilt++;
    sBuilds++;
    const uint64_t took = MapCacheMesh::Ticks() - t0;
    sBuildTicks += took;
    sBuildMax = took > sBuildMax ? took : sBuildMax;
}

struct View
{
    f32 scale, rhw, centreX, centreY;
    s32 rowMin, rowMax, colMin, colMax;
};

// The rows and columns the screen shows at the layer's top (or bottom), and one more around them; false when the
// top is not in front of the camera.
bool TopView(View* pView, bool bottom = false)
{
    pView->scale = (bottom ? gScreenScaleBottom : gScreenScaleTop).ToFloat();
    pView->rhw = gInvDepthTop.ToFloat();
    if (gInvDepthTop.mValue <= 0 || gInvDepthBottom.mValue <= 0 || pView->scale <= 0.0f)
    {
        return false;
    }
    const f32 scale = pView->scale;
    pView->centreX = (f32)static_cast<u32>(gViewCamera->mScreenPxCenterX);
    pView->centreY = (f32)static_cast<u32>(gViewCamera->mScreenPxCenterY);
    const f32 camX = gViewCamera->mCurPos.mX.ToFloat(), camY = gViewCamera->mCurPos.mY.ToFloat();
    pView->rowMin = (s32)floorf(camY - pView->centreY / scale) - 1;
    pView->rowMax = (s32)floorf(camY + pView->centreY / scale) + 1;
    pView->colMin = (s32)floorf(camX - pView->centreX / scale) - 1;
    pView->colMax = (s32)floorf(camX + pView->centreX / scale) + 1;
    return true;
}

bool InView(const View& v, s32 wx, s32 wy)
{
    return !(wx > v.colMax || wx + kChunkSize <= v.colMin || wy > v.rowMax || wy + kChunkSize <= v.rowMin);
}

// The chunk's rows between the heights lo and hi that a light past the shader's may reach, which gbh_api.cpp lights
// on the CPU (a tile's box as near as NearLights finds it, with a margin): all of them when the shader cannot light it.
u16 FarRows(s32 cx, s32 cy, f32 lo, f32 hi)
{
    // The chunk's lights for all the lids' heights, looked up once a frame.
    const s32 chunk = cy * kChunks + cx;
    s32 k = 0;
    while (k < sFarSetCount && sFarSets[k].chunk != chunk)
    {
        k++;
    }
    const f32 x0 = (f32)(cx << kChunkShift), y0 = (f32)(cy << kChunkShift);
    FarSet& set = sFarSets[k < kFarSets ? k : kFarSets - 1];
    if (k >= sFarSetCount)
    {
        sFarSetCount += k < kFarSets ? 1 : 0;
        set.chunk = chunk;
        set.count = MapCacheMesh::FarLights(x0, y0, 1.0f, x0 + kChunkSize, y0 + kChunkSize, (f32)kLayers, set.lights, kMaxFarLights);
    }
    if (set.count < 0)
    {
        return 0xFFFF;
    }
    u16 rows = 0;
    for (s32 i = 0; i < set.count; i++)
    {
        const f32* pLight = &set.lights[i * 4];
        const f32 dz = (pLight[2] < lo ? lo - pLight[2] : pLight[2] > hi ? pLight[2] - hi : 0.0f) - 0.01f;
        const f32 left = pLight[3] - (dz > 0.0f ? dz * dz : 0.0f);
        if (left < 0.0f)
        {
            continue;
        }
        const f32 reach = sqrtf(left) + 0.05f;
        s32 r0 = (s32)floorf(pLight[1] - reach - y0);
        s32 r1 = (s32)floorf(pLight[1] + reach - y0);
        r0 = r0 < 0 ? 0 : r0;
        r1 = r1 >= kChunkSize ? kChunkSize - 1 : r1;
        if (r0 <= r1)
        {
            rows |= (u16)(((2u << r1) - 1) & ~((1u << r0) - 1));
        }
    }
    return rows;
}

// A texture of a lid left to the game has been converted since.
bool Converted(Chunk& c)
{
    for (size_t i = 0; i < c.missing.size(); i++)
    {
        if (MapCacheMesh::TileTexture(c.missing[i].pTex, c.missing[i].alpha))
        {
            return true;
        }
    }
    c.builtFrame = sFrame;
    return false;
}

// The tiles still map to the textures the mesh was built with, and those still have their texels.
bool Check(Chunk& c)
{
    bool ok = true;
    for (size_t i = 0; i < c.tiles.size(); i++)
    {
        const TileUse& t = c.tiles[i];
        if (gGtx_0x106C->GetTile(t.logical) != t.physical)
        {
            // Animated (TileAnimManager) or remapped: left to the game's draws from now on.
            sAnimated[t.logical >> 5] |= 1u << (t.logical & 31);
            ok = false;
        }
    }
    for (size_t i = 0; i < c.textures.size(); i++)
    {
        MapCacheMesh::TexturePlace place;
        if (!MapCacheMesh::Texture(c.textures[i].pTex, &place) || place.version != c.textures[i].version)
        {
            ok = false;
        }
    }
    return ok;
}

void FreePending()
{
    for (size_t i = 0; i < sPendingFree.size(); i++)
    {
        MapCacheMesh::Free(sPendingFree[i]);
    }
    sPendingFree.clear();
}

void BeginFrame()
{
    sFrameOn = false;
    if (bShow_hidden_faces || bSkip_lid || bSkip_tiles || !gMap_0x370 || !gGtx_0x106C || !gTextureCache_15D8)
    {
        return;
    }
    if (!MapCacheMesh::BeginFrame(&sFrame))
    {
        return;
    }
    // C3D_FrameBegin has waited for the GPU: the last frame's meshes are no longer read.
    FreePending();
    const u32 colours = gpMapRenderer->mColourT1 | (gpMapRenderer->mColourT2 << 8) | (gpMapRenderer->mColourT3 << 16) |
                        (gpMapRenderer->mRightColour << 24);
    if (MapCacheMesh::FreedTextures() != sFreed || colours != sColours || sWallsBuilt != WallsEnabled())
    {
        DropAll();
        sFreed = MapCacheMesh::FreedTextures();
        sColours = colours;
        sWallsBuilt = WallsEnabled();
    }
    sLit = (gLightingDrawFlag & kFlagLighting) && MapCacheMesh::Lit();
    sBuildsLeft = kBuildsPerFrame;
    sFarSetCount = 0;
    sFrameOn = true;
    sLogFrames++;
}

// A system call per read, every layer: left out without the profiler.
uint64_t LayerTicks()
{
    return PortProf::gOn ? MapCacheMesh::Ticks() : 0;
}

void Log()
{
    if (sLogFrames < kLogEvery)
    {
        return;
    }
    const f32 n = (f32)sLogFrames;
    uint64_t parts[6];
    MapCacheMesh::TakeDrawTicks(parts);
    f32 ms[6];
    for (int i = 0; i < 6; i++)
    {
        ms[i] = MapCacheMesh::TicksToMs(parts[i]) / n;
    }
    // Per frame but for the builds.
    Log3DS_Printf("MAPCACHE frames %u built %d builds %u drops %u quads %.1f walls %.1f draws %.1f mismatch %u cpulit %u invalid %u "
                  "build.ms %.2f (textures %.2f, slowest %.2f) draw.ms %.3f (flush %.3f switch %.3f draws %.3f "
                  "[first DrawArrays %.3f, others %.3f] back %.3f) begin.ms %.3f mesh.kb %u linear.free.kb %u cells %.1f skipped %.1f\n",
                  (unsigned)sLogFrames, (int)sBuilt, (unsigned)sBuilds, (unsigned)sDrops, sQuadsDrawn / n, sWallsDrawn / n, sDrawCalls / n,
                  (unsigned)sMismatches, (unsigned)sCpuLit, (unsigned)sInvalid, MapCacheMesh::TicksToMs(sBuildTicks),
                  MapCacheMesh::TicksToMs(sBuildTexTicks), MapCacheMesh::TicksToMs(sBuildMax),
                  MapCacheMesh::TicksToMs(sDrawTicks) / n, ms[0], ms[1], ms[2], ms[4], ms[5], ms[3],
                  MapCacheMesh::TicksToMs(sBeginTicks) / n, (unsigned)(sMeshBytes / 1024), (unsigned)MapCacheMesh::LinearFreeKb(), sCells / n, sSkipped / n);
    sLogFrames = sBuilds = sDrops = sQuadsDrawn = sWallsDrawn = sDrawCalls = sMismatches = sCpuLit = sInvalid = sCells = sSkipped = 0;
    sBuildTicks = sBuildTexTicks = sBuildMax = sDrawTicks = sBeginTicks = 0;
}
} // namespace

bool Enabled()
{
    if (!sRead)
    {
        const char* pEnv = getenv("GTA2_3DS_MAPCACHE");
        sOn = !pEnv || atoi(pEnv) != 0;
        sRead = true;
    }
    return sOn;
}

void SetEnabled(bool on)
{
    sOn = on;
    sRead = true;
}

bool WallsEnabled()
{
    if (!sWallsRead)
    {
        const char* pEnv = getenv("GTA2_3DS_MAPCACHE_WALLS");
        sWalls = !pEnv || atoi(pEnv) != 0;
        sWallsRead = true;
    }
    return sWalls;
}

void SetWalls(bool on)
{
    sWalls = on;
    sWallsRead = true;
}

void BeginLayer(s32 z, s32 minX, s32 minY, s32 maxX, s32 maxY)
{
    if (!Enabled())
    {
        // The meshes stay: Invalidate, Check and TakeLid's mismatch catch what changes while off.
        sFrameOn = false;
        return;
    }
    PORT_PROF(kDrawMapCache);
    if (z == 0)
    {
        Log();
        BeginFrame();
    }
    if (!sFrameOn)
    {
        return;
    }
    sLayer = z;
    const uint64_t t0 = LayerTicks();
    minX = minX < 0 ? 0 : minX;
    minY = minY < 0 ? 0 : minY;
    maxX = maxX > 255 ? 255 : maxX;
    maxY = maxY > 255 ? 255 : maxY;
    sMinCx = minX >> kChunkShift;
    sMinCy = minY >> kChunkShift;
    sMaxCx = maxX >> kChunkShift;
    sMaxCy = maxY >> kChunkShift;
    View view, wallView;
    const bool shown = TopView(&view) && TopView(&wallView, true);
    // Build's walls assume a ray to the screen stays less than a block in a layer.
    const bool wallsFit = shown && view.centreX * view.rhw < view.scale && view.centreY * view.rhw < view.scale;
    for (s32 cy = sMinCy; cy <= sMaxCy; cy++)
    {
        for (s32 cx = sMinCx; cx <= sMaxCx; cx++)
        {
            Chunk& c = sChunks[cy][cx];
            const bool stale = !c.built || c.dirty;
            bool converted = false;
            if (!stale && sFrame - c.builtFrame >= kPartialRebuild)
            {
                PORT_PROF(kDrawMapCacheCheck);
                converted = Converted(c);
            }
            if (stale || converted)
            {
                // The rest wait for the next frames, drawn by the game meanwhile.
                if (sBuildsLeft > 0)
                {
                    sBuildsLeft--;
                    Build(c, cx, cy);
                }
                else if (stale)
                {
                    continue;
                }
            }
            if (c.checkedFrame != sFrame)
            {
                c.checkedFrame = sFrame;
                PORT_PROF_COUNT(kMcChecks, 1);
                {
                    PORT_PROF(kDrawMapCacheCheck);
                    c.valid = Check(c);
                }
                if (!c.valid && sBuildsLeft > 0)
                {
                    sInvalid++;
                    sBuildsLeft--;
                    Build(c, cx, cy);
                    c.valid = Check(c);
                }
            }
            c.usedFrame = sFrame;
            c.farRows = 0;
            if (c.drawnFrame != sFrame)
            {
                c.drawnFrame = sFrame;
                c.drawn = 0;
                c.walls = 0;
            }
            if (!c.valid || !(c.layers & (1u << z)) || !shown)
            {
                continue;
            }
            const f32 top = (f32)(z + 1);
            u16 farRows = 0;
            // Off screen, the lids are taken from the walk and not drawn.
            if (sLit && InView(view, cx << kChunkShift, cy << kChunkShift))
            {
                PORT_PROF(kDrawMapCacheLight);
                farRows = FarRows(cx, cy, top, top);
            }
            if (farRows == 0xFFFF)
            {
                sCpuLit++;
                PORT_PROF_COUNT(kMcCpuLit, 1);
                continue;
            }
            for (u16 rows = farRows; rows; rows &= rows - 1)
            {
                PORT_PROF_COUNT(kMcFarRows, 1);
            }
            c.farRows = farRows;
            c.drawn |= (u8)(1u << z);
            PORT_PROF_COUNT(kMcDrawn, 1);
            // The walls only where no light past the shader's reaches the chunk's blocks of the layer.
            if (wallsFit && (c.wallLayers >> z) & 1)
            {
                u16 wallRows = farRows;
                if (sLit && !wallRows && InView(wallView, cx << kChunkShift, cy << kChunkShift))
                {
                    PORT_PROF(kDrawMapCacheLight);
                    wallRows = FarRows(cx, cy, top - 1.0f, top);
                }
                c.walls |= (u8)(wallRows ? 0 : 1u << z);
            }
        }
    }
    sBeginTicks += LayerTicks() - t0;
}

bool TakeLid(u16 lid)
{
    if (!sFrameOn)
    {
        return false;
    }
    const s32 x = (gXCoord + gViewCamera->mCurPos.mX).ToInt();
    const s32 y = (gYCoord + gViewCamera->mCurPos.mY).ToInt();
    const s32 z = (s32)gZCoord;
    if (x < 0 || x > 255 || y < 0 || y > 255 || z < 0 || z >= kLayers)
    {
        return false;
    }
    Chunk& c = sChunks[y >> kChunkShift][x >> kChunkShift];
    if (c.drawnFrame != sFrame || !(c.drawn & (1u << z)))
    {
        return false;
    }
    const s32 ly = y & (kChunkSize - 1);
    const u16 cached = c.lids[(z * kChunkSize + ly) * kChunkSize + (x & (kChunkSize - 1))];
    if (cached == lid && !((c.farRows >> ly) & 1))
    {
        PORT_PROF_COUNT(kMcLids, 1);
        return true;
    }
    if (cached && cached != lid)
    {
        // Changed without Invalidate: drawn by the game now, built again in the next frame.
        sMismatches++;
        c.dirty = true;
    }
    return false;
}

bool SkipCell(s32 x, s32 y)
{
    if (!sFrameOn)
    {
        return false;
    }
    sCells++;
    if (x < 0 || x > 255 || y < 0 || y > 255)
    {
        return false;
    }
    const Chunk& c = sChunks[y >> kChunkShift][x >> kChunkShift];
    const s32 lx = x & (kChunkSize - 1), ly = y & (kChunkSize - 1);
    const bool walls = c.drawnFrame == sFrame && (c.walls >> sLayer) & 1;
    if (!c.built || c.dirty || c.checkedFrame != sFrame || ((walls ? c.walkWalls : c.walk)[sLayer][ly] >> lx) & 1)
    {
        return false;
    }
    // A cached lid is drawn by EndLayer only in the chunks it draws this frame.
    const bool skip = !c.lids[(sLayer * kChunkSize + ly) * kChunkSize + lx] || ((c.drawn >> sLayer) & 1 && !((c.farRows >> ly) & 1));
    sSkipped += skip ? 1 : 0;
    PORT_PROF_COUNT(kMcSkipped, skip ? 1 : 0);
    return skip;
}

void EndLayer(s32 z)
{
    View view, wallView;
    if (!sFrameOn || !TopView(&view) || !TopView(&wallView, true))
    {
        return;
    }
    PORT_PROF(kDrawMapCache);
    const uint64_t t0 = LayerTicks();
    sDraws.clear();
    const f32 scale = view.scale, rhw = view.rhw, centreX = view.centreX, centreY = view.centreY;
    // A 1:1 lid samples its first level, as C3DRenderer::DrawTriangles' OneToOne would find.
    const bool noMip = fabsf(62.9999f / scale - 1.0f) < 0.01f;
    for (s32 pass = 0; pass <= kLidPass; pass++)
    {
        const bool walls = pass != kLidPass;
        const s32 rowMin = (walls ? wallView : view).rowMin, rowMax = (walls ? wallView : view).rowMax;
        for (s32 cy = sMinCy; cy <= sMaxCy; cy++)
        {
            for (s32 cx = sMinCx; cx <= sMaxCx; cx++)
            {
                Chunk& c = sChunks[cy][cx];
                if (c.drawnFrame != sFrame || !((walls ? c.walls : c.drawn) & (1u << z)))
                {
                    continue;
                }
                const s32 wx = cx << kChunkShift, wy = cy << kChunkShift;
                if (!InView(walls ? wallView : view, wx, wy))
                {
                    continue;
                }
                const f32 ox = (Fix16(wx) - gViewCamera->mCurPos.mX).ToFloat();
                const f32 oy = (Fix16(wy) - gViewCamera->mCurPos.mY).ToFloat();
                for (size_t i = 0; i < c.groups.size(); i++)
                {
                    const Group& g = c.groups[i];
                    if (g.layer != z || g.pass != pass)
                    {
                        continue;
                    }
                    const s32 rowFirst = rowMin - wy < 0 ? 0 : rowMin - wy;
                    const s32 rowLast = rowMax - wy >= kChunkSize ? kChunkSize - 1 : rowMax - wy;
                    // A draw per run of rows the game leaves to the mesh.
                    for (s32 r = rowFirst; r <= rowLast;)
                    {
                        while (r <= rowLast && (c.farRows >> r) & 1)
                        {
                            r++;
                        }
                        const s32 r0 = r;
                        while (r <= rowLast && !((c.farRows >> r) & 1))
                        {
                            r++;
                        }
                        if (g.rows[r] == g.rows[r0])
                        {
                            continue;
                        }
                        MapCacheMesh::Draw d;
                        d.pVerts = c.pVerts;
                        d.first = g.first + g.rows[r0];
                        d.count = g.rows[r] - g.rows[r0];
                        d.pTex = g.pTex;
                        d.blend = g.blend;
                        d.noMip = noMip && !walls;
                        d.walls = walls;
                        d.pass = pass;
                        d.scale = scale;
                        d.x0 = ox * scale + centreX;
                        d.y0 = oy * scale + centreY;
                        d.rhw = rhw;
                        d.rhwBottom = gInvDepthBottom.ToFloat();
                        d.centreX = centreX;
                        d.centreY = centreY;
                        d.wx = (f32)wx;
                        d.wy = (f32)wy;
                        d.wz = (f32)(z + 1);
                        sDraws.push_back(d);
                        if (walls)
                        {
                            sWallsDrawn += (u32)d.count / 6;
                            PORT_PROF_COUNT(kMcWalls, (u32)d.count / 6);
                        }
                        else
                        {
                            sQuadsDrawn += (u32)d.count / 6;
                            PORT_PROF_COUNT(kMcQuads, (u32)d.count / 6);
                        }
                    }
                }
            }
        }
    }
    if (!sDraws.empty())
    {
        PORT_PROF(kDrawMapCacheDraw);
        MapCacheMesh::DrawAll(&sDraws[0], (s32)sDraws.size(), sLit);
        sDrawCalls += (u32)sDraws.size();
    }
    sDrawTicks += LayerTicks() - t0;
}

void Invalidate(s32 x, s32 y)
{
    if (x < 0 || x > 255 || y < 0 || y > 255)
    {
        return;
    }
    Chunk& c = sChunks[y >> kChunkShift][x >> kChunkShift];
    if (c.built)
    {
        c.dirty = true;
    }
}
} // namespace PortMapCache
