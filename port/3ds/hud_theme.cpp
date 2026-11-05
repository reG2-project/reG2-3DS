#include "hud_theme.hpp"
#include "../renderer/renderer.hpp"
#include "gtx_0x106C.hpp"
#include "map_0x370.hpp"
#include <vector>

namespace HudTheme
{
namespace
{
u32 Rgba(u32 r, u32 g, u32 b, u32 a = 255)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

Quad MakeQuad(f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1, u32 rgba)
{
    const Quad q = {x0, y0, x1, y1, u0, v0, u1, v1, {rgba, rgba, rgba, rgba}};
    return q;
}

// Plain alpha blended rectangles, drawn together.
class Shapes
{
  public:
    explicit Shapes(Sink& sink) : mSink(sink), mCount(0) {}
    void Fill(s32 x0, s32 y0, s32 x1, s32 y1, u32 rgba)
    {
        if (x0 < x1 && y0 < y1)
        {
            mQuads[mCount++] = MakeQuad((f32)x0, (f32)y0, (f32)x1, (f32)y1, 0.0f, 0.0f, 0.0f, 0.0f, rgba);
        }
    }
    // Recessed: dark top and left edges, light bottom and right ones, corner pixels left out.
    void Panel(const Rect& r)
    {
        const u32 fill = Rgba(0, 0, 0, 150), shade = Rgba(0, 0, 0, 220), light = Rgba(255, 255, 255, 40);
        Fill(r.x0 + 1, r.y0 + 1, r.x1 - 1, r.y1 - 1, fill);
        Fill(r.x0 + 1, r.y0, r.x1 - 1, r.y0 + 1, shade);
        Fill(r.x0, r.y0 + 1, r.x0 + 1, r.y1 - 1, shade);
        Fill(r.x0 + 1, r.y1 - 1, r.x1 - 1, r.y1, light);
        Fill(r.x1 - 1, r.y0 + 1, r.x1, r.y1 - 1, light);
    }
    void Finish()
    {
        mSink.DrawQuads(mQuads, mCount, 0, RS_NO_TEXTURE | RS_BLEND_ALPHA);
    }

  private:
    Sink& mSink;
    Quad mQuads[5 * kPartCount];
    s32 mCount;
};
// The backdrop: the city's most common plain pavement tile, dark, repeated at the game's scale and darker
// towards the screen's edges. Built once per city.
const s32 kScreenW = 320;
const s32 kScreenH = 240;
const s32 kMapSize = 256;
const s32 kTileSize = 64;

RTexture* sTile;
bool sHaveTile;
// The city the tile was taken from; 0 for none.
u32 sCity;
} // namespace

u32 CitySignature()
{
    if (!gMap_0x370 || !gMap_0x370->mpDmap || !gGtx_0x106C || !gGtx_0x106C->has_tiles())
    {
        return 0;
    }
    const gmp_compressed_map_32* pDmap = gMap_0x370->mpDmap;
    u32 h = 2166136261u;
    const u32 words[] = {(u32)(size_t)pDmap, (u32)pDmap->mNumBlocks, (u32)pDmap->mColumnWords, (u32)(size_t)gGtx_0x106C->mTiles};
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
    {
        h = (h ^ words[i]) * 16777619u;
    }
    return h ? h : 1;
}

namespace
{
// The lid tile of the highest block with one at a map column (0 for none), with that block's z and ground type.
u16 TopLid(s32 x, s32 y, s32* pZ, s32* pType)
{
    gmp_compressed_map_32* pDmap = gMap_0x370->mpDmap;
    const gmp_col_info* pCol = reinterpret_cast<const gmp_col_info*>(&pDmap->mpColumn[*pDmap->get_base(y, x)]);
    for (s32 i = pCol->mHeight - pCol->mOffset - 1; i >= 0; i--)
    {
        const gmp_block_info* pBlock = pDmap->get_block(pCol->mBlockd[i]);
        const u16 lid = pBlock->mLid & 0x3FF;
        if (lid)
        {
            *pZ = i + pCol->mOffset;
            *pType = pBlock->mSlopeType & 3;
            return lid;
        }
    }
    *pZ = 0;
    *pType = AIR;
    return 0;
}

bool IsPavement(s32 x, s32 y, s32 z)
{
    s32 cz, type;
    return TopLid(x, y, &cz, &type) && type == PAVEMENT && cz == z;
}

// The most common lid of pavement surrounded by pavement: a plain one, without kerbs or markings.
u16 MostCommonPavement()
{
    std::vector<u32> counts(1024, 0);
    for (s32 y = 1; y < kMapSize - 1; y++)
    {
        for (s32 x = 1; x < kMapSize - 1; x++)
        {
            s32 z, type;
            const u16 lid = TopLid(x, y, &z, &type);
            if (lid && type == PAVEMENT && IsPavement(x - 1, y, z) && IsPavement(x + 1, y, z) && IsPavement(x, y - 1, z) &&
                IsPavement(x, y + 1, z))
            {
                counts[lid]++;
            }
        }
    }
    u16 best = 0;
    for (u16 i = 1; i < 1024; i++)
    {
        best = counts[i] > counts[best] ? i : best;
    }
    return best;
}

bool BuildTile(IRenderer& renderer)
{
    const u16 tile = MostCommonPavement();
    if (!tile)
    {
        return false;
    }
    // Tiles are 64x64 bytes, four side by side in rows of 256; palette entries are 64 dwords apart.
    const u8* pTexels = gGtx_0x106C->get_tile(tile);
    const u32* pPal = reinterpret_cast<const u32*>(gGtx_0x106C->GetPalData(gGtx_0x106C->get_phys_pal(tile)));
    std::vector<u32> image(kTileSize * kTileSize);
    for (s32 y = 0; y < kTileSize; y++)
    {
        for (s32 x = 0; x < kTileSize; x++)
        {
            const u8 index = pTexels[y * 256 + x];
            const u32 c = index ? pPal[index * 64] : 0;
            image[y * kTileSize + x] = Rgba((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
        }
    }
    if (!sTile)
    {
        sTile = renderer.CreateTexture();
    }
    renderer.UploadTexture(sTile, kTileSize, kTileSize, &image[0], RMIP_NONE);
    return true;
}

// The tile's brightness at a screen point: highest in the middle, less towards the edges.
u32 Vignette(f32 x, f32 y)
{
    const f32 dx = x / kScreenW * 2.0f - 1.0f, dy = y / kScreenH * 2.0f - 1.0f;
    const f32 d = (dx * dx + dy * dy) * 0.5f;
    const u32 k = (u32)(96.0f * (1.0f - 0.6f * d));
    return Rgba(k, k, k);
}

void DrawBackdrop(IRenderer& renderer, Sink& sink)
{
    const u32 city = CitySignature();
    if (city && city != sCity)
    {
        sCity = city;
        sHaveTile = BuildTile(renderer);
    }
    if (!sHaveTile)
    {
        return;
    }
    Quad quads[20];
    s32 n = 0;
    for (s32 y = 0; y < kScreenH; y += kTileSize)
    {
        for (s32 x = 0; x < kScreenW; x += kTileSize)
        {
            const f32 y1 = (f32)(y + kTileSize < kScreenH ? y + kTileSize : kScreenH);
            Quad q = MakeQuad((f32)x, (f32)y, (f32)(x + kTileSize), y1, 0.0f, 0.0f, 1.0f, (y1 - y) / kTileSize, 0);
            q.rgba[0] = Vignette(q.x0, q.y0);
            q.rgba[1] = Vignette(q.x1, q.y0);
            q.rgba[2] = Vignette(q.x1, q.y1);
            q.rgba[3] = Vignette(q.x0, q.y1);
            quads[n++] = q;
        }
    }
    sink.DrawQuads(quads, n, sTile, 0);
}
} // namespace

void Draw(IRenderer& renderer, Sink& sink, const Rect* pPanels, u32 parts)
{
    DrawBackdrop(renderer, sink);
    Shapes shapes(sink);
    for (s32 i = 0; i < kPartCount; i++)
    {
        if (parts & (1u << i))
        {
            shapes.Panel(pPanels[i]);
        }
    }
    shapes.Finish();
}

void Shutdown(IRenderer& renderer)
{
    renderer.FreeTexture(sTile);
    sTile = 0;
    sHaveTile = false;
    sCity = 0;
}
} // namespace HudTheme
