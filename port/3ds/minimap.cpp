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
// The police scanner frame (port/docs/minimap/assets/frames/J_police_scanner, its nine.json): corners, an edge strip
// to repeat, the light bar's tab, and the frame's width over the map's edge.
const s32 kCap = 20;
const s32 kRibX = 20;
const s32 kRib = 12;
const s32 kTabX = 33;
const s32 kTabW = 44;
const s32 kTube = 13;
// The lamps: the light bar's lens from the tab's left, lit in the territory's gang colour, and the small one top
// right from the frame's right edge, red when that gang hates the player, else green.
const s32 kGangLampX = 45 - kTabX;
const s32 kGangLampY = 2;
const s32 kHostileLampX = 11;
const s32 kHostileLampY = 4;
// The atlas: the frame at (0, 0), the player arrow right of it, the lit lamps under the frame. The arrow is drawn
// filtered, so a clear column between them keeps the frame's edge out of the arrow's.
const s32 kAtlasSize = 128;
const s32 kArrowX = kFrameW + 1;
const s32 kLampsY = kFrameH + 1;
const s32 kHostileAtlasX = kLampGangW + 1;
// The game's sprites for the markers, read from the city's style.
const s32 kSpritesW = 128;
const s32 kSpritesH = 64;
// Markers stay this far inside the map; further out they go on its edge.
const s32 kEdgeMargin = 5;
// The pause map: the whole bottom screen, its zooms in pixels per block, and its buttons, squares of the frame.
const s32 kScreenW = 320;
const s32 kScreenH = 240;
const s32 kZooms[] = {1, 2, 4, 8};
const s32 kZoomCount = 4;
const HudTheme::Rect kZoomIn = {276, 152, 316, 192};
const HudTheme::Rect kZoomOut = {276, 196, 316, 236};

struct SpriteRect
{
    s16 x, y, w, h;
};

RTexture* sMap;
RTexture* sEdges;
RTexture* sFill;
RTexture* sAtlas;
RTexture* sSprites;
// At half size, as the bottom screen draws the HUD, but the pips at full size to be seen on the map.
SpriteRect sArrows[MinimapGame::kMaxArrow + 1];
SpriteRect sIcons[MinimapGame::kMaxArrow + 1];
SpriteRect sPips[MinimapGame::kPipCount];
SpriteRect sSmallPips[MinimapGame::kPipCount];
SpriteRect sCross;
SpriteRect sPlus;
SpriteRect sMinus;
u32 sCity;
bool sMapOk;
// Each block's gang, 1 + its pool slot (MinimapGame::ReadOwners), in the textures' tiled order for BuildFill.
std::vector<u8> sOwners;
u32 sEdgeSig;
s32 sFillGang;
u32 sFillRgba;
u32 sNone; // the ground where no block has a lid, also around the city
// The respect meter's green and red lamps, and the colours the frame's two lamps are lit in, 0 when dark.
u32 sGreenRgba, sRedRgba;
u32 sGangLampRgba, sHostileLampRgba;
MinimapGame::View sView;

enum PauseTouch
{
    kNoTouch,
    kDrag,
    kZoomInHeld,
    kZoomOutHeld,
};

bool sPauseShown;
s32 sZoom = 1;
f32 sPanX, sPanY; // the screen's middle, in blocks
PauseTouch sTouch;
// Where the stylus grabbed the map, and the pan it moves from.
s32 sGrabX, sGrabY;
f32 sGrabPanX, sGrabPanY;

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

// Every territory's edge in its gang's colour with a shade outside it (RGBA4, a texel per block), from the
// gangs in sView.arrows.
void BuildEdges(C3DRenderer& renderer)
{
    std::vector<u8> owners(kMapSize * kMapSize);
    const u8* pOwners = &owners[0];
    MinimapGame::ReadOwners(sView.arrows, &owners[0]);
    sOwners.resize(kMapSize * kMapSize);
    u32 colours[MinimapGame::kMaxGangs + 1] = {0};
    for (s32 i = 0; i < MinimapGame::kMaxGangs; i++)
    {
        colours[i + 1] = MinimapGame::ArrowColour(sView.arrows[i]) & 0xFFFFFFu;
    }
    std::vector<u32> texels(kMapSize * kMapSize);
    const auto owner = [pOwners](s32 x, s32 y) { return x < 0 || y < 0 || x >= kMapSize || y >= kMapSize ? 0 : pOwners[y * kMapSize + x]; };
    for (s32 y = 0; y < kMapSize; y++)
    {
        for (s32 x = 0; x < kMapSize; x++)
        {
            const u8 o = pOwners[y * kMapSize + x];
            const u8 l = owner(x - 1, y), r = owner(x + 1, y), t = owner(x, y - 1), b = owner(x, y + 1);
            u32 c = 0;
            if (o && (l != o || r != o || t != o || b != o))
            {
                // Dashed, a block in the colour and a block shaded, to tell it from a road of that colour.
                c = (x + y) & 1 ? 0x66000000u : colours[o] | 0xEE000000u;
            }
            else if (!o && (l || r || t || b))
            {
                // A shade along the outside, so that the edge shows in any colour over any ground.
                c = 0x66000000u;
            }
            texels[y * kMapSize + x] = Rgba4(c);
            sOwners[TiledIndex(x, y, kMapSize)] = o;
        }
    }
    renderer.UploadTexture(sEdges, kMapSize, kMapSize, &texels[0], RMIP_NONE, RTX_KNOWN | RTX_4444);
}

// The light tint over the territory the player is in (A8, drawn in the gang's colour).
void BuildFill(C3DRenderer& renderer)
{
    u8* pAlpha = static_cast<u8*>(renderer.LockTiled(sFill, kMapSize, true));
    if (!pAlpha || sOwners.empty())
    {
        return;
    }
    const u8 gang = (u8)sView.gang;
    for (s32 i = 0; i < kMapSize * kMapSize; i++)
    {
        pAlpha[i] = gang && sOwners[i] == gang ? 0x44 : 0;
    }
    renderer.UnlockTiled(sFill);
}

// The gangs the edges show.
u32 EdgeSignature()
{
    u32 h = 1;
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
    const auto putBelow = [&atlas](const unsigned int* pSrc, s32 w, s32 h, s32 x0) {
        for (s32 y = 0; y < h; y++)
        {
            for (s32 x = 0; x < w; x++)
            {
                const u32 c = pSrc[y * w + x];
                atlas[(kLampsY + y) * kAtlasSize + x0 + x] = (c >> 24) >= 128 ? c | 0xFF000000u : 0;
            }
        }
    };
    putBelow(kLampGang, kLampGangW, kLampGangH, 0);
    putBelow(kLampHostile, kLampHostileW, kLampHostileH, kHostileAtlasX);
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

// The save point's icon: the pause font's '+' cut into a Latin cross, its arms one texel shorter than the stem
// is wide and its foot twice as long, with a shade around it. At most 32x32.
bool BuildCross(u32* pDst, s32* pW, s32* pH)
{
    static u32 glyph[64 * 64];
    s32 w, h;
    if (!MinimapGame::ReadPauseFontGlyph('+', glyph, 64 * 64, &w, &h))
    {
        return false;
    }
    const auto opaque = [&](s32 x, s32 y) { return (glyph[y * w + x] >> 24) != 0; };
    // The '+' in the glyph: its bounds, the stem's columns at its top and the bar's rows at its left.
    s32 x0 = w, x1 = -1, y0 = h, y1 = -1;
    for (s32 y = 0; y < h; y++)
    {
        for (s32 x = 0; x < w; x++)
        {
            if (opaque(x, y))
            {
                x0 = x < x0 ? x : x0;
                x1 = x > x1 ? x : x1;
                y0 = y < y0 ? y : y0;
                y1 = y;
            }
        }
    }
    s32 sx = x0, sw = 0, by = y0, bh = 0;
    while (sx <= x1 && !opaque(sx, y0))
    {
        sx++;
    }
    while (sx + sw <= x1 && opaque(sx + sw, y0))
    {
        sw++;
    }
    while (by <= y1 && !opaque(x0, by))
    {
        by++;
    }
    while (by + bh <= y1 && opaque(x0, by + bh))
    {
        bh++;
    }
    const s32 arm = sw - 1;
    if (sw < 2 || sw > 8 || bh > 8 || sx - arm < x0 || sx + sw + arm - 1 > x1)
    {
        return false;
    }
    // The cross's columns and rows in the glyph; the right arm keeps the glyph's end column.
    s32 cols[32], rows[32], nc = 0, nr = 0;
    for (s32 x = sx - arm; x < sx + sw + arm - 1; x++)
    {
        cols[nc++] = x;
    }
    cols[nc++] = x1;
    for (s32 y = y0; y < y0 + arm; y++)
    {
        rows[nr++] = y;
    }
    for (s32 y = by; y < by + bh; y++)
    {
        rows[nr++] = y;
    }
    for (s32 i = 0; i <= 2 * arm; i++)
    {
        rows[nr++] = by + bh + i < y1 ? by + bh + i : y1;
    }
    const auto at = [&](s32 x, s32 y) { return x < 0 || y < 0 || x >= nc || y >= nr ? 0u : glyph[rows[y] * w + cols[x]]; };
    *pW = nc + 2;
    *pH = nr + 2;
    for (s32 y = 0; y < *pH; y++)
    {
        for (s32 x = 0; x < *pW; x++)
        {
            u32 c = at(x - 1, y - 1);
            for (s32 i = 0; i < 9 && !(c >> 24); i++)
            {
                c = (at(x - 2 + i % 3, y - 2 + i / 3) >> 24) ? Rgba(0, 0, 0, 150) : 0;
            }
            pDst[y * *pW + x] = c;
        }
    }
    return true;
}

// The zoom buttons' glyphs: the pause font's '+' cut to its bounds, and its bar alone for the '-', which the font
// lacks. At most 32x32.
bool BuildPlusMinus(u32* pPlus, u32* pMinus, s32* pW, s32* pH)
{
    static u32 glyph[64 * 64];
    s32 w, h;
    if (!MinimapGame::ReadPauseFontGlyph('+', glyph, 64 * 64, &w, &h))
    {
        return false;
    }
    s32 x0 = w, x1 = -1, y0 = h, y1 = -1;
    for (s32 y = 0; y < h; y++)
    {
        for (s32 x = 0; x < w; x++)
        {
            if (glyph[y * w + x] >> 24)
            {
                x0 = x < x0 ? x : x0;
                x1 = x > x1 ? x : x1;
                y0 = y < y0 ? y : y0;
                y1 = y;
            }
        }
    }
    *pW = x1 - x0 + 1;
    *pH = y1 - y0 + 1;
    if (x1 < 0 || *pW > 32 || *pH > 32)
    {
        return false;
    }
    for (s32 y = 0; y < *pH; y++)
    {
        const bool bar = (glyph[(y0 + y) * w + x0] >> 24) != 0;
        for (s32 x = 0; x < *pW; x++)
        {
            const u32 c = glyph[(y0 + y) * w + x0 + x];
            pPlus[y * *pW + x] = c;
            pMinus[y * *pW + x] = bar ? c : 0;
        }
    }
    return true;
}

// A colour at full brightness, its hue kept: a lamp lit in it.
u32 Lit(u32 rgba)
{
    const u32 r = rgba & 0xFF, g = (rgba >> 8) & 0xFF, b = (rgba >> 16) & 0xFF;
    const u32 m = r > g ? (r > b ? r : b) : (g > b ? g : b);
    return m ? Rgba(r * 255 / m, g * 255 / m, b * 255 / m) : 0;
}

// The mean colour of a user sprite's opaque texels, lit; 0 when it is missing.
u32 SpriteColour(s32 n)
{
    static u32 texels[64 * 64];
    s32 w, h;
    if (!MinimapGame::ReadUserSprite(n, texels, 64 * 64, &w, &h))
    {
        return 0;
    }
    u32 r = 0, g = 0, b = 0, count = 0;
    for (s32 i = 0; i < w * h; i++)
    {
        if (texels[i] >> 24)
        {
            r += texels[i] & 0xFF;
            g += (texels[i] >> 8) & 0xFF;
            b += (texels[i] >> 16) & 0xFF;
            count++;
        }
    }
    return count ? Lit(Rgba(r / count, g / count, b / count)) : 0;
}

// The markers' sprites from the city's style, packed in rows a texel apart.
void BuildSprites(C3DRenderer& renderer)
{
    std::vector<u32> sheet(kSpritesW * kSpritesH, 0);
    static u32 full[64 * 64], half[32 * 32];
    s32 px = 0, py = 0, row = 0;
    const auto place = [&](const u32* pSrc, s32 w, s32 h, SpriteRect* pRect) {
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
    const auto add = [&](s32 n, bool halve, SpriteRect* pRect) {
        s32 w, h;
        pRect->w = 0;
        if (!MinimapGame::ReadUserSprite(n, full, 64 * 64, &w, &h))
        {
            return;
        }
        if (halve)
        {
            Halve(full, w, h, half, &w, &h);
        }
        place(halve ? half : full, w, h, pRect);
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
    s32 w, h;
    sCross.w = 0;
    if (BuildCross(half, &w, &h))
    {
        place(half, w, h, &sCross);
    }
    sPlus.w = sMinus.w = 0;
    static u32 minus[32 * 32];
    if (BuildPlusMinus(half, minus, &w, &h))
    {
        place(half, w, h, &sPlus);
        place(minus, w, h, &sMinus);
    }
    renderer.UploadTexture(sSprites, kSpritesW, kSpritesH, &sheet[0], RMIP_NONE);
    sGreenRgba = SpriteColour(MinimapGame::kMeterLampSprite);
    sRedRgba = SpriteColour(MinimapGame::kMeterLampSprite + 2);
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

void DrawFrame(HudTheme::Sink& sink, const HudTheme::Rect& f, bool tab = true)
{
    // Enough for a frame across the whole bottom screen.
    HudTheme::Quad quads[160];
    s32 n = 0;
    const s32 w = kFrameW, h = kFrameH;
    quads[n++] = AtlasQuad(f.x0, f.y0, 0, 0, kCap, kCap);
    quads[n++] = AtlasQuad(f.x1 - kCap, f.y0, w - kCap, 0, kCap, kCap);
    quads[n++] = AtlasQuad(f.x0, f.y1 - kCap, 0, h - kCap, kCap, kCap);
    quads[n++] = AtlasQuad(f.x1 - kCap, f.y1 - kCap, w - kCap, h - kCap, kCap, kCap);
    const s32 tabX = (f.x0 + f.x1 - kTabW) / 2;
    if (tab)
    {
        quads[n++] = AtlasQuad(tabX, f.y0, kTabX, 0, kTabW, kCap);
    }
    // The edges: one strip repeated, the last one cut.
    const auto tube = [&](s32 from, s32 to, s32 at, bool across, s32 src) {
        for (s32 p = from; p < to && n < 160; p += kRib)
        {
            const s32 len = to - p < kRib ? to - p : kRib;
            quads[n++] = across ? AtlasQuad(p, at, kRibX, src, len, kCap) : AtlasQuad(at, p, src, kRibX, kCap, len);
        }
    };
    tube(f.x0 + kCap, tab ? tabX : f.x1 - kCap, f.y0, true, 0);
    if (tab)
    {
        tube(tabX + kTabW, f.x1 - kCap, f.y0, true, 0);
    }
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

// Inside the map: a mission phone is its pip, the info phone and a gang's territory the gang's icon, a save point the
// cross. Outside: a phone is the game's own arrow towards it on the map's edge, with its pip; a territory or a save
// point, its icon on the edge. Quarter block (0, 0) lands on (ox, oy), k pixels per quarter block; the edge's marks
// are on the rays from (cx, cy).
void DrawMarks(HudTheme::Sink& sink, s32 hx0, s32 hy0, s32 hx1, s32 hy1, f32 ox, f32 oy, f32 k, f32 cx, f32 cy)
{
    HudTheme::Quad quads[kMaxMarks];
    RVertex tris[kMaxMarks * 12];
    s32 nq = 0, nt = 0;
    const f32 x0 = (f32)(hx0 + kEdgeMargin), y0 = (f32)(hy0 + kEdgeMargin), x1 = (f32)(hx1 - kEdgeMargin), y1 = (f32)(hy1 - kEdgeMargin);
    for (s32 i = 0; i < sView.markCount; i++)
    {
        const MinimapGame::Mark& m = sView.marks[i];
        const f32 x = ox + m.x4 * k, y = oy + m.y4 * k;
        const SpriteRect& arrow = sArrows[m.arrow <= MinimapGame::kMaxArrow ? m.arrow : 0];
        const SpriteRect& icon = m.kind == MinimapGame::kSavePoint ? sCross : sIcons[m.arrow <= MinimapGame::kMaxArrow ? m.arrow : 0];
        const bool mission = m.kind == MinimapGame::kMissionPhone;
        if (x < x0 || x > x1 || y < y0 || y > y1)
        {
            // Along the ray from (cx, cy), onto the inner edge.
            const f32 dx = x - cx, dy = y - cy, len = sqrtf(dx * dx + dy * dy);
            const f32 tx = dx ? ((dx > 0 ? x1 : x0) - cx) / dx : 1e9f, ty = dy ? ((dy > 0 ? y1 : y0) - cy) / dy : 1e9f;
            const f32 t = tx < ty ? tx : ty, ex = cx + dx * t, ey = cy + dy * t, ux = dx / len, uy = dy / len;
            if (m.kind == MinimapGame::kTerritory || m.kind == MinimapGame::kSavePoint)
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
            quads[nq++] = SpriteQuad((s32)floorf(x + 0.5f), (s32)floorf(y + 0.5f), mission ? sPips[m.pip] : icon);
        }
    }
    sink.DrawTriangles(tris, nt, sSprites, RS_BLEND_ALPHA);
    sink.DrawQuads(quads, nq, sSprites, RS_BLEND_ALPHA | C3DRenderer::kStateNearest);
}

// The ground, territories and tint in the window (hx0, hy0)-(hx1, hy1), with the city's top left corner on (left, top)
// at `zoom` pixels per block. Under 2 the map's texels are filtered, as nearest sampling would drop some.
void DrawGround(HudTheme::Sink& sink, s32 hx0, s32 hy0, s32 hx1, s32 hy1, s32 left, s32 top, s32 zoom)
{
    HudTheme::Quad q = MakeQuad((f32)hx0, (f32)hy0, (f32)hx1, (f32)hy1, 0, 0, 0, 0, sNone);
    sink.DrawQuads(&q, 1, 0, RS_NO_TEXTURE);
    const s32 span = kMapSize * zoom;
    const s32 x0 = hx0 > left ? hx0 : left, y0 = hy0 > top ? hy0 : top;
    const s32 x1 = hx1 < left + span ? hx1 : left + span, y1 = hy1 < top + span ? hy1 : top + span;
    if (x0 < x1 && y0 < y1)
    {
        const f32 k = 1.0f / span;
        q = MakeQuad((f32)x0, (f32)y0, (f32)x1, (f32)y1, (x0 - left) * k, (y0 - top) * k, (x1 - left) * k, (y1 - top) * k, 0xFFFFFFFFu);
        sink.DrawQuads(&q, 1, sMap, zoom >= 2 ? C3DRenderer::kStateNearest : 0);
        if (sView.gang)
        {
            for (s32 i = 0; i < 4; i++)
            {
                q.rgba[i] = sFillRgba;
            }
            sink.DrawQuads(&q, 1, sFill, RS_BLEND_ALPHA | C3DRenderer::kStateNearest);
            for (s32 i = 0; i < 4; i++)
            {
                q.rgba[i] = 0xFFFFFFFFu;
            }
        }
        sink.DrawQuads(&q, 1, sEdges, RS_BLEND_ALPHA | C3DRenderer::kStateNearest);
    }
}

// The pan's limits: the screen's middle keeps the city across the screen, or in its middle when it is smaller.
f32 ClampPan(f32 pan, s32 screen)
{
    const f32 half = screen * 0.5f / kZooms[sZoom];
    return half * 2 >= kMapSize ? kMapSize * 0.5f : pan < half ? half : pan > kMapSize - half ? kMapSize - half : pan;
}

s32 PanLeft()
{
    return kScreenW / 2 - (s32)floorf(sPanX * kZooms[sZoom] + 0.5f);
}

s32 PanTop()
{
    return kScreenH / 2 - (s32)floorf(sPanY * kZooms[sZoom] + 0.5f);
}

bool Inside(const HudTheme::Rect& r, const touchPosition& t)
{
    return t.px >= r.x0 && t.px < r.x1 && t.py >= r.y0 && t.py < r.y1;
}

// A zoom button: the frame around a shade with the pause font's glyph, sunk while held, dimmed when it can go
// no further.
void DrawButton(HudTheme::Sink& sink, const HudTheme::Rect& r, const SpriteRect& glyph, bool active, bool held)
{
    HudTheme::Quad q = MakeQuad((f32)(r.x0 + kTube), (f32)(r.y0 + kTube), (f32)(r.x1 - kTube), (f32)(r.y1 - kTube), 0, 0, 0, 0,
                                Rgba(0, 0, 0, 160));
    sink.DrawQuads(&q, 1, 0, RS_NO_TEXTURE | RS_BLEND_ALPHA);
    if (glyph.w)
    {
        const s32 sunk = held ? 1 : 0;
        q = SpriteQuad((r.x0 + r.x1) / 2 + sunk, (r.y0 + r.y1) / 2 + sunk, glyph);
        for (s32 i = 0; i < 4; i++)
        {
            q.rgba[i] = active ? 0xFFFFFFFFu : 0x80FFFFFFu;
        }
        sink.DrawQuads(&q, 1, sSprites, RS_BLEND_ALPHA | C3DRenderer::kStateNearest);
    }
    DrawFrame(sink, r, false);
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
            sEdges = renderer.CreateTexture();
            sFill = renderer.CreateTexture();
            sSprites = renderer.CreateTexture();
            BuildAtlas(renderer);
        }
        sCity = city;
        sMapOk = BuildMap(renderer);
        BuildSprites(renderer);
        sEdgeSig = 0;
        fprintf(stderr, "minimap: built in %.1f ms; map %d KB, edges %d KB, fill %d KB, frame %d KB and markers %d KB of linear memory\n",
                (svcGetSystemTick() - t0) * 1000.0 / SYSCLOCK_ARM11, kMapTexels * kMapTexels * 2 / 1024, kMapSize * kMapSize * 2 / 1024,
                kMapSize * kMapSize / 1024, kAtlasSize * kAtlasSize * 2 / 1024, kSpritesW * kSpritesH * 4 / 1024);
    }
    if (!sMapOk)
    {
        return 0;
    }
    if (EdgeSignature() != sEdgeSig)
    {
        sEdgeSig = EdgeSignature();
        BuildEdges(renderer);
        sFillGang = -1;
    }
    if (sView.gang != sFillGang)
    {
        sFillGang = sView.gang;
        BuildFill(renderer);
    }
    sFillRgba = sView.gang ? MinimapGame::ArrowColour(sView.arrows[sView.gang - 1]) : 0;
    sGangLampRgba = sView.gang ? Lit(sFillRgba) : 0;
    sHostileLampRgba = !sView.gang ? 0 : sView.hostile ? sRedRgba : sGreenRgba;
    // 64 headings to the turn are enough for an 11 pixel arrow.
    sView.heading = (sView.heading + 11) / 22 * 22;
    u32 h = 2166136261u;
    const auto mix = [&h](u32 v) { h = (h ^ v) * 16777619u; };
    mix(city);
    mix((u32)sView.px4);
    mix((u32)sView.py4);
    mix((u32)sView.heading);
    mix(sEdgeSig);
    mix((u32)sView.gang);
    mix((u32)sView.hostile);
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
    // At whole pixels, so each texel covers 2x2 of them.
    DrawGround(sink, hx0, hy0, hx1, hy1, cx - sView.px4, cy - sView.py4, kPxPerBlock);
    // The frame's shadow on the map, from the top left.
    HudTheme::Quad shade[2];
    const u32 dark = Rgba(0, 0, 0, 120), clear = Rgba(0, 0, 0, 0);
    shade[0] = MakeQuad((f32)hx0, (f32)hy0, (f32)hx1, (f32)(hy0 + 5), 0, 0, 0, 0, dark);
    shade[0].rgba[2] = shade[0].rgba[3] = clear;
    shade[1] = MakeQuad((f32)hx0, (f32)hy0, (f32)(hx0 + 5), (f32)hy1, 0, 0, 0, 0, dark);
    shade[1].rgba[1] = shade[1].rgba[2] = clear;
    sink.DrawQuads(shade, 2, 0, RS_NO_TEXTURE | RS_BLEND_ALPHA);
    DrawMarks(sink, hx0, hy0, hx1, hy1, (f32)(cx - sView.px4), (f32)(cy - sView.py4), 1.0f, (f32)cx, (f32)cy);
    DrawArrow(sink, (f32)cx, (f32)cy, sView.heading);
    DrawFrame(sink, frame);
    // The lamps lit over the frame's dark ones, outside any territory left dark.
    if (sView.gang)
    {
        const s32 tabX = (frame.x0 + frame.x1 - kTabW) / 2;
        const HudTheme::Quad lamps[2] = {
            AtlasQuad(tabX + kGangLampX, frame.y0 + kGangLampY, 0, kLampsY, kLampGangW, kLampGangH, sGangLampRgba),
            AtlasQuad(frame.x1 - kHostileLampX, frame.y0 + kHostileLampY, kHostileAtlasX, kLampsY, kLampHostileW, kLampHostileH,
                      sHostileLampRgba),
        };
        sink.DrawQuads(lamps, 2, sAtlas, RS_BLEND_ALPHA | C3DRenderer::kStateNearest);
    }
}

u32 UpdatePause(C3DRenderer& renderer)
{
    if (!MinimapGame::PauseMenuOpen())
    {
        sPauseShown = false;
        return 0;
    }
    const u32 sig = Update(renderer);
    if (!sig)
    {
        return 0;
    }
    if (!sPauseShown)
    {
        sPauseShown = true;
        sTouch = kNoTouch;
        sPanX = sView.px4 * 0.25f;
        sPanY = sView.py4 * 0.25f;
    }
    touchPosition t;
    hidTouchRead(&t);
    if (!t.px && !t.py)
    {
        sTouch = kNoTouch;
    }
    else if (sTouch == kNoTouch)
    {
        // A press on a button zooms once; anywhere else it grabs the map.
        if (Inside(kZoomIn, t))
        {
            sTouch = kZoomInHeld;
            sZoom = sZoom + 1 < kZoomCount ? sZoom + 1 : sZoom;
        }
        else if (Inside(kZoomOut, t))
        {
            sTouch = kZoomOutHeld;
            sZoom = sZoom > 0 ? sZoom - 1 : sZoom;
        }
        else
        {
            sTouch = kDrag;
            sGrabX = t.px;
            sGrabY = t.py;
            sGrabPanX = sPanX;
            sGrabPanY = sPanY;
        }
    }
    else if (sTouch == kDrag)
    {
        sPanX = sGrabPanX - (t.px - sGrabX) / (f32)kZooms[sZoom];
        sPanY = sGrabPanY - (t.py - sGrabY) / (f32)kZooms[sZoom];
    }
    sPanX = ClampPan(sPanX, kScreenW);
    sPanY = ClampPan(sPanY, kScreenH);
    if (sTouch == kDrag)
    {
        // Held against a limit, the map follows the stylus again as soon as it turns back.
        sGrabPanX = sPanX + (t.px - sGrabX) / (f32)kZooms[sZoom];
        sGrabPanY = sPanY + (t.py - sGrabY) / (f32)kZooms[sZoom];
    }
    u32 h = sig;
    const auto mix = [&h](u32 v) { h = (h ^ v) * 16777619u; };
    mix((u32)PanLeft());
    mix((u32)PanTop());
    mix((u32)sZoom);
    mix((u32)sTouch);
    return h ? h : 1;
}

void DrawPause(C3DRenderer&, HudTheme::Sink& sink)
{
    const s32 left = PanLeft(), top = PanTop(), zoom = kZooms[sZoom];
    const f32 k = zoom * 0.25f;
    DrawGround(sink, 0, 0, kScreenW, kScreenH, left, top, zoom);
    DrawMarks(sink, 0, 0, kScreenW, kScreenH, (f32)left, (f32)top, k, kScreenW * 0.5f, kScreenH * 0.5f);
    DrawArrow(sink, left + sView.px4 * k, top + sView.py4 * k, sView.heading);
    DrawButton(sink, kZoomIn, sPlus, sZoom + 1 < kZoomCount, sTouch == kZoomInHeld);
    DrawButton(sink, kZoomOut, sMinus, sZoom > 0, sTouch == kZoomOutHeld);
}

void Shutdown(C3DRenderer& renderer)
{
    renderer.FreeTexture(sMap);
    renderer.FreeTexture(sEdges);
    renderer.FreeTexture(sFill);
    renderer.FreeTexture(sAtlas);
    renderer.FreeTexture(sSprites);
    sMap = sEdges = sFill = sAtlas = sSprites = 0;
    sCity = 0;
    sMapOk = false;
    sEdgeSig = 0;
    sFillGang = 0;
    std::vector<u8>().swap(sOwners);
}
} // namespace Minimap
