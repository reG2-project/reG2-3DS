#include "c3d_renderer.hpp"
#include "c3d_context.hpp"
#include "fps_overlay.hpp"
#include "stats_3ds.hpp"
#include "../src/prof.hpp"
#include "../src/hud_screen.hpp"
#include "hud_theme.hpp"
#include "minimap.hpp"
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// render.v.pica assembled by picasso and embedded by bin2s.
extern "C" const u8 render_shbin[];
extern "C" const u8 render_shbin_end[];

struct AtlasPage;

// The game's texture: a citro3d texture padded to a power of two, u and v scaled to the used part. In an atlas page's
// slot, mTex is the page's and u and v are moved to the slot.
struct RTexture
{
    C3D_Tex mTex;
    bool mValid;
    f32 mScaleU;
    f32 mScaleV;
    f32 mOffsetU;
    f32 mOffsetV;
    AtlasPage* mPage;
    s32 mSlot;
    u32 mUsedFrame; // last frame that drew it, 0 for none
    bool mPremultiplied; // colours times alpha: blended with ONE, 1 - alpha
    u8 mLevels;          // mip levels after the first
    u32 mVersion;        // counts the uploads, for the bottom screen's signature
    u32 mBytes;          // linear memory of all its levels
    bool mEvictable;     // its owner uploads it again when Evicted
    bool mEvicted;       // storage dropped to make room, or an upload that did not fit (in mFailedFrame)
    u32 mFailedFrame;
    RTexture* mLruPrev;
    RTexture* mLruNext;
};

namespace
{
// What the vertex shader loads (v0 position and rhw, v1 colour bytes, v2 uv, v3 world position and shade).
struct C3DVertex
{
    f32 x, y, rhw;
    u32 rgba;
    f32 u, v;
    f32 wx, wy, wz, shade; // shade < 0: unlit
};

const s32 kMaxVerts = 65536;
// The most lights the vertex shader takes (render.v.pica's lights[]).
const s32 kMaxShaderLights = 44;
// The top screen's target, landscape: the game's 800x480 at half size.
const s32 kScreenW = 400;
const s32 kScreenH = 240;
const s32 kBottomW = 320;
const s32 kBottomH = 240;
const s32 kMaxTexSize = 1024;
// The bottom screen is drawn at most every that many frames, and only when what it shows changed.
const u32 kBottomEvery = 2;
// GLRenderer's default vertex offset: D3D pixel centres are on integers, the PICA's on halves.
const f32 kPixelOffset = 0.5f - 1.0f / 2048.0f;
// Batch state only: the batch's texture is premultiplied; its draws sample the first mip level only.
const u32 kStatePremultiplied = 1u << 31;
const u32 kStateNoMip = 1u << 30;
// Batch state only: the texture is A8, drawn in the vertex colour with its alpha.
const u32 kStateAlphaTexture = 1u << 29;
// Linear memory for the game's textures, least recently drawn evicted beyond it. The rest of the 16 MB holds
// the vertices, command buffers, the port's own textures and some headroom. GTA2_3DS_TEXTURE_BUDGET=<KB> sets it.
const u32 kTextureBudget = 8192 * 1024;
const u32 kBatchStateMask = RS_ALPHA_TEST | RS_BLEND_ALPHA | RS_BLEND_ADD | RS_FILTER_LINEAR | RS_NO_TEXTURE;
// Evictable 64x64 textures with mip levels (tiles, 64 texel sprite slots) share atlas pages by format: one batch for a
// run of them. Each level's border repeats its edges (a texel at 8x8), so filtering sees a clamped texture of its own.
const s32 kAtlasSize = 512;
const s32 kSlotSize = 64;
const s32 kSlotLevels = 3;
const s32 kSlotBorder = kSlotSize >> kSlotLevels;
const s32 kSlotPitch = kSlotSize + 2 * kSlotBorder;
const s32 kSlotsPerRow = kAtlasSize / kSlotPitch;
const s32 kSlotsPerPage = kSlotsPerRow * kSlotsPerRow;

// Morton order of the low 3 bits of x inside an 8x8 tile; y's bits go one place higher.
const u8 kMorton[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};

s32 Pow2(s32 n)
{
    s32 p = 8;
    while (p < n)
    {
        p <<= 1;
    }
    return p;
}

// x / 255 rounded, for x up to 255 * 255, without a division (the ARM11 has none).
constexpr u32 Div255(u32 x)
{
    x += 128;
    return (x + (x >> 8)) >> 8;
}

// Colours times alpha, for blended textures.
u32 Premultiply(u32 c)
{
    const u32 a = c >> 24;
    if (a == 255 || a == 0)
    {
        return a ? c : 0;
    }
    return Div255((c & 0xFF) * a) | (Div255(((c >> 8) & 0xFF) * a) << 8) | (Div255(((c >> 16) & 0xFF) * a) << 16) | (a << 24);
}

// The RTexelBits of texels as stored: premultiplied when blended.
u32 ScanTexels(const u32* pRgba, size_t count, RMipmaps mipmaps)
{
    u32 bits = ~0u;
    for (size_t i = 0; i < count && (bits & (RTX_BINARY | RTX_4444)); i++)
    {
        bits &= TexelBits(mipmaps == RMIP_ALPHA ? Premultiply(pRgba[i]) : pRgba[i], mipmaps);
    }
    return bits;
}

// The smallest format that holds the texels exactly, else 16 bits with the alpha kept, else RGBA8.
GPU_TEXCOLOR PickFormat(u32 bits)
{
    if ((bits & (RTX_OPAQUE | RTX_565)) == (RTX_OPAQUE | RTX_565))
    {
        return GPU_RGB565;
    }
    if (bits & RTX_4444)
    {
        return GPU_RGBA4;
    }
    if (!(bits & RTX_BINARY))
    {
        return GPU_RGBA8;
    }
    return (bits & RTX_OPAQUE) ? GPU_RGB565 : GPU_RGBA5551;
}

// A channel rounded to the nearest value of `bits` bits, expanded back by replication as the GPU does.
constexpr u32 RoundChannel(u32 v, s32 bits)
{
    if (bits == 1)
    {
        return v >= 128 ? 255 : 0;
    }
    const u32 q = Div255(v * ((1u << bits) - 1));
    return (q << (8 - bits)) | (q >> (2 * bits - 8));
}

// RoundChannel for the formats' channel sizes: [bits][v], 255 for an absent channel (0 bits).
struct RoundTables
{
    u8 mRound[9][256];
    constexpr RoundTables() : mRound()
    {
        for (s32 bits : {0, 1, 4, 5, 6, 8})
        {
            for (u32 v = 0; v < 256; v++)
            {
                mRound[bits][v] = (u8)(bits == 8 ? v : bits == 0 ? 255 : RoundChannel(v, bits));
            }
        }
    }
};
constexpr RoundTables kRound;

// A 2x2 block averaged, each channel rounded by its table so the format's truncating writes are exact.
// Red and blue, then green and alpha, are summed two at a time in 16-bit lanes.
u32 Average(u32 a, u32 b, u32 c, u32 d, const u8* const* round)
{
    const u32 m = 0x00FF00FF;
    const u32 rb = (((a & m) + (b & m) + (c & m) + (d & m) + 0x00020002) >> 2) & m;
    const u32 ga = ((((a >> 8) & m) + ((b >> 8) & m) + ((c >> 8) & m) + ((d >> 8) & m) + 0x00020002) >> 2) & m;
    return round[0][rb & 0xFF] | (round[1][ga & 0xFF] << 8) | (round[2][rb >> 16] << 16) | ((u32)round[3][ga >> 16] << 24);
}

// The caller's texels as the first level: premultiplied if asked, cut to cw x ch (scaled down when larger),
// with the last column and row repeated out to the texture's size.
struct SourceLevel
{
    const u32* mRgba;
    s32 mWidth, mHeight, mCw, mCh;
    bool mPremultiply;

    const u32* Row(s32 y) const
    {
        const s32 cy = y < mCh ? y : mCh - 1;
        return mRgba + (size_t)(mCh == mHeight ? cy : (s32)((long long)cy * mHeight / mCh)) * mWidth;
    }
    u32 At(const u32* pRow, s32 x) const
    {
        const s32 cx = x < mCw ? x : mCw - 1;
        const u32 c = pRow[mCw == mWidth ? cx : (s32)((long long)cx * mWidth / mCw)];
        return mPremultiply ? Premultiply(c) : c;
    }
};

// A level made by Average, row by row.
struct ScratchLevel
{
    const u32* mTexels;
    s32 mWidth;

    const u32* Row(s32 y) const { return mTexels + (size_t)y * mWidth; }
    u32 At(const u32* pRow, s32 x) const { return pRow[x]; }
};

// A w x h level into the PICA's tiled layout, image row y at t = 1 - y / h (Append flips v), and the next
// level into pHalf when given. pHalf may be the source's storage: each average lands before what is still unread.
template <typename T, typename Conv, typename Source>
void WriteTiles(T* pDst, s32 w, s32 h, const Source& src, Conv conv, u32* pHalf, const u8* const* round)
{
    for (s32 y = 0; y < h; y += 2)
    {
        const u32* pRow0 = src.Row(y);
        const u32* pRow1 = src.Row(y + 1);
        T* pTiles = pDst + (size_t)(y >> 3) * (w >> 3) * 64 + ((u32)kMorton[y & 7] << 1);
        u32* pOut = pHalf ? pHalf + (size_t)(y / 2) * (w / 2) : 0;
        for (s32 x = 0; x < w; x += 2)
        {
            const u32 a = src.At(pRow0, x), b = src.At(pRow0, x + 1), c = src.At(pRow1, x), d = src.At(pRow1, x + 1);
            // A 2x2 block is 4 consecutive texels in Morton order.
            T* p = pTiles + (x >> 3) * 64 + kMorton[x & 7];
            p[0] = conv(a);
            p[1] = conv(b);
            p[2] = conv(c);
            p[3] = conv(d);
            if (pOut)
            {
                pOut[x / 2] = Average(a, b, c, d, round);
            }
        }
    }
}

// A level of a texture of format fmt, and the next one into pHalf when given.
template <typename Source>
void WriteLevel(void* pData, GPU_TEXCOLOR fmt, s32 w, s32 h, const Source& src, u32* pHalf)
{
    static const s8 kBits[][4] = {{8, 8, 8, 8}, {8, 8, 8, 0}, {5, 5, 5, 1}, {5, 6, 5, 0}, {4, 4, 4, 4}};
    const s8* bits = kBits[fmt <= GPU_RGBA4 ? fmt : 0];
    const u8* round[4] = {kRound.mRound[bits[0]], kRound.mRound[bits[1]], kRound.mRound[bits[2]], kRound.mRound[bits[3]]};
    switch (fmt)
    {
    case GPU_RGB565:
        WriteTiles((u16*)pData, w, h, src, [](u32 c) {
            return (u16)((((c >> 3) & 0x1F) << 11) | (((c >> 10) & 0x3F) << 5) | ((c >> 19) & 0x1F));
        }, pHalf, round);
        break;
    case GPU_RGBA4:
        WriteTiles((u16*)pData, w, h, src, [](u32 c) {
            return (u16)((((c >> 4) & 0xF) << 12) | (((c >> 12) & 0xF) << 8) | (((c >> 20) & 0xF) << 4) | (c >> 28));
        }, pHalf, round);
        break;
    case GPU_RGBA5551:
        WriteTiles((u16*)pData, w, h, src, [](u32 c) {
            return (u16)((((c >> 3) & 0x1F) << 11) | (((c >> 11) & 0x1F) << 6) | (((c >> 19) & 0x1F) << 1) | (c >> 31));
        }, pHalf, round);
        break;
    default:
        WriteTiles((u32*)pData, w, h, src, [](u32 c) { return __builtin_bswap32(c); }, pHalf, round);
        break;
    }
}

// A level of a slot's texels (tiled, size x size) into its place in a page level dstW texels wide, with its border.
template <typename T>
void CopySlot(T* pDst, s32 dstW, s32 x0, s32 y0, const T* pSrc, s32 size, s32 border)
{
    for (s32 y = -border; y < size + border; y++)
    {
        const s32 sy = y < 0 ? 0 : y < size ? y : size - 1;
        for (s32 x = -border; x < size + border; x++)
        {
            const s32 sx = x < 0 ? 0 : x < size ? x : size - 1;
            pDst[TiledIndex(x0 + border + x, y0 + border + y, dstW)] = pSrc[TiledIndex(sx, sy, size)];
        }
    }
}

// The draw maps texels 1:1 onto logical pixels, axis aligned and without perspective.
bool OneToOne(const RVertex* pVerts, s32 count, const RTexture* pTex)
{
    if (!pTex || count < 3 || pVerts[0].rhw != pVerts[1].rhw || pVerts[0].rhw != pVerts[2].rhw)
    {
        return false;
    }
    const f32 tw = pTex->mScaleU * pTex->mTex.width, th = pTex->mScaleV * pTex->mTex.height;
    const f32 x1 = pVerts[1].x - pVerts[0].x, y1 = pVerts[1].y - pVerts[0].y;
    const f32 x2 = pVerts[2].x - pVerts[0].x, y2 = pVerts[2].y - pVerts[0].y;
    const f32 u1 = (pVerts[1].u - pVerts[0].u) * tw, v1 = (pVerts[1].v - pVerts[0].v) * th;
    const f32 u2 = (pVerts[2].u - pVerts[0].u) * tw, v2 = (pVerts[2].v - pVerts[0].v) * th;
    const f32 det = fabsf(x1 * y2 - x2 * y1);
    if (det < 1.0f)
    {
        return false;
    }
    // Texels per logical pixel along x and y, times |det| (the ARM11 divides slowly).
    const f32 dudx = fabsf(u1 * y2 - u2 * y1), dudy = fabsf(u2 * x1 - u1 * x2);
    const f32 dvdx = fabsf(v1 * y2 - v2 * y1), dvdy = fabsf(v2 * x1 - v1 * x2);
    const f32 e = 0.01f * det;
    const bool straight = fabsf(dudx - det) < e && dudy < e && dvdx < e && fabsf(dvdy - det) < e;
    const bool turned = dudx < e && fabsf(dudy - det) < e && fabsf(dvdx - det) < e && dvdy < e;
    return straight || turned;
}

struct ScreenRect
{
    s32 x0, y0, x1, y1;
    bool operator!=(const ScreenRect& o) const { return x0 != o.x0 || y0 != o.y0 || x1 != o.x1 || y1 != o.y1; }
};

// A part of a HUD group on the bottom screen: draws centred in the layout rect `from` (640x480 layout pixels)
// move by (dx, dy) and are cut to `slot`. A scrolling part taller than its slot scrolls through it.
struct HudRule
{
    ScreenRect from;
    s32 dx, dy;
    ScreenRect slot;
    bool scroll;
    HudTheme::Part panel;
};

struct HudGroupRules
{
    const HudRule* rules; // the last one takes what the others leave
    s32 count;
};

// The bottom screen: gang bars and pager on the left, the status row along the top, wanted level, power-ups and
// weapon below it, the brief (scrolling when long) bottom left and the minimap in its frame bottom right; each part
// in its panel (HudTheme::Part). Without the minimap the brief takes the whole bottom row.
const HudTheme::Rect kHudPanels[HudTheme::kPartCount] = {
    {2, 2, 102, 80},     // kGangs
    {2, 84, 102, 124},   // kPager
    {106, 2, 318, 50},   // kStatus
    {106, 54, 236, 124}, // kWanted
    {240, 54, 318, 124}, // kWeapon
    {2, 128, 206, 238},  // kBrief
};
const s16 kBriefRightNoMinimap = 318;
const HudTheme::Rect kMinimapFrame = {208, 128, 318, 238};
// GTA2_3DS_MINIMAP=wide: the whole bottom row while no brief shows.
const HudTheme::Rect kMinimapWideFrame = {2, 128, 318, 238};

enum MinimapMode
{
    kMinimapOn,
    kMinimapWide,
    kMinimapOff,
};
const ScreenRect kAll = {0, 0, 640, 480};
const HudRule kStatsRules[] = {
    {{560, 44, 640, 100}, -324, 14, {240, 54, 318, 124}, false, HudTheme::kWeapon}, // weapon and ammo
    {{0, 100, 640, 480}, -407, -25, {106, 54, 236, 124}, false, HudTheme::kWanted}, // power-ups
    {{400, 0, 492, 44}, -334, 3, {106, 2, 318, 50}, false, HudTheme::kStatus},     // clock
    {kAll, -326, 3, {106, 2, 318, 50}, false, HudTheme::kStatus},                  // multiplier, lives, money, health
};
const HudRule kWantedRules[] = {{kAll, -149, 57, {106, 54, 236, 124}, false, HudTheme::kWanted}};
const HudRule kGangRules[] = {{kAll, 0, 0, {2, 2, 102, 80}, false, HudTheme::kGangs}};
const HudRule kBriefRules[] = {
    {{0, 0, 60, 480}, 0, -244, {2, 128, 64, 238}, false, HudTheme::kBrief}, // the face
    {kAll, 0, -244, {64, 130, 206, 236}, true, HudTheme::kBrief},
};
const HudRule kPagerRules[] = {{kAll, 21, -2, {0, 80, 104, 128}, false, HudTheme::kPager}};

#define HUD_RULES(rules) {rules, (s32)(sizeof(rules) / sizeof(rules[0]))}
const HudGroupRules kHudGroups[PortHudScreen::kGroupCount] = {
    {0, 0}, // kWorld: the top screen
    HUD_RULES(kStatsRules),
    HUD_RULES(kWantedRules),
    HUD_RULES(kGangRules),
    HUD_RULES(kBriefRules),
    HUD_RULES(kPagerRules),
};
#undef HUD_RULES

const HudRule* PickRule(const HudGroupRules& group, const RVertex* pVerts, s32 count)
{
    f32 x = 0.0f, y = 0.0f;
    for (s32 i = 0; i < count; i++)
    {
        x += pVerts[i].x;
        y += pVerts[i].y;
    }
    x /= count;
    y /= count;
    for (s32 i = 0; i < group.count - 1; i++)
    {
        const ScreenRect& r = group.rules[i].from;
        if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1)
        {
            return &group.rules[i];
        }
    }
    return &group.rules[group.count - 1];
}

// A scrolling part is shown from its top, scrolled to its end and held at both ends, over and over.
const f32 kScrollHoldMs = 2000.0f;
const f32 kScrollPxPerMs = 0.012f;

s32 Round(f32 v)
{
    return (s32)(v < 0.0f ? v - 0.5f : v + 0.5f);
}
} // namespace

struct AtlasPage
{
    RTexture mTex;
    bool mTaken[kSlotsPerPage];
    u32 mFreedFrame[kSlotsPerPage]; // a slot freed in the current frame waits for the next
    s32 mUsed;
};

struct C3DRenderer::Impl : HudTheme::Sink
{
    C3D_RenderTarget* mTarget;
    DVLB_s* mDvlb;
    shaderProgram_s mProgram;
    s32 mUniProjection;
    s32 mUniAmbient, mUniLights, mUniLightLoop, mUniAnyLights;
    // The batch has vertices lit in the shader.
    bool mBatchLit;
    // Subtracted from the world positions for the shader, whose floats keep 16 mantissa bits: near the lights.
    f32 mLightOriginX, mLightOriginY;
    C3D_Mtx mProjection;
    // Logical to screen: screen = (logical - origin) * scale.
    f32 mOriginX, mOriginY, mScale;
    ScreenRect mLogicalRect;
    ScreenRect mClip;

    C3DVertex* mVerts; // linear memory, refilled every frame
    s32 mVertCount;
    s32 mBatchStart;
    RTexture* mBatchTex;
    u32 mBatchState;
    ScreenRect mBatchScissor;

    bool mStateValid;
    u32 mAppliedState;
    ScreenRect mAppliedScissor;
    C3D_Tex* mBoundTex;
    u32 mBoundParam;
    u32 mBoundLod;

    // Set while the group being drawn goes to the bottom screen: its batches wait for the end of the frame.
    const HudGroupRules* mHudGroup;
    const HudRule* mBatchRule;
    struct HudBatch
    {
        s32 start, count;
        RTexture* tex;
        u32 state;
        ScreenRect scissor;
        bool scroll;
    };
    std::vector<HudBatch> mHudBatches;
    u32 mHudPanels; // a bit per HudTheme::Part drawn in this frame
    // The scrolling part's top this frame (bottom screen pixels) and its slot's; how much of it was hidden above
    // the slot when its scroll began, and when.
    s32 mScrollTop, mScrollSlotTop;
    bool mScrollSeen;
    s32 mScrollOverflow;
    u64 mScrollStartMs;
    // The signature of the bottom screen's last drawn frame, and that frame (0 before the first).
    u32 mBottomSig;
    u32 mBottomFrame;
    C3D_RenderTarget* mBottom;
    C3D_Mtx mBottomProjection;
    // The layout: kHudPanels and kHudGroups, with the brief's right edge for the minimap's mode.
    MinimapMode mMinimap;
    HudTheme::Rect mPanels[HudTheme::kPartCount];
    HudRule mBriefRules[sizeof(kBriefRules) / sizeof(kBriefRules[0])];
    HudGroupRules mGroups[PortHudScreen::kGroupCount];
    // The target drawn on, landscape.
    s32 mTargetW, mTargetH;

    bool mInFrame;
    bool mFrameDrew;
    bool mNeedClear;
    u32 mFrame;
    // Textures the GPU may still read: freed once the next frame has begun (C3D_FrameBegin waits for the GPU).
    std::vector<C3D_Tex> mPendingFree;
    // Textures with storage and their bytes in linear memory; evictions and allocations that did not fit.
    u32 mTexCount, mTexBytes, mEvictions, mAllocFails;
    // Evictable textures with storage, least recently drawn first, their bytes and the budget for them.
    RTexture* mLruHead;
    RTexture* mLruTail;
    u32 mCachedBytes;
    u32 mBudget;
    RTexture* mWhite;
    RTexture* mSurface;
    std::vector<u32> mLevel; // UploadTexture's next mip level being made
    std::vector<AtlasPage*> mPages;
    std::vector<u32> mSlotTexels; // a slot's levels before they go to its page
    bool mWarnedVerts, mWarnedCmdBuf, mWarnedAlloc;
    C3DRenderer* mOwner;

    ScreenRect ToScreen(f32 left, f32 top, f32 right, f32 bottom) const;
    void BeginFrame();
    void Append(const RVertex* pVerts, s32 count, RTexture* pTex, u32 state, f32 offset, const ScreenRect& scissor, const HudRule* pRule);
    void Flush();
    void SetLights(const RLight* pLights, s32 count, const f32* pAmbient, f32 unlitAmbient);
    f32 ScrollOffset();
    u32 BottomSignature(f32 scroll, u32 minimap) const;
    void DrawHud();
    void DrawOverlay();
    void DrawQuads(const HudTheme::Quad* pQuads, s32 count, RTexture* pTex, u32 state) override;
    void DrawTriangles(const RVertex* pVerts, s32 count, RTexture* pTex, u32 state) override;
    void ApplyState(u32 state, const ScreenRect& scissor);
    void Draw(RTexture* pTex, u32 state, const ScreenRect& scissor, s32 first, s32 count);
    bool Allocate(RTexture* pTex, s32 tw, s32 th, GPU_TEXCOLOR fmt, s32 levels, bool clear);
    bool AllocateSlot(RTexture* pTex, GPU_TEXCOLOR fmt);
    bool FindSlot(RTexture* pTex, GPU_TEXCOLOR fmt);
    bool AddPage(GPU_TEXCOLOR fmt);
    void WriteSlot(RTexture* pTex, const u8* pTexels);
    void Release(RTexture* pTex);
    void LruRemove(RTexture* pTex);
    void LruAppend(RTexture* pTex);
    bool EvictOne();
};

void C3DRenderer::Impl::LruRemove(RTexture* pTex)
{
    (pTex->mLruPrev ? pTex->mLruPrev->mLruNext : mLruHead) = pTex->mLruNext;
    (pTex->mLruNext ? pTex->mLruNext->mLruPrev : mLruTail) = pTex->mLruPrev;
    pTex->mLruPrev = pTex->mLruNext = 0;
    mCachedBytes -= pTex->mBytes;
}

void C3DRenderer::Impl::LruAppend(RTexture* pTex)
{
    pTex->mLruPrev = mLruTail;
    pTex->mLruNext = 0;
    (mLruTail ? mLruTail->mLruNext : mLruHead) = pTex;
    mLruTail = pTex;
    mCachedBytes += pTex->mBytes;
}

// Frees the least recently drawn evictable texture, unless the GPU may still read it.
bool C3DRenderer::Impl::EvictOne()
{
    RTexture* pTex = mLruHead;
    if (!pTex || pTex->mUsedFrame == mFrame)
    {
        return false;
    }
    Release(pTex);
    pTex->mEvicted = true;
    pTex->mFailedFrame = 0;
    mEvictions++;
    return true;
}

static u32 BitsPerTexel(GPU_TEXCOLOR fmt)
{
    return fmt == GPU_RGBA8 ? 32 : fmt == GPU_RGB8 ? 24 : fmt == GPU_A8 || fmt == GPU_L8 || fmt == GPU_LA4 ? 8 : 16;
}

// Drops the texture's storage, once the GPU is done with it.
void C3DRenderer::Impl::Release(RTexture* pTex)
{
    if (!pTex->mValid)
    {
        return;
    }
    if (pTex->mEvictable)
    {
        LruRemove(pTex);
    }
    pTex->mValid = false;
    if (AtlasPage* pPage = pTex->mPage)
    {
        pPage->mTaken[pTex->mSlot] = false;
        pPage->mFreedFrame[pTex->mSlot] = pTex->mUsedFrame;
        pPage->mUsed--;
        pTex->mPage = 0;
        return;
    }
    if (mBoundTex == &pTex->mTex)
    {
        mBoundTex = 0;
    }
    mTexCount--;
    mTexBytes -= pTex->mBytes;
    if (pTex->mUsedFrame == mFrame)
    {
        mPendingFree.push_back(pTex->mTex);
    }
    else
    {
        C3D_TexDelete(&pTex->mTex);
    }
}

static u32 LevelsBytes(s32 size, s32 levels, GPU_TEXCOLOR fmt)
{
    u32 bytes = 0;
    for (s32 l = 0; l <= levels; l++)
    {
        bytes += (u32)(size >> l) * (u32)(size >> l) * BitsPerTexel(fmt) / 8;
    }
    return bytes;
}

// A free slot for pTex in a page of that format, unless the GPU may still read what was there.
bool C3DRenderer::Impl::FindSlot(RTexture* pTex, GPU_TEXCOLOR fmt)
{
    for (size_t i = 0; i < mPages.size(); i++)
    {
        AtlasPage* pPage = mPages[i];
        if (pPage->mTex.mTex.fmt != fmt || pPage->mUsed == kSlotsPerPage)
        {
            continue;
        }
        for (s32 slot = 0; slot < kSlotsPerPage; slot++)
        {
            if (!pPage->mTaken[slot] && pPage->mFreedFrame[slot] != mFrame)
            {
                pPage->mTaken[slot] = true;
                pPage->mUsed++;
                pTex->mPage = pPage;
                pTex->mSlot = slot;
                return true;
            }
        }
    }
    return false;
}

bool C3DRenderer::Impl::AddPage(GPU_TEXCOLOR fmt)
{
    AtlasPage* pPage = new AtlasPage();
    memset(pPage, 0, sizeof(*pPage));
    const C3D_TexInitParams params = {(u16)kAtlasSize, (u16)kAtlasSize, (u8)kSlotLevels, fmt, GPU_TEX_2D, false};
    if (!C3D_TexInitWithParams(&pPage->mTex.mTex, NULL, params))
    {
        delete pPage;
        return false;
    }
    C3D_TexSetWrap(&pPage->mTex.mTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    pPage->mTex.mValid = true;
    pPage->mTex.mLevels = (u8)kSlotLevels;
    pPage->mTex.mBytes = LevelsBytes(kAtlasSize, kSlotLevels, fmt);
    mTexCount++;
    mTexBytes += pPage->mTex.mBytes;
    mPages.push_back(pPage);
    return true;
}

// A slot for new texels, as Allocate: in the cache, each slot counts for its share of its page.
bool C3DRenderer::Impl::AllocateSlot(RTexture* pTex, GPU_TEXCOLOR fmt)
{
    if (pTex->mValid)
    {
        if (pTex->mPage && pTex->mTex.fmt == fmt && pTex->mUsedFrame != mFrame)
        {
            return true;
        }
        Release(pTex);
    }
    const u32 bytes = LevelsBytes(kAtlasSize, kSlotLevels, fmt) / kSlotsPerPage;
    while (pTex->mEvictable && mCachedBytes + bytes > mBudget && EvictOne())
    {
    }
    while (!FindSlot(pTex, fmt))
    {
        if (!AddPage(fmt) && !EvictOne())
        {
            mAllocFails++;
            pTex->mEvicted = true;
            pTex->mFailedFrame = mFrame;
            return false;
        }
    }
    const s32 x = pTex->mSlot % kSlotsPerRow * kSlotPitch + kSlotBorder;
    const s32 y = pTex->mSlot / kSlotsPerRow * kSlotPitch + kSlotBorder;
    pTex->mTex = pTex->mPage->mTex.mTex;
    pTex->mScaleU = pTex->mScaleV = (f32)kSlotSize / kAtlasSize;
    pTex->mOffsetU = (f32)x / kAtlasSize;
    pTex->mOffsetV = (f32)y / kAtlasSize;
    pTex->mLevels = (u8)kSlotLevels;
    pTex->mBytes = bytes;
    pTex->mValid = true;
    pTex->mUsedFrame = 0;
    pTex->mEvicted = false;
    LruAppend(pTex);
    return true;
}

// pTexels: the slot's levels one after the other, tiled.
void C3DRenderer::Impl::WriteSlot(RTexture* pTex, const u8* pTexels)
{
    AtlasPage* pPage = pTex->mPage;
    C3D_Tex* pPageTex = &pPage->mTex.mTex;
    const u32 bytes = BitsPerTexel(pPageTex->fmt) / 8;
    const s32 x0 = pTex->mSlot % kSlotsPerRow * kSlotPitch;
    const s32 y0 = pTex->mSlot / kSlotsPerRow * kSlotPitch;
    for (s32 l = 0; l <= kSlotLevels; l++)
    {
        const s32 size = kSlotSize >> l, border = kSlotBorder >> l, w = kAtlasSize >> l;
        u8* pLevel = (u8*)C3D_Tex2DGetImagePtr(pPageTex, l, NULL);
        if (bytes == 2)
        {
            CopySlot((u16*)pLevel, w, x0 >> l, y0 >> l, (const u16*)pTexels, size, border);
        }
        else
        {
            CopySlot((u32*)pLevel, w, x0 >> l, y0 >> l, (const u32*)pTexels, size, border);
        }
        pTexels += (size_t)size * size * bytes;
        // The rows of 8x8 tiles the slot touches.
        const u32 rowBytes = (u32)w * 8 * bytes;
        const u32 first = (u32)(y0 >> l) / 8, last = (u32)((y0 >> l) + size + 2 * border - 1) / 8;
        GSPGPU_FlushDataCache(pLevel + first * rowBytes, (last - first + 1) * rowBytes);
    }
    pPage->mTex.mVersion++;
}

// Storage for new texels: kept when the GPU is done with it and it has the size and format, else replaced.
bool C3DRenderer::Impl::Allocate(RTexture* pTex, s32 tw, s32 th, GPU_TEXCOLOR fmt, s32 levels, bool clear)
{
    if (pTex->mValid)
    {
        const bool inFlight = pTex->mUsedFrame == mFrame;
        pTex->mTex.maxLevel = pTex->mLevels;
        if (!inFlight && pTex->mTex.width == tw && pTex->mTex.height == th && pTex->mTex.fmt == fmt && pTex->mLevels == levels)
        {
            return true;
        }
        Release(pTex);
    }
    if (mBoundTex == &pTex->mTex)
    {
        mBoundTex = 0;
    }
    const C3D_TexInitParams params = {(u16)tw, (u16)th, (u8)levels, fmt, GPU_TEX_2D, false};
    u32 bytes = 0;
    for (s32 l = 0; l <= levels; l++)
    {
        bytes += (u32)(tw >> l) * (u32)(th >> l) * BitsPerTexel(fmt) / 8;
    }
    while (pTex->mEvictable && mCachedBytes + bytes > mBudget && EvictOne())
    {
    }
    bool fits;
    // Should no free block be big enough, older textures go until one is.
    while (!(fits = C3D_TexInitWithParams(&pTex->mTex, NULL, params)) && EvictOne())
    {
    }
    if (!fits)
    {
        mAllocFails++;
        pTex->mEvicted = true;
        pTex->mFailedFrame = mFrame;
        if (!mWarnedAlloc)
        {
            fprintf(stderr, "c3d: texture %dx%d (format %d) does not fit in linear memory\n", (int)tw, (int)th, (int)fmt);
            mWarnedAlloc = true;
        }
        return false;
    }
    if (clear)
    {
        memset(pTex->mTex.data, 0, pTex->mTex.size);
    }
    C3D_TexSetWrap(&pTex->mTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    pTex->mOffsetU = pTex->mOffsetV = 0.0f;
    pTex->mLevels = (u8)levels;
    pTex->mBytes = bytes;
    pTex->mValid = true;
    pTex->mUsedFrame = 0;
    pTex->mEvicted = false;
    mTexCount++;
    mTexBytes += pTex->mBytes;
    if (pTex->mEvictable)
    {
        LruAppend(pTex);
    }
    return true;
}

ScreenRect C3DRenderer::Impl::ToScreen(f32 left, f32 top, f32 right, f32 bottom) const
{
    ScreenRect r;
    r.x0 = Round((left - mOriginX) * mScale);
    r.y0 = Round((top - mOriginY) * mScale);
    r.x1 = Round((right - mOriginX) * mScale);
    r.y1 = Round((bottom - mOriginY) * mScale);
    r.x0 = r.x0 > 0 ? r.x0 : 0;
    r.y0 = r.y0 > 0 ? r.y0 : 0;
    r.x1 = r.x1 < kScreenW ? r.x1 : kScreenW;
    r.y1 = r.y1 < kScreenH ? r.y1 : kScreenH;
    return r;
}

void C3DRenderer::Impl::BeginFrame()
{
    if (mInFrame)
    {
        return;
    }
    const u64 t0 = svcGetSystemTick();
    // GTA2_UNCAPPED (frame_loop.cpp): without waiting for the screens' refresh, frames the screens never show.
    static const bool sUncapped = getenv("GTA2_UNCAPPED") != NULL;
    // C3D_FRAME_SYNCDRAW's two waits apart: the screens' next refresh, then the GPU.
    if (!sUncapped)
    {
        PORT_PROF_EXCLUDED(kWaitVsync);
        C3D_FrameSync();
    }
    {
        PORT_PROF_EXCLUDED(kWaitGpu);
        C3D_FrameBegin(0);
    }
    Stats3DS_AddFrameBegin(svcGetSystemTick() - t0);
    // Those of the previous frame, now over.
    PORT_PROF_ADD_MS(kGpu, C3D_GetDrawingTime());
    PORT_PROF_ADD_MS(kC3dCpu, C3D_GetProcessingTime());
    mInFrame = true;
    mFrameDrew = false;
    mFrame++;
    for (size_t i = 0; i < mPendingFree.size(); i++)
    {
        C3D_TexDelete(&mPendingFree[i]);
    }
    mPendingFree.clear();
    for (size_t i = 0; i < mPages.size(); i++)
    {
        if (!mPages[i]->mUsed)
        {
            // citro3d keeps the bound pointer: never leave it dangling.
            C3D_TexBind(0, &mWhite->mTex);
            Release(&mPages[i]->mTex);
            delete mPages[i];
            mPages.erase(mPages.begin() + i--);
        }
    }
    if (mNeedClear)
    {
        C3D_RenderTargetClear(mTarget, C3D_CLEAR_COLOR, 0x000000FF, 0);
        mNeedClear = false;
    }
    C3D_FrameDrawOn(mTarget);
    mTargetW = kScreenW;
    mTargetH = kScreenH;
    C3D_BindProgram(&mProgram);
    C3D_AttrInfo* pAttr = C3D_GetAttrInfo();
    AttrInfo_Init(pAttr);
    AttrInfo_AddLoader(pAttr, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(pAttr, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(pAttr, 2, GPU_FLOAT, 2);
    AttrInfo_AddLoader(pAttr, 3, GPU_FLOAT, 4);
    C3D_BufInfo* pBuf = C3D_GetBufInfo();
    BufInfo_Init(pBuf);
    BufInfo_Add(pBuf, mVerts, sizeof(C3DVertex), 4, 0x3210);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mUniProjection, &mProjection);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    mVertCount = 0;
    mBatchStart = 0;
    mBatchTex = 0;
    mStateValid = false;
    mBoundTex = 0;
}

void C3DRenderer::Impl::Append(const RVertex* pVerts, s32 count, RTexture* pTex, u32 state, f32 offset, const ScreenRect& scissor, const HudRule* pRule)
{
    if (pTex && pTex->mPremultiplied)
    {
        state |= kStatePremultiplied;
    }
    if (pTex && pTex->mTex.fmt == GPU_A8)
    {
        state |= kStateAlphaTexture;
    }
    if (!pRule && !(state & C3DRenderer::kStateNearest))
    {
        // Nearest sampling at half size would drop every other texel.
        state |= RS_FILTER_LINEAR;
    }
    state &= ~C3DRenderer::kStateNearest;
    if (pRule)
    {
        state |= kStateNoMip;
    }
    RTexture* pBind = pTex && pTex->mPage ? &pTex->mPage->mTex : pTex;
    if (mVertCount > mBatchStart && (pBind != mBatchTex || state != mBatchState || scissor != mBatchScissor || pRule != mBatchRule))
    {
        Flush();
    }
    if (mVertCount + count > kMaxVerts)
    {
        if (!mWarnedVerts)
        {
            fprintf(stderr, "c3d: more than %d vertices in a frame, the rest is dropped\n", (int)kMaxVerts);
            mWarnedVerts = true;
        }
        return;
    }
    mBatchTex = pBind;
    mBatchState = state;
    mBatchScissor = scissor;
    mBatchRule = pRule;
    mFrameDrew = true;
    const f32 dx = offset + (pRule ? pRule->dx : 0);
    const f32 dy = offset + (pRule ? pRule->dy : 0);
    const f32 su = pTex ? pTex->mScaleU : 0.0f;
    const f32 sv = pTex ? pTex->mScaleV : 0.0f;
    const f32 ou = pTex ? pTex->mOffsetU : 0.0f;
    const f32 ov = pTex ? 1.0f - pTex->mOffsetV : 1.0f;
    if (pTex)
    {
        if (pTex->mEvictable && pTex->mUsedFrame != mFrame)
        {
            LruRemove(pTex);
            LruAppend(pTex);
        }
        pTex->mUsedFrame = mFrame;
        pBind->mUsedFrame = mFrame;
    }
    C3DVertex* pDst = mVerts + mVertCount;
    for (s32 i = 0; i < count; i++)
    {
        const RVertex& s = pVerts[i];
        pDst[i].x = s.x + dx;
        pDst[i].y = s.y + dy;
        pDst[i].rhw = s.rhw;
        pDst[i].rgba = s.rgba;
        pDst[i].u = s.u * su + ou;
        pDst[i].v = ov - s.v * sv;
        pDst[i].wx = s.wx - mLightOriginX;
        pDst[i].wy = s.wy - mLightOriginY;
        pDst[i].wz = s.wz;
        pDst[i].shade = -1.0f;
        if (s.lit != 0.0f)
        {
            pDst[i].rgba = s.unlit;
            pDst[i].shade = s.shade;
            mBatchLit = true;
        }
    }
    if (pRule)
    {
        mHudPanels |= 1u << pRule->panel;
    }
    if (pRule && pRule->scroll)
    {
        for (s32 i = 0; i < count; i++)
        {
            const s32 y = (s32)pDst[i].y;
            mScrollTop = mScrollSeen && mScrollTop < y ? mScrollTop : y;
            mScrollSeen = true;
        }
        mScrollSlotTop = pRule->slot.y0;
    }
    mVertCount += count;
}

// render.v.pica's uniforms; ambient[0] + c * ambient[1] is gbh_api.cpp's Ambient with its rounding.
void C3DRenderer::Impl::SetLights(const RLight* pLights, s32 count, const f32* pAmbient, f32 unlitAmbient)
{
    if (mBatchLit)
    {
        Flush();
    }
    const f32 round = unlitAmbient > 0.0f ? 0.5f : 0.0f;
    const f32 k = unlitAmbient / 255.0f;
    const f32 a = 1.0f - unlitAmbient;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, mUniAmbient, pAmbient[0] * a + round, pAmbient[1] * a + round, pAmbient[2] * a + round, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, mUniAmbient + 1, pAmbient[0] * k, pAmbient[1] * k, pAmbient[2] * k, 0.0f);
    count = count < kMaxShaderLights ? count : kMaxShaderLights;
    C3D_BoolUnifSet(GPU_VERTEX_SHADER, mUniAnyLights, count > 0);
    if (count > 0)
    {
        mLightOriginX = floorf(pLights[0].x);
        mLightOriginY = floorf(pLights[0].y);
        C3D_IVUnifSet(GPU_VERTEX_SHADER, mUniLightLoop, count - 1, 0, 2, 0);
        C3D_FVec* pDst = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, mUniLights, 2 * count);
        for (s32 i = 0; i < count; i++)
        {
            const RLight& l = pLights[i];
            const f32 r = l.radius > 0.0f ? 1.0f / l.radius : 0.0f;
            // 2.5: 1 / gbh_api.cpp's kCeilingFade.
            pDst[2 * i] = FVec4_New(l.x - mLightOriginX, l.y - mLightOriginY, l.z, l.radius);
            pDst[2 * i + 1] = FVec4_New(l.r * r, l.g * r, l.b * r, l.ceiling * 2.5f);
        }
    }
    mStateValid = false;
}

void C3DRenderer::Impl::Flush()
{
    const s32 n = mVertCount - mBatchStart;
    mBatchLit = false;
    if (n <= 0)
    {
        return;
    }
    PORT_PROF(kFlush);
    if (mBatchRule)
    {
        const HudBatch batch = {mBatchStart, n, mBatchTex, mBatchState, mBatchScissor, mBatchRule->scroll};
        mHudBatches.push_back(batch);
        mBatchStart = mVertCount;
        return;
    }
    if (C3D_GetCmdBufUsage() > 0.97f)
    {
        if (!mWarnedCmdBuf)
        {
            fprintf(stderr, "c3d: command buffer full, draws dropped until the next frame\n");
            mWarnedCmdBuf = true;
        }
        mBatchStart = mVertCount;
        return;
    }
    Draw(mBatchTex, mBatchState, mBatchScissor, mBatchStart, n);
    mBatchStart = mVertCount;
}

// How far down the scrolling part is drawn this frame: from all of its overflow (its top shown) to none.
f32 C3DRenderer::Impl::ScrollOffset()
{
    const s32 overflow = mScrollSeen ? mScrollSlotTop - mScrollTop : 0;
    mScrollSeen = false;
    if (overflow <= 0)
    {
        mScrollOverflow = 0;
        return 0.0f;
    }
    const u64 now = osGetTime();
    if (overflow != mScrollOverflow)
    {
        mScrollOverflow = overflow;
        mScrollStartMs = now;
    }
    const f32 scrollMs = overflow / kScrollPxPerMs;
    const f32 t = fmodf((f32)(now - mScrollStartMs), kScrollHoldMs * 2.0f + scrollMs) - kScrollHoldMs;
    const f32 scrolled = t < 0.0f ? 0.0f : (t > scrollMs ? scrollMs : t) * kScrollPxPerMs;
    return (f32)Round(overflow - scrolled);
}

// What the bottom screen would show: its panels, scroll and HUD batches, with their vertices and textures.
u32 C3DRenderer::Impl::BottomSignature(f32 scroll, u32 minimap) const
{
    u32 h = 2166136261u;
    const auto mix = [&h](u32 v) { h = (h ^ v) * 16777619u; };
    mix(mHudPanels);
    mix(minimap);
    mix((u32)(s32)scroll);
    for (size_t i = 0; i < mHudBatches.size(); i++)
    {
        const HudBatch& b = mHudBatches[i];
        mix((u32)(size_t)b.tex);
        mix(b.tex ? b.tex->mVersion : 0);
        mix(b.state);
        mix((u32)b.scissor.x0 | ((u32)b.scissor.y0 << 16));
        mix((u32)b.scissor.x1 | ((u32)b.scissor.y1 << 16));
        mix(b.scroll);
        const u32* pWords = reinterpret_cast<const u32*>(mVerts + b.start);
        for (size_t w = 0; w < b.count * (sizeof(C3DVertex) / 4); w++)
        {
            mix(pWords[w]);
        }
    }
    return h;
}

// The bottom screen, after the top one: cleared, with the HUD's batches of the frame. When nothing on it changed,
// or it was drawn less than kBottomEvery frames ago, it is left alone and keeps showing its last frame.
void C3DRenderer::Impl::DrawHud()
{
    PORT_PROF(kPresentBottom);
    const u64 t0 = svcGetSystemTick();
    const f32 scroll = ScrollOffset();
    if (!mBottom)
    {
        mHudBatches.clear();
        return;
    }
    // The minimap shows with the HUD, in game only.
    const u32 minimap = mHudPanels && mMinimap != kMinimapOff ? Minimap::Update(*mOwner) : 0;
    const bool wide = mMinimap == kMinimapWide && !(mHudPanels & (1u << HudTheme::kBrief));
    const u32 sig = BottomSignature(scroll, minimap ^ wide);
    if (mBottomFrame && (sig == mBottomSig || mFrame - mBottomFrame < kBottomEvery))
    {
        mHudBatches.clear();
        mHudPanels = 0;
        Stats3DS_AddBottom(svcGetSystemTick() - t0, false);
        return;
    }
    mBottomSig = sig;
    mBottomFrame = mFrame;
    C3D_RenderTargetClear(mBottom, C3D_CLEAR_COLOR, 0x000000FF, 0);
    C3D_FrameDrawOn(mBottom);
    mTargetW = kBottomW;
    mTargetH = kBottomH;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mUniProjection, &mBottomProjection);
    mStateValid = false;
    if (mHudPanels)
    {
        HudTheme::Draw(*mOwner, *this, mPanels, mHudPanels);
        if (minimap)
        {
            Minimap::Draw(*mOwner, *this, wide ? kMinimapWideFrame : kMinimapFrame);
        }
        Flush();
        mHudPanels = 0;
    }
    C3D_Mtx scrolled = mBottomProjection;
    Mtx_Translate(&scrolled, 0.0f, scroll, 0.0f, true);
    bool inScroll = false;
    for (size_t i = 0; i < mHudBatches.size(); i++)
    {
        const HudBatch& b = mHudBatches[i];
        if (C3D_GetCmdBufUsage() > 0.97f)
        {
            break;
        }
        if (b.scroll != inScroll)
        {
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mUniProjection, b.scroll ? &scrolled : &mBottomProjection);
            inScroll = b.scroll;
            mStateValid = false;
        }
        Draw(b.tex, b.state, b.scissor, b.start, b.count);
    }
    mHudBatches.clear();
    Stats3DS_AddBottom(svcGetSystemTick() - t0, true);
}

// The FPS overlay over the top screen's corner, texel for pixel.
void C3DRenderer::Impl::DrawOverlay()
{
    PORT_PROF(kPresentOverlay);
    s32 w, h;
    RTexture* pTex = FpsOverlay::Update(*mOwner, &w, &h);
    if (!pTex || !pTex->mValid)
    {
        return;
    }
    const f32 x0 = mOriginX + FpsOverlay::kX / mScale, y0 = mOriginY + FpsOverlay::kY / mScale;
    const f32 x1 = x0 + w / mScale, y1 = y0 + h / mScale;
    const f32 xs[4] = {x0, x1, x1, x0};
    const f32 ys[4] = {y0, y0, y1, y1};
    const f32 us[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    const f32 vs[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    static const s32 kCorners[6] = {0, 1, 2, 0, 2, 3};
    RVertex v[6];
    memset(v, 0, sizeof(v));
    for (s32 k = 0; k < 6; k++)
    {
        const s32 c = kCorners[k];
        v[k].x = xs[c];
        v[k].y = ys[c];
        v[k].rhw = 1.0f;
        v[k].rgba = 0xFFFFFFFFu;
        v[k].u = us[c];
        v[k].v = vs[c];
    }
    const ScreenRect screen = {0, 0, kScreenW, kScreenH};
    Append(v, 6, pTex, RS_BLEND_ALPHA | C3DRenderer::kStateNearest, 0.0f, screen, 0);
    Flush();
}

// The HUD theme's draws, under the HUD on the bottom screen.
void C3DRenderer::Impl::DrawQuads(const HudTheme::Quad* pQuads, s32 count, RTexture* pTex, u32 state)
{
    const ScreenRect bottom = {0, 0, kBottomW, kBottomH};
    if (!pTex || !pTex->mValid)
    {
        pTex = 0;
        state |= RS_NO_TEXTURE;
    }
    for (s32 i = 0; i < count; i++)
    {
        const HudTheme::Quad& q = pQuads[i];
        const f32 xs[4] = {q.x0, q.x1, q.x1, q.x0};
        const f32 ys[4] = {q.y0, q.y0, q.y1, q.y1};
        const f32 us[4] = {q.u0, q.u1, q.u1, q.u0};
        const f32 vs[4] = {q.v0, q.v0, q.v1, q.v1};
        static const s32 kCorners[6] = {0, 1, 2, 0, 2, 3};
        RVertex v[6];
        memset(v, 0, sizeof(v));
        for (s32 k = 0; k < 6; k++)
        {
            const s32 c = kCorners[k];
            v[k].x = xs[c];
            v[k].y = ys[c];
            v[k].rhw = 1.0f;
            v[k].rgba = q.rgba[c];
            v[k].u = us[c];
            v[k].v = vs[c];
        }
        Append(v, 6, pTex, state & (kBatchStateMask | C3DRenderer::kStateNearest), 0.0f, bottom, 0);
    }
}

void C3DRenderer::Impl::DrawTriangles(const RVertex* pVerts, s32 count, RTexture* pTex, u32 state)
{
    const ScreenRect bottom = {0, 0, kBottomW, kBottomH};
    if (!pTex || !pTex->mValid)
    {
        pTex = 0;
        state |= RS_NO_TEXTURE;
    }
    if (count > 0)
    {
        Append(pVerts, count, pTex, state & (kBatchStateMask | C3DRenderer::kStateNearest), 0.0f, bottom, 0);
    }
}

void C3DRenderer::Impl::ApplyState(u32 state, const ScreenRect& scissor)
{
    PORT_PROF_COUNT(kStates, 1);
    if (!mStateValid || scissor != mAppliedScissor)
    {
        // Mtx_OrthoTilt turns the screen: x runs down the rows of the target (400 on top), y across its columns.
        C3D_SetScissor(GPU_SCISSOR_NORMAL, mTargetH - scissor.y1, mTargetW - scissor.x1, mTargetH - scissor.y0, mTargetW - scissor.x0);
        mAppliedScissor = scissor;
    }
    if (!mStateValid || state != mAppliedState)
    {
        if (state & RS_BLEND_ADD)
        {
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
        }
        else if ((state & RS_BLEND_ALPHA) && (state & kStatePremultiplied))
        {
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
        }
        else if (state & RS_BLEND_ALPHA)
        {
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
        }
        else
        {
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        }
        C3D_AlphaTest((state & RS_ALPHA_TEST) != 0, GPU_GEQUAL, 128);
        C3D_TexEnv* pEnv = C3D_GetTexEnv(0);
        C3D_TexEnvInit(pEnv);
        if (state & RS_NO_TEXTURE)
        {
            C3D_TexEnvSrc(pEnv, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(pEnv, C3D_Both, GPU_REPLACE);
        }
        else if (state & kStateAlphaTexture)
        {
            C3D_TexEnvSrc(pEnv, C3D_RGB, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(pEnv, C3D_RGB, GPU_REPLACE);
            C3D_TexEnvSrc(pEnv, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(pEnv, C3D_Alpha, GPU_MODULATE);
        }
        else
        {
            C3D_TexEnvSrc(pEnv, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(pEnv, C3D_Both, GPU_MODULATE);
        }
        // A premultiplied texel's colour takes the vertex alpha as well.
        C3D_TexEnv* pEnv1 = C3D_GetTexEnv(1);
        C3D_TexEnvInit(pEnv1);
        if ((state & (RS_BLEND_ALPHA | kStatePremultiplied)) == (RS_BLEND_ALPHA | kStatePremultiplied))
        {
            C3D_TexEnvSrc(pEnv1, C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvOpRgb(pEnv1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR);
            C3D_TexEnvFunc(pEnv1, C3D_RGB, GPU_MODULATE);
        }
        mAppliedState = state;
    }
    mStateValid = true;
}

// A batch's draw. When only its texture differs from the last draw's, that texture's registers and the draw go
// straight into the command buffer, as citro3d writes them; C3D_DrawArrays would also go through every uniform.
void C3DRenderer::Impl::Draw(RTexture* pTex, u32 state, const ScreenRect& scissor, s32 first, s32 count)
{
    const bool sameState = mStateValid && state == mAppliedState && !(scissor != mAppliedScissor);
    if (!sameState)
    {
        ApplyState(state, scissor);
    }
    // Untextured draws keep a valid texture bound: citro3d reads the bound one whenever it is dirty.
    RTexture* pBind = pTex ? pTex : mWhite;
    C3D_Tex* pTex3 = &pBind->mTex;
    const GPU_TEXTURE_FILTER_PARAM filter = (state & RS_FILTER_LINEAR) ? GPU_LINEAR : GPU_NEAREST;
    C3D_TexSetFilter(pTex3, filter, filter);
    C3D_TexSetFilterMipmap(pTex3, GPU_LINEAR);
    pTex3->maxLevel = (state & kStateNoMip) ? 0 : pBind->mLevels;
    const bool rebind = mBoundTex != pTex3 || mBoundParam != pTex3->param || mBoundLod != pTex3->lodParam;
    if (rebind)
    {
        C3D_TexBind(0, pTex3);
        PORT_PROF_COUNT(kBinds, 1);
        mBoundTex = pTex3;
        mBoundParam = pTex3->param;
        mBoundLod = pTex3->lodParam;
    }
    mOwner->mDrawCalls++;
    mOwner->mVertices += (u32)count;
    PORT_PROF_COUNT(kDrawCalls, 1);
    PORT_PROF_COUNT(kVertices, (u32)count);
    if (!sameState)
    {
        C3D_DrawArrays(GPU_TRIANGLES, first, count);
        return;
    }
    // Texture unit 0 as C3Di_SetTex sets it, its cache cleared, then C3D_DrawArrays' commands.
    u32 cmds[32];
    u32* p = cmds;
    if (rebind)
    {
        const u32 tex[] = {pTex3->border, GPUCMD_HEADER(1, 0xF, GPUREG_TEXUNIT0_BORDER_COLOR) | (4 << 20),
                           pTex3->dim, pTex3->param, pTex3->lodParam, osConvertVirtToPhys(pTex3->data) >> 3,
                           (u32)pTex3->fmt, GPUCMD_HEADER(0, 0xF, GPUREG_TEXUNIT0_TYPE),
                           0x10000, GPUCMD_HEADER(0, 0x4, GPUREG_TEXUNIT_CONFIG)};
        memcpy(p, tex, sizeof(tex));
        p += sizeof(tex) / 4;
    }
    const u32 draw[] = {GPU_TRIANGLES, GPUCMD_HEADER(0, 0x2, GPUREG_PRIMITIVE_CONFIG),
                        1, GPUCMD_HEADER(0, 0xF, GPUREG_RESTART_PRIMITIVE),
                        0x80000000, GPUCMD_HEADER(0, 0xF, GPUREG_INDEXBUFFER_CONFIG),
                        (u32)count, GPUCMD_HEADER(0, 0xF, GPUREG_NUMVERTICES),
                        (u32)first, GPUCMD_HEADER(0, 0xF, GPUREG_VERTEX_OFFSET),
                        1, GPUCMD_HEADER(0, 0x1, GPUREG_GEOSTAGE_CONFIG2),
                        0, GPUCMD_HEADER(0, 0x1, GPUREG_START_DRAW_FUNC0),
                        1, GPUCMD_HEADER(0, 0xF, GPUREG_DRAWARRAYS),
                        1, GPUCMD_HEADER(0, 0x1, GPUREG_START_DRAW_FUNC0),
                        0, GPUCMD_HEADER(0, 0x1, GPUREG_GEOSTAGE_CONFIG2),
                        1, GPUCMD_HEADER(0, 0xF, GPUREG_VTX_FUNC)};
    memcpy(p, draw, sizeof(draw));
    p += sizeof(draw) / 4;
    GPUCMD_AddRawCommands(cmds, (u32)(p - cmds));
}

C3DRenderer::C3DRenderer() : mImpl(0), mLogicalW(640), mLogicalH(480), mDrawCalls(0), mUploads(0), mVertices(0)
{
}

C3DRenderer::~C3DRenderer()
{
    Shutdown();
}

bool C3DRenderer::Init(Loader, const char** ppError)
{
    if (mImpl)
    {
        return true;
    }
    C3D_RenderTarget* pTarget = C3DContextTarget();
    if (!pTarget)
    {
        *ppError = "no citro3d context";
        return false;
    }
    Impl* p = new Impl();
    p->mOwner = this;
    p->mTarget = pTarget;
    p->mBottom = C3DContextBottomTarget();
    const char* pMinimap = getenv("GTA2_3DS_MINIMAP");
    p->mMinimap = !pMinimap ? kMinimapOn : !strcmp(pMinimap, "off") ? kMinimapOff : !strcmp(pMinimap, "wide") ? kMinimapWide : kMinimapOn;
    memcpy(p->mPanels, kHudPanels, sizeof(kHudPanels));
    memcpy(p->mBriefRules, kBriefRules, sizeof(kBriefRules));
    memcpy(p->mGroups, kHudGroups, sizeof(kHudGroups));
    p->mGroups[PortHudScreen::kBrief].rules = p->mBriefRules;
    if (p->mMinimap == kMinimapOff)
    {
        p->mPanels[HudTheme::kBrief].x1 = kBriefRightNoMinimap;
        p->mBriefRules[1].slot.x1 = kBriefRightNoMinimap;
    }
    Mtx_OrthoTilt(&p->mBottomProjection, 0.0f, (f32)kBottomW, (f32)kBottomH, 0.0f, 0.0f, 1.0f, true);
    p->mDvlb = DVLB_ParseFile((u32*)render_shbin, (u32)(render_shbin_end - render_shbin));
    p->mVerts = (C3DVertex*)linearAlloc(kMaxVerts * sizeof(C3DVertex));
    if (!p->mDvlb || !p->mVerts)
    {
        if (p->mDvlb)
        {
            DVLB_Free(p->mDvlb);
        }
        if (p->mVerts)
        {
            linearFree(p->mVerts);
        }
        delete p;
        *ppError = "citro3d renderer: shader or vertex buffer allocation failed";
        return false;
    }
    shaderProgramInit(&p->mProgram);
    shaderProgramSetVsh(&p->mProgram, &p->mDvlb->DVLE[0]);
    p->mUniProjection = shaderInstanceGetUniformLocation(p->mProgram.vertexShader, "projection");
    p->mUniAmbient = shaderInstanceGetUniformLocation(p->mProgram.vertexShader, "ambient");
    p->mUniLights = shaderInstanceGetUniformLocation(p->mProgram.vertexShader, "lights");
    p->mUniLightLoop = shaderInstanceGetUniformLocation(p->mProgram.vertexShader, "lightLoop");
    p->mUniAnyLights = shaderInstanceGetUniformLocation(p->mProgram.vertexShader, "anyLights");
    p->mNeedClear = true;
    p->mFrame = 1;
    const char* pBudget = getenv("GTA2_3DS_TEXTURE_BUDGET");
    p->mBudget = pBudget ? (u32)atoi(pBudget) * 1024 : kTextureBudget;
    mImpl = p;

    const s32 n = 8;
    std::vector<u32> white(n * n, 0xFFFFFFFFu);
    p->mWhite = CreateTexture();
    UploadTexture(p->mWhite, n, n, &white[0], RMIP_NONE);
    p->mSurface = CreateTexture();
    // The colour key becomes 0: transparent black.
    p->mSurface->mPremultiplied = true;
    SetLogicalSize(mLogicalW, mLogicalH);
    return true;
}

void C3DRenderer::Shutdown()
{
    Impl* p = mImpl;
    if (!p)
    {
        return;
    }
    if (p->mInFrame)
    {
        C3D_FrameEnd(0);
        p->mInFrame = false;
    }
    HudTheme::Shutdown(*this);
    Minimap::Shutdown(*this);
    FpsOverlay::Shutdown(*this);
    FreeTexture(p->mSurface);
    p->mSurface = 0;
    FreeTexture(p->mWhite);
    p->mWhite = 0;
    for (size_t i = 0; i < p->mPages.size(); i++)
    {
        C3D_TexDelete(&p->mPages[i]->mTex.mTex);
        delete p->mPages[i];
    }
    for (size_t i = 0; i < p->mPendingFree.size(); i++)
    {
        C3D_TexDelete(&p->mPendingFree[i]);
    }
    shaderProgramFree(&p->mProgram);
    DVLB_Free(p->mDvlb);
    linearFree(p->mVerts);
    delete p;
    mImpl = 0;
}

void C3DRenderer::SetLogicalSize(s32 width, s32 height)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }
    Impl* p = mImpl;
    if (p)
    {
        mImpl->Flush();
    }
    mLogicalW = width;
    mLogicalH = height;
    if (!p)
    {
        return;
    }
    // Scaled to fit the top screen with the aspect kept: Mtx_OrthoTilt spans the whole screen in logical units.
    const f32 scale = (f32)kScreenW / width < (f32)kScreenH / height ? (f32)kScreenW / width : (f32)kScreenH / height;
    const f32 visibleW = kScreenW / scale;
    const f32 visibleH = kScreenH / scale;
    p->mScale = scale;
    p->mOriginX = (width - visibleW) * 0.5f;
    p->mOriginY = (height - visibleH) * 0.5f;
    Mtx_OrthoTilt(&p->mProjection, p->mOriginX, p->mOriginX + visibleW, p->mOriginY + visibleH, p->mOriginY, 0.0f, 1.0f, true);
    p->mLogicalRect = p->ToScreen(0.0f, 0.0f, (f32)width, (f32)height);
    p->mClip = p->mLogicalRect;
    p->mNeedClear = true;
    if (p->mInFrame)
    {
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, p->mUniProjection, &p->mProjection);
        p->mStateValid = false;
    }
}

void C3DRenderer::Clear(u8 r, u8 g, u8 b, s32 left, s32 top, s32 right, s32 bottom)
{
    if (!mImpl)
    {
        return;
    }
    if (left == 0 && top == 0 && right == 0 && bottom == 0)
    {
        right = mLogicalW;
        bottom = mLogicalH;
    }
    left = left > 0 ? left : 0;
    top = top > 0 ? top : 0;
    right = right < mLogicalW ? right : mLogicalW;
    bottom = bottom < mLogicalH ? bottom : mLogicalH;
    if (left >= right || top >= bottom)
    {
        return;
    }
    mImpl->BeginFrame();
    RVertex v[6];
    memset(v, 0, sizeof(v));
    const f32 xs[6] = {(f32)left, (f32)right, (f32)right, (f32)left, (f32)right, (f32)left};
    const f32 ys[6] = {(f32)top, (f32)top, (f32)bottom, (f32)top, (f32)bottom, (f32)bottom};
    for (s32 i = 0; i < 6; i++)
    {
        v[i].x = xs[i];
        v[i].y = ys[i];
        v[i].rhw = 1.0f;
        v[i].rgba = r | (g << 8) | (b << 16) | 0xFF000000u;
    }
    mImpl->Append(v, 6, 0, RS_NO_TEXTURE, 0.0f, mImpl->mLogicalRect, 0);
}

void C3DRenderer::CompositeSurface(const u16* pPixels, s32 pitchBytes, s32 width, s32 height, u16 key)
{
    Impl* p = mImpl;
    if (!p || !pPixels || width <= 0 || height <= 0)
    {
        return;
    }
    if (width > kMaxTexSize || height > kMaxTexSize)
    {
        return;
    }
    // Begun first: the GPU is then done with the previous frame's surface, which is rewritten in place.
    p->BeginFrame();
    RTexture* pTex = p->mSurface;
    if (!p->Allocate(pTex, Pow2(width), Pow2(height), GPU_RGBA5551, 0, true))
    {
        return;
    }
    mUploads++;
    pTex->mVersion++;
    u16* pData = (u16*)pTex->mTex.data;
    const s32 tilesPerRow = pTex->mTex.width >> 3;
    for (s32 y = 0; y < height; y++)
    {
        const u16* pRow = (const u16*)((const u8*)pPixels + (size_t)y * pitchBytes);
        u16* pTiles = pData + (size_t)(y >> 3) * tilesPerRow * 64;
        const u32 my = (u32)kMorton[y & 7] << 1;
        for (s32 x = 0; x < width; x++)
        {
            const u16 c = pRow[x];
            // RGB565 to RGB5A1: red and the top five green bits stay in place.
            pTiles[(x >> 3) * 64 + (kMorton[x & 7] | my)] = c == key ? 0 : (u16)((c & 0xFFC0) | ((c & 0x1F) << 1) | 1);
        }
    }
    C3D_TexFlush(&pTex->mTex);
    pTex->mScaleU = (f32)width / pTex->mTex.width;
    pTex->mScaleV = (f32)height / pTex->mTex.height;
    RVertex v[6];
    memset(v, 0, sizeof(v));
    const f32 xs[6] = {0.0f, (f32)width, (f32)width, 0.0f, (f32)width, 0.0f};
    const f32 ys[6] = {0.0f, 0.0f, (f32)height, 0.0f, (f32)height, (f32)height};
    for (s32 i = 0; i < 6; i++)
    {
        v[i].x = xs[i];
        v[i].y = ys[i];
        v[i].rhw = 1.0f;
        v[i].rgba = 0xFFFFFFFFu;
        v[i].u = xs[i] / width;
        v[i].v = ys[i] / height;
    }
    p->Append(v, 6, p->mSurface, RS_BLEND_ALPHA, 0.0f, p->mLogicalRect, 0);
}

void C3DRenderer::FinishFrame()
{
    if (mImpl)
    {
        mImpl->Flush();
    }
}

void C3DRenderer::Present(s32, s32)
{
    Impl* p = mImpl;
    if (!p)
    {
        return;
    }
    PORT_PROF(kPresent);
    // A frame without draws shows the targets again.
    mImpl->BeginFrame();
    mImpl->Flush();
    if (p->mFrameDrew)
    {
        p->DrawOverlay();
        p->DrawHud();
    }
    if (p->mVertCount > 0)
    {
        GSPGPU_FlushDataCache(p->mVerts, p->mVertCount * sizeof(C3DVertex));
    }
    const u64 t0 = svcGetSystemTick();
    {
        PORT_PROF(kPresentFrameEnd);
        C3D_FrameEnd(0);
    }
    p->mInFrame = false;
    Stats3DS_SetTextures(p->mTexCount, p->mTexBytes, p->mCachedBytes, p->mEvictions, p->mAllocFails);
    Stats3DS_Frame(svcGetSystemTick() - t0);
}

RTexture* C3DRenderer::CreateTexture()
{
    RTexture* pTex = new RTexture;
    memset(pTex, 0, sizeof(*pTex));
    return pTex;
}

void C3DRenderer::UploadTexture(RTexture* pTex, s32 width, s32 height, const u32* pRgba, RMipmaps mipmaps, u32 texels)
{
    Impl* p = mImpl;
    if (!p || !pTex || !pRgba || width <= 0 || height <= 0)
    {
        return;
    }
    if (pTex == p->mBatchTex || (pTex->mPage && &pTex->mPage->mTex == p->mBatchTex))
    {
        mImpl->Flush();
    }
    const s32 cw = width < kMaxTexSize ? width : kMaxTexSize;
    const s32 ch = height < kMaxTexSize ? height : kMaxTexSize;
    const s32 tw = Pow2(cw);
    const s32 th = Pow2(ch);
    PORT_PROF_EXCLUDED(kTexUpload);
    PORT_PROF_COUNT(kUploads, 1);
    const u64 t0 = svcGetSystemTick();
    // Partial alpha changes the colours when premultiplied, so only a scan tells their format.
    if (!(texels & RTX_KNOWN) || (mipmaps == RMIP_ALPHA && !(texels & RTX_BINARY)))
    {
        texels = ScanTexels(pRgba, (size_t)width * height, mipmaps);
    }
    // Blended textures are premultiplied: filtered, transparent texels lend no colour.
    const bool premultiply = mipmaps == RMIP_ALPHA && !(texels & RTX_OPAQUE);
    pTex->mPremultiplied = premultiply;
    GPU_TEXCOLOR fmt = PickFormat(texels);
    // The game's textures get mip levels down to 8x8, for the world drawn smaller than 1:1 on the top screen.
    const s32 levels = mipmaps == RMIP_NONE ? 0 : C3D_TexCalcMaxLevel(tw, th);
    if (levels > 0 && premultiply && fmt == GPU_RGBA5551)
    {
        // Averaged edges need more than one bit of alpha.
        fmt = GPU_RGBA4;
    }
    const bool slot = pTex->mEvictable && tw == kSlotSize && th == kSlotSize && levels == kSlotLevels;
    if (slot ? !p->AllocateSlot(pTex, fmt) : !p->Allocate(pTex, tw, th, fmt, levels, false))
    {
        return;
    }
    mUploads++;
    pTex->mVersion++;
    // One pass per level: each writes its texels and averages the next level into the scratch buffer.
    u32* pHalf = 0;
    if (levels > 0)
    {
        p->mLevel.resize((size_t)(tw / 2) * (th / 2));
        pHalf = &p->mLevel[0];
    }
    // A slot's levels are made one after the other, as in a texture of its own, then copied to its page.
    if (slot)
    {
        p->mSlotTexels.resize(LevelsBytes(kSlotSize, kSlotLevels, fmt) / 4);
    }
    u8* pLevel = slot ? (u8*)&p->mSlotTexels[0] : (u8*)pTex->mTex.data;
    const SourceLevel src = {pRgba, width, height, cw, ch, premultiply};
    WriteLevel(pLevel, fmt, tw, th, src, pHalf);
    for (s32 l = 1; l <= levels; l++)
    {
        const ScratchLevel level = {pHalf, tw >> l};
        const size_t lastBytes = (size_t)(tw >> (l - 1)) * (th >> (l - 1)) * BitsPerTexel(fmt) / 8;
        pLevel = slot ? pLevel + lastBytes : (u8*)C3D_Tex2DGetImagePtr(&pTex->mTex, l, NULL);
        WriteLevel(pLevel, fmt, tw >> l, th >> l, level, l < levels ? pHalf : 0);
    }
    if (slot)
    {
        p->WriteSlot(pTex, (const u8*)&p->mSlotTexels[0]);
    }
    else
    {
        C3D_TexFlush(&pTex->mTex);
        pTex->mScaleU = (f32)cw / tw;
        pTex->mScaleV = (f32)ch / th;
    }
    Stats3DS_AddUpload(svcGetSystemTick() - t0);
}

void C3DRenderer::SetEvictable(RTexture* pTex)
{
    if (!mImpl || !pTex || pTex->mEvictable)
    {
        return;
    }
    pTex->mEvictable = true;
    if (pTex->mValid)
    {
        mImpl->LruAppend(pTex);
    }
}

bool C3DRenderer::Evicted(RTexture* pTex) const
{
    return mImpl && pTex && pTex->mEvicted && pTex->mFailedFrame != mImpl->mFrame;
}

void* C3DRenderer::LockTiled(RTexture* pTex, s32 size, bool alphaOnly)
{
    Impl* p = mImpl;
    if (!p || !pTex || size < 8 || size > kMaxTexSize || (size & (size - 1)))
    {
        return 0;
    }
    if (pTex == p->mBatchTex)
    {
        p->Flush();
    }
    if (!p->Allocate(pTex, size, size, alphaOnly ? GPU_A8 : GPU_RGB565, 0, false))
    {
        return 0;
    }
    pTex->mPremultiplied = false;
    pTex->mScaleU = 1.0f;
    pTex->mScaleV = 1.0f;
    return pTex->mTex.data;
}

void C3DRenderer::UnlockTiled(RTexture* pTex)
{
    if (pTex && pTex->mValid)
    {
        C3D_TexFlush(&pTex->mTex);
        pTex->mVersion++;
        mUploads++;
    }
}

void C3DRenderer::FreeTexture(RTexture* pTex)
{
    if (!pTex)
    {
        return;
    }
    Impl* p = mImpl;
    if (p)
    {
        if (pTex == p->mBatchTex)
        {
            mImpl->Flush();
            p->mBatchTex = 0;
        }
        if (p->mBoundTex == &pTex->mTex)
        {
            // citro3d keeps the bound pointer: never leave it dangling.
            p->mBoundTex = 0;
            if (pTex != p->mWhite && p->mWhite && p->mWhite->mValid)
            {
                C3D_TexBind(0, &p->mWhite->mTex);
                p->mBoundTex = &p->mWhite->mTex;
                p->mBoundParam = p->mWhite->mTex.param;
                p->mStateValid = false;
            }
        }
        p->Release(pTex);
    }
    if (pTex->mValid)
    {
        C3D_TexDelete(&pTex->mTex);
    }
    delete pTex;
}

void C3DRenderer::BeginScene()
{
}

void C3DRenderer::EndScene()
{
    if (mImpl)
    {
        mImpl->Flush();
    }
}

void C3DRenderer::DrawTriangles(const RVertex* pVerts, s32 count, RTexture* pTex, u32 state)
{
    Impl* p = mImpl;
    if (!p || !pVerts || count <= 0)
    {
        return;
    }
    PORT_PROF(kPrimC3d);
    if (!pTex || !pTex->mValid || (state & RS_NO_TEXTURE))
    {
        pTex = 0;
        state |= RS_NO_TEXTURE;
    }
    mImpl->BeginFrame();
    const HudRule* pRule = p->mHudGroup ? PickRule(*p->mHudGroup, pVerts, count) : 0;
    // On the top screen, no offset puts the pixel centres where the game's 2x2 pixel blocks meet: on the texel
    // corners of its 1:1 quads, where bilinear filtering averages the 4 texels.
    const bool exact = (state & RS_PIXEL_EXACT) || !pRule;
    // There, a 1:1 quad samples its first level: on the texel corners, that is the exact average.
    const u32 noMip = !pRule && OneToOne(pVerts, count, pTex) ? kStateNoMip : 0;
    p->Append(pVerts, count, pTex, (state & kBatchStateMask) | noMip, exact ? 0.0f : kPixelOffset, pRule ? pRule->slot : p->mClip, pRule);
}

void C3DRenderer::SetLights(const RLight* pLights, s32 count, const f32* pAmbient, f32 unlitAmbient)
{
    if (mImpl)
    {
        mImpl->SetLights(pLights, count, pAmbient, unlitAmbient);
    }
}

s32 C3DRenderer::MaxShaderLights() const
{
    return kMaxShaderLights;
}

void C3DRenderer::SetClipRect(f32 left, f32 top, f32 right, f32 bottom)
{
    Impl* p = mImpl;
    if (!p)
    {
        return;
    }
    // Right and bottom are inclusive: the game passes its window size minus one.
    ScreenRect r = p->ToScreen(left, top, right + 1.0f, bottom + 1.0f);
    const ScreenRect& l = p->mLogicalRect;
    r.x0 = r.x0 > l.x0 ? r.x0 : l.x0;
    r.y0 = r.y0 > l.y0 ? r.y0 : l.y0;
    r.x1 = r.x1 < l.x1 ? r.x1 : l.x1;
    r.y1 = r.y1 < l.y1 ? r.y1 : l.y1;
    p->mClip = (r.x0 < r.x1 && r.y0 < r.y1) ? r : l;
}

bool C3DRenderer::SetHudGroup(s32 group)
{
    Impl* p = mImpl;
    if (!p || !p->mBottom)
    {
        return false;
    }
    const bool hud = group > PortHudScreen::kWorld && group < PortHudScreen::kGroupCount;
    p->mHudGroup = hud ? &p->mGroups[group] : 0;
    return hud;
}

s32 C3DRenderer::HudScreenWidth() const
{
    // The brief's panel: the minimap takes the rest of its row.
    return mImpl && mImpl->mBottom ? mImpl->mPanels[HudTheme::kBrief].x1 : 0;
}
