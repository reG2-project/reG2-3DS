#include "minimap.hpp"
#include "c3d_renderer.hpp"
#include "minimap_assets.h"
#include "minimap_game.hpp"
#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace Minimap
{
namespace
{
using MinimapGame::kMapSize;
using MinimapGame::kMaxMarks;
// The map texture has 2x2 texels per block (RGB565, 512 KB), drawn at 2x2 pixels per texel.
const s32 kMapTexels = kMapSize * 2;
const s32 kPxPerBlock = 4;
// The tube frame (frame_1x.png of port/docs/minimap/assets/frames/E_tube_frame): corner caps, a tube rib period
// to repeat, the N tab, and the tube's width over the map's edge.
const s32 kCap = 14;
const s32 kRibX = 14;
const s32 kRib = 7;
const s32 kTabX = 44;
const s32 kTabW = 15;
const s32 kTube = 13;
// The atlas: the frame at (0, 0), the player arrow right of it.
const s32 kAtlasSize = 128;
const s32 kArrowX = 104;
// The game's sprites for the markers, read from the city's style.
const s32 kSpritesW = 128;
const s32 kSpritesH = 64;
// Markers stay this far inside the map; further out they go on its edge.
const s32 kEdgeMargin = 5;

struct SpriteRect
{
    s16 x, y, w, h;
};

RTexture* sMap;
RTexture* sTint;
RTexture* sAtlas;
RTexture* sSprites;
// At half size, as the bottom screen draws the HUD, but the pips at full size to be seen on the map.
SpriteRect sArrows[MinimapGame::kMaxArrow + 1];
SpriteRect sIcons[MinimapGame::kMaxArrow + 1];
SpriteRect sPips[MinimapGame::kPipCount];
SpriteRect sSmallPips[MinimapGame::kPipCount];
u32 sCity;
bool sMapOk;
u32 sTintSig;
u32 sNone; // the ground where no block has a lid, also around the city
MinimapGame::View sView;

u32 Rgba(u32 r, u32 g, u32 b, u32 a = 255)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

// The ground, with dark rims where a block stands over its neighbour and a shadow towards the bottom right, as in
// the minimap study (port/docs/minimap).
bool BuildMap(C3DRenderer& renderer)
{
    std::vector<u8> heights(kMapSize * kMapSize);
    std::vector<u16> colours(kMapSize * kMapSize);
    const u16 none = MinimapGame::ReadGround(&colours[0], &heights[0]);
    sNone = Rgba(((none >> 11) * 255 + 15) / 31, (((none >> 5) & 0x3F) * 255 + 31) / 63, ((none & 0x1F) * 255 + 15) / 31);
    u16* pTexels = static_cast<u16*>(renderer.LockTiled(sMap, kMapTexels, false));
    if (!pTexels)
    {
        return false;
    }
    const auto height = [&heights](s32 x, s32 y) { return x < 0 || y < 0 || x >= kMapSize || y >= kMapSize ? 0 : heights[y * kMapSize + x]; };
    for (s32 ty = 0; ty < kMapTexels; ty++)
    {
        for (s32 tx = 0; tx < kMapTexels; tx++)
        {
            const s32 bx = tx >> 1, by = ty >> 1;
            const s32 h = heights[by * kMapSize + bx];
            const bool rim = h > height(bx + ((tx & 1) ? 1 : -1), by) || h > height(bx, by + ((ty & 1) ? 1 : -1));
            const bool shadow = height((tx - 1) >> 1, (ty - 1) >> 1) > h;
            u16 c = colours[by * kMapSize + bx];
            if (rim || shadow)
            {
                const u32 k = rim ? 141 : 179; // 0.55 and 0.7 of 256
                const u32 r = ((c >> 11) * k) >> 8, g = (((c >> 5) & 0x3F) * k) >> 8, b = ((c & 0x1F) * k) >> 8;
                c = (u16)((r << 11) | (g << 5) | b);
            }
            pTexels[TiledIndex(tx, ty, kMapTexels)] = c;
        }
    }
    renderer.UnlockTiled(sMap);
    return true;
}

// A colour with each channel cut to 4 bits, as an RGBA4 texture holds it.
u32 Rgba4(u32 rgba)
{
    return (rgba & 0xF0F0F0F0u) | ((rgba >> 4) & 0x0F0F0F0Fu);
}

// The tint over the gangs' territories (RGBA4, a texel per block): every territory's edge in its gang's colour,
// and lighter over the one the player is in.
void BuildTint(C3DRenderer& renderer)
{
    std::vector<u8> owners(kMapSize * kMapSize);
    MinimapGame::ReadOwners(sView.arrows, &owners[0]);
    u32 colours[MinimapGame::kMaxGangs + 1] = {0};
    for (s32 i = 0; i < MinimapGame::kMaxGangs; i++)
    {
        colours[i + 1] = MinimapGame::ArrowColour(sView.arrows[i]) & 0xFFFFFFu;
    }
    std::vector<u32> texels(kMapSize * kMapSize);
    const auto owner = [&owners](s32 x, s32 y) { return x < 0 || y < 0 || x >= kMapSize || y >= kMapSize ? 0 : owners[y * kMapSize + x]; };
    for (s32 y = 0; y < kMapSize; y++)
    {
        for (s32 x = 0; x < kMapSize; x++)
        {
            const u8 o = owners[y * kMapSize + x];
            u32 a = 0;
            if (o)
            {
                const bool edge = owner(x - 1, y) != o || owner(x + 1, y) != o || owner(x, y - 1) != o || owner(x, y + 1) != o;
                a = edge ? 0xAA : o == sView.gang ? 0x44 : 0;
            }
            texels[y * kMapSize + x] = a ? Rgba4(colours[o] | (a << 24)) : 0;
        }
    }
    renderer.UploadTexture(sTint, kMapSize, kMapSize, &texels[0], RMIP_NONE, RTX_KNOWN | RTX_4444);
}

// What the tint shows: the gangs and the one the player is in.
u32 TintSignature()
{
    u32 h = (u32)sView.gang;
    for (s32 i = 0; i < MinimapGame::kMaxGangs; i++)
    {
        h = h * 31 + sView.arrows[i];
    }
    return h;
}

// The frame and the player's arrow, alpha cut at half.
void BuildAtlas(C3DRenderer& renderer)
{
    std::vector<u32> atlas(kAtlasSize * kAtlasSize, 0);
    const auto put = [&atlas](const unsigned int* pSrc, s32 w, s32 h, s32 x0) {
        for (s32 y = 0; y < h; y++)
        {
            for (s32 x = 0; x < w; x++)
            {
                const u32 c = pSrc[y * w + x];
                atlas[y * kAtlasSize + x0 + x] = (c >> 24) >= 128 ? c | 0xFF000000u : 0;
            }
        }
    };
    put(kFrame, kFrameW, kFrameH, 0);
    put(kArrow, kArrowW, kArrowH, kArrowX);
    sAtlas = renderer.CreateTexture();
    renderer.UploadTexture(sAtlas, kAtlasSize, kAtlasSize, &atlas[0], RMIP_NONE);
}

// Half size, a texel per 2x2: the mean of the opaque ones, opaque when at least two are.
void Halve(const u32* pSrc, s32 w, s32 h, u32* pDst, s32* pW, s32* pH)
{
    *pW = (w + 1) / 2;
    *pH = (h + 1) / 2;
    for (s32 y = 0; y < *pH; y++)
    {
        for (s32 x = 0; x < *pW; x++)
        {
            u32 r = 0, g = 0, b = 0, n = 0;
            for (s32 i = 0; i < 4; i++)
            {
                const s32 sx = x * 2 + (i & 1), sy = y * 2 + (i >> 1);
                const u32 c = sx < w && sy < h ? pSrc[sy * w + sx] : 0;
                if (c >> 24)
                {
                    r += c & 0xFF;
                    g += (c >> 8) & 0xFF;
                    b += (c >> 16) & 0xFF;
                    n++;
                }
            }
            pDst[y * *pW + x] = n >= 2 ? Rgba(r / n, g / n, b / n) : 0;
        }
    }
}

// The markers' sprites from the city's style, packed in rows a texel apart.
void BuildSprites(C3DRenderer& renderer)
{
    std::vector<u32> sheet(kSpritesW * kSpritesH, 0);
    static u32 full[64 * 64], half[32 * 32];
    s32 px = 0, py = 0, row = 0;
    const auto add = [&](s32 n, bool halve, SpriteRect* pRect) {
        s32 w, h;
        pRect->w = 0;
        if (!MinimapGame::ReadUserSprite(n, full, 64 * 64, &w, &h))
        {
            return;
        }
        const u32* pSrc = full;
        if (halve)
        {
            Halve(full, w, h, half, &w, &h);
            pSrc = half;
        }
        if (px + w > kSpritesW)
        {
            px = 0;
            py += row + 1;
            row = 0;
        }
        if (w > kSpritesW || py + h > kSpritesH)
        {
            return;
        }
        for (s32 y = 0; y < h; y++)
        {
            memcpy(&sheet[(py + y) * kSpritesW + px], pSrc + y * w, w * sizeof(u32));
        }
        const SpriteRect r = {(s16)px, (s16)py, (s16)w, (s16)h};
        *pRect = r;
        px += w + 1;
        row = h > row ? h : row;
    };
    for (s32 a = 1; a <= MinimapGame::kMaxArrow; a++)
    {
        add(a, true, &sArrows[a]);
        add(MinimapGame::kIconSprite + a, true, &sIcons[a]);
    }
    for (s32 i = 0; i < MinimapGame::kPipCount; i++)
    {
        add(MinimapGame::kPipSprite + i, false, &sPips[i]);
        add(MinimapGame::kPipSprite + i, true, &sSmallPips[i]);
    }
    renderer.UploadTexture(sSprites, kSpritesW, kSpritesH, &sheet[0], RMIP_NONE);
}

HudTheme::Quad MakeQuad(f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1, u32 rgba)
{
    const HudTheme::Quad q = {x0, y0, x1, y1, u0, v0, u1, v1, {rgba, rgba, rgba, rgba}};
    return q;
}

// A piece of the atlas copied 1:1 to (x, y), w x h of it from (sx, sy).
HudTheme::Quad AtlasQuad(s32 x, s32 y, s32 sx, s32 sy, s32 w, s32 h, u32 rgba = 0xFFFFFFFFu)
{
    const f32 k = 1.0f / kAtlasSize;
    return MakeQuad((f32)x, (f32)y, (f32)(x + w), (f32)(y + h), sx * k, sy * k, (sx + w) * k, (sy + h) * k, rgba);
}

void DrawFrame(HudTheme::Sink& sink, const HudTheme::Rect& f)
{
    // Enough for a frame across the whole bottom screen.
    HudTheme::Quad quads[160];
    s32 n = 0;
    const s32 w = kFrameW, h = kFrameH;
    quads[n++] = AtlasQuad(f.x0, f.y0, 0, 0, kCap, kCap);
    quads[n++] = AtlasQuad(f.x1 - kCap, f.y0, w - kCap, 0, kCap, kCap);
    quads[n++] = AtlasQuad(f.x0, f.y1 - kCap, 0, h - kCap, kCap, kCap);
    quads[n++] = AtlasQuad(f.x1 - kCap, f.y1 - kCap, w - kCap, h - kCap, kCap, kCap);
    const s32 tab = (f.x0 + f.x1 - kTabW) / 2;
    quads[n++] = AtlasQuad(tab, f.y0, kTabX, 0, kTabW, kCap);
    // The tubes: one rib period repeated, the last one cut.
    const auto tube = [&](s32 from, s32 to, s32 at, bool across, s32 src) {
        for (s32 p = from; p < to && n < 160; p += kRib)
        {
            const s32 len = to - p < kRib ? to - p : kRib;
            quads[n++] = across ? AtlasQuad(p, at, kRibX, src, len, kCap) : AtlasQuad(at, p, src, kRibX, kCap, len);
        }
    };
    tube(f.x0 + kCap, tab, f.y0, true, 0);
    tube(tab + kTabW, f.x1 - kCap, f.y0, true, 0);
    tube(f.x0 + kCap, f.x1 - kCap, f.y1 - kCap, true, h - kCap);
    tube(f.y0 + kCap, f.y1 - kCap, f.x0, false, 0);
    tube(f.y0 + kCap, f.y1 - kCap, f.x1 - kCap, false, w - kCap);
    sink.DrawQuads(quads, n, sAtlas, RS_BLEND_ALPHA | C3DRenderer::kStateNearest);
}

RVertex Vertex(f32 x, f32 y, u32 rgba, f32 u = 0.0f, f32 v = 0.0f)
{
    RVertex r;
    memset(&r, 0, sizeof(r));
    r.x = x;
    r.y = y;
    r.rhw = 1.0f;
    r.rgba = rgba;
    r.u = u;
    r.v = v;
    return r;
}

// The player's arrow at (x, y), turned by the heading (Ang16, 1440 to the turn).
void DrawArrow(HudTheme::Sink& sink, f32 x, f32 y, s32 heading)
{
    // A ped at heading a moves by (sin a, cos a) (Char_B4's PolarToCartesian), so 0 faces down the map.
    const f32 a = heading * (6.2831853f / 1440.0f);
    const f32 c = cosf(a), s = sinf(a);
    const f32 hw = kArrowW * 0.5f, hh = kArrowH * 0.5f, k = 1.0f / kAtlasSize;
    const f32 xs[4] = {-hw, hw, hw, -hw}, ys[4] = {-hh, -hh, hh, hh};
    const f32 us[4] = {0, (f32)kArrowW, (f32)kArrowW, 0}, vs[4] = {0, 0, (f32)kArrowH, (f32)kArrowH};
    RVertex v[4];
    for (s32 i = 0; i < 4; i++)
    {
        // The atlas arrow points up, (0, -1), and lands on (s, c).
        v[i] = Vertex(x - xs[i] * c - ys[i] * s, y + xs[i] * s - ys[i] * c, 0xFFFFFFFFu, (kArrowX + us[i]) * k, vs[i] * k);
    }
    const RVertex tris[6] = {v[0], v[1], v[2], v[0], v[2], v[3]};
    sink.DrawTriangles(tris, 6, sAtlas, RS_BLEND_ALPHA);
}

// A sprite of the markers' sheet centred on (x, y), upright.
HudTheme::Quad SpriteQuad(s32 x, s32 y, const SpriteRect& r)
{
    const f32 ku = 1.0f / kSpritesW, kv = 1.0f / kSpritesH;
    const s32 x0 = x - r.w / 2, y0 = y - r.h / 2;
    return MakeQuad((f32)x0, (f32)y0, (f32)(x0 + r.w), (f32)(y0 + r.h), r.x * ku, r.y * kv, (r.x + r.w) * ku, (r.y + r.h) * kv,
                    0xFFFFFFFFu);
}

// A sprite of the sheet centred on (x, y), turned so that its top points along the unit vector (ux, uy).
void SpriteTurned(RVertex* pOut, f32 x, f32 y, const SpriteRect& r, f32 ux, f32 uy)
{
    const f32 ku = 1.0f / kSpritesW, kv = 1.0f / kSpritesH, hw = r.w * 0.5f, hh = r.h * 0.5f;
    const f32 xs[4] = {-hw, hw, hw, -hw}, ys[4] = {-hh, -hh, hh, hh};
    const f32 us[4] = {0, (f32)r.w, (f32)r.w, 0}, vs[4] = {0, 0, (f32)r.h, (f32)r.h};
    RVertex v[4];
    for (s32 i = 0; i < 4; i++)
    {
        v[i] = Vertex(x - xs[i] * uy - ys[i] * ux, y + xs[i] * ux - ys[i] * uy, 0xFFFFFFFFu, (r.x + us[i]) * ku, (r.y + vs[i]) * kv);
    }
    const RVertex tris[6] = {v[0], v[1], v[2], v[0], v[2], v[3]};
    memcpy(pOut, tris, sizeof(tris));
}

// Inside the map: a mission phone is its pip, the info phone and a gang's territory the gang's icon. Outside: a phone
// is the game's own arrow towards it on the map's edge, with its pip; a territory, the icon on the edge.
void DrawMarks(HudTheme::Sink& sink, s32 hx0, s32 hy0, s32 hx1, s32 hy1, s32 cx, s32 cy)
{
    HudTheme::Quad quads[kMaxMarks];
    RVertex tris[kMaxMarks * 12];
    s32 nq = 0, nt = 0;
    const f32 x0 = (f32)(hx0 + kEdgeMargin), y0 = (f32)(hy0 + kEdgeMargin), x1 = (f32)(hx1 - kEdgeMargin), y1 = (f32)(hy1 - kEdgeMargin);
    for (s32 i = 0; i < sView.markCount; i++)
    {
        const MinimapGame::Mark& m = sView.marks[i];
        const s32 x = cx + m.x4 - sView.px4, y = cy + m.y4 - sView.py4;
        const SpriteRect& arrow = sArrows[m.arrow <= MinimapGame::kMaxArrow ? m.arrow : 0];
        const SpriteRect& icon = sIcons[m.arrow <= MinimapGame::kMaxArrow ? m.arrow : 0];
        const bool mission = m.kind == MinimapGame::kMissionPhone;
        if (x < x0 || x > x1 || y < y0 || y > y1)
        {
            // Along the ray from the player, onto the inner edge.
            const f32 dx = (f32)(x - cx), dy = (f32)(y - cy), len = sqrtf(dx * dx + dy * dy);
            const f32 tx = dx ? ((dx > 0 ? x1 : x0) - cx) / dx : 1e9f, ty = dy ? ((dy > 0 ? y1 : y0) - cy) / dy : 1e9f;
            const f32 t = tx < ty ? tx : ty, ex = cx + dx * t, ey = cy + dy * t, ux = dx / len, uy = dy / len;
            if (m.kind == MinimapGame::kTerritory)
            {
                if (icon.w)
                {
                    quads[nq++] = SpriteQuad((s32)floorf(ex + 0.5f), (s32)floorf(ey + 0.5f), icon);
                }
                continue;
            }
            // The arrow's tip on the map's edge.
            const f32 back = arrow.h * 0.5f - kEdgeMargin, ax = ex - ux * back, ay = ey - uy * back;
            if (arrow.w)
            {
                SpriteTurned(tris + nt, ax, ay, arrow, ux, uy);
                nt += 6;
            }
            if (mission && sSmallPips[m.pip].w)
            {
                SpriteTurned(tris + nt, ax, ay, sSmallPips[m.pip], ux, uy);
                nt += 6;
            }
        }
        else if (mission ? sPips[m.pip].w : icon.w)
        {
            quads[nq++] = SpriteQuad(x, y, mission ? sPips[m.pip] : icon);
        }
    }
    sink.DrawTriangles(tris, nt, sSprites, RS_BLEND_ALPHA);
    sink.DrawQuads(quads, nq, sSprites, RS_BLEND_ALPHA | C3DRenderer::kStateNearest);
}
} // namespace

u32 Update(C3DRenderer& renderer)
{
    const u32 city = HudTheme::CitySignature();
    if (!city || !MinimapGame::ReadView(&sView))
    {
        return 0;
    }
    if (city != sCity)
    {
        const u64 t0 = svcGetSystemTick();
        if (!sMap)
        {
            sMap = renderer.CreateTexture();
            sTint = renderer.CreateTexture();
            sSprites = renderer.CreateTexture();
            BuildAtlas(renderer);
        }
        sCity = city;
        sMapOk = BuildMap(renderer);
        BuildSprites(renderer);
        sTintSig = TintSignature();
        BuildTint(renderer);
        fprintf(stderr, "minimap: built in %.1f ms; map %d KB, tint %d KB, frame %d KB and markers %d KB of linear memory\n",
                (svcGetSystemTick() - t0) * 1000.0 / SYSCLOCK_ARM11, kMapTexels * kMapTexels * 2 / 1024, kMapSize * kMapSize * 2 / 1024,
                kAtlasSize * kAtlasSize * 2 / 1024, kSpritesW * kSpritesH * 2 / 1024);
    }
    if (!sMapOk)
    {
        return 0;
    }
    if (TintSignature() != sTintSig)
    {
        sTintSig = TintSignature();
        BuildTint(renderer);
    }
    // 64 headings to the turn are enough for an 11 pixel arrow.
    sView.heading = (sView.heading + 11) / 22 * 22;
    u32 h = 2166136261u;
    const auto mix = [&h](u32 v) { h = (h ^ v) * 16777619u; };
    mix(city);
    mix((u32)sView.px4);
    mix((u32)sView.py4);
    mix((u32)sView.heading);
    mix(sTintSig);
    for (s32 i = 0; i < sView.markCount; i++)
    {
        mix((u32)sView.marks[i].x4);
        mix((u32)sView.marks[i].y4);
        mix((u32)sView.marks[i].kind | (sView.marks[i].arrow << 8) | (sView.marks[i].pip << 16));
    }
    return h ? h : 1;
}

void Draw(C3DRenderer&, HudTheme::Sink& sink, const HudTheme::Rect& frame)
{
    const s32 hx0 = frame.x0 + kTube, hy0 = frame.y0 + kTube, hx1 = frame.x1 - kTube, hy1 = frame.y1 - kTube;
    const s32 cx = (hx0 + hx1) / 2, cy = (hy0 + hy1) / 2;
    HudTheme::Quad q = MakeQuad((f32)hx0, (f32)hy0, (f32)hx1, (f32)hy1, 0, 0, 0, 0, sNone);
    sink.DrawQuads(&q, 1, 0, RS_NO_TEXTURE);
    // The city's part in the window, at whole pixels so each texel covers 2x2 of them.
    const s32 left = cx - sView.px4, top = cy - sView.py4, span = kMapSize * kPxPerBlock;
    const s32 x0 = hx0 > left ? hx0 : left, y0 = hy0 > top ? hy0 : top;
    const s32 x1 = hx1 < left + span ? hx1 : left + span, y1 = hy1 < top + span ? hy1 : top + span;
    if (x0 < x1 && y0 < y1)
    {
        const f32 k = 1.0f / span;
        q = MakeQuad((f32)x0, (f32)y0, (f32)x1, (f32)y1, (x0 - left) * k, (y0 - top) * k, (x1 - left) * k, (y1 - top) * k, 0xFFFFFFFFu);
        sink.DrawQuads(&q, 1, sMap, C3DRenderer::kStateNearest);
        sink.DrawQuads(&q, 1, sTint, RS_BLEND_ALPHA | C3DRenderer::kStateNearest);
    }
    // The tube's shadow on the map, from the top left.
    HudTheme::Quad shade[2];
    const u32 dark = Rgba(0, 0, 0, 120), clear = Rgba(0, 0, 0, 0);
    shade[0] = MakeQuad((f32)hx0, (f32)hy0, (f32)hx1, (f32)(hy0 + 5), 0, 0, 0, 0, dark);
    shade[0].rgba[2] = shade[0].rgba[3] = clear;
    shade[1] = MakeQuad((f32)hx0, (f32)hy0, (f32)(hx0 + 5), (f32)hy1, 0, 0, 0, 0, dark);
    shade[1].rgba[1] = shade[1].rgba[2] = clear;
    sink.DrawQuads(shade, 2, 0, RS_NO_TEXTURE | RS_BLEND_ALPHA);
    DrawMarks(sink, hx0, hy0, hx1, hy1, cx, cy);
    DrawArrow(sink, (f32)cx, (f32)cy, sView.heading);
    DrawFrame(sink, frame);
}

void Shutdown(C3DRenderer& renderer)
{
    renderer.FreeTexture(sMap);
    renderer.FreeTexture(sTint);
    renderer.FreeTexture(sAtlas);
    renderer.FreeTexture(sSprites);
    sMap = sTint = sAtlas = sSprites = 0;
    sCity = 0;
    sMapOk = false;
    sTintSig = 0;
}
} // namespace Minimap
