#include "c3d_renderer.hpp"
#include "c3d_context.hpp"
#include "stats_3ds.hpp"
#include "../src/hud_screen.hpp"
#include "hud_theme.hpp"
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// render.v.pica assembled by picasso and embedded by bin2s.
extern "C" const u8 render_shbin[];
extern "C" const u8 render_shbin_end[];

// The game's texture: a citro3d texture padded to a power of two, u and v scaled to the used part.
struct RTexture
{
    C3D_Tex mTex;
    bool mValid;
    f32 mScaleU;
    f32 mScaleV;
    u32 mUsedFrame; // last frame that drew it, 0 for none
    bool mPremultiplied; // colours times alpha: blended with ONE, 1 - alpha
    u8 mLevels;          // mip levels after the first
};

namespace
{
// What the vertex shader loads (v0 position and rhw, v1 colour bytes, v2 uv).
struct C3DVertex
{
    f32 x, y, rhw;
    u32 rgba;
    f32 u, v;
};

const s32 kMaxVerts = 65536;
// The top screen's target, landscape: the game's 800x480 at half size.
const s32 kScreenW = 400;
const s32 kScreenH = 240;
const s32 kBottomW = 320;
const s32 kBottomH = 240;
const s32 kMaxTexSize = 1024;
// GLRenderer's default vertex offset: D3D pixel centres are on integers, the PICA's on halves.
const f32 kPixelOffset = 0.5f - 1.0f / 2048.0f;
// Batch state only: the batch's texture is premultiplied; its draws sample the first mip level only.
const u32 kStatePremultiplied = 1u << 31;
const u32 kStateNoMip = 1u << 30;
const u32 kBatchStateMask = RS_ALPHA_TEST | RS_BLEND_ALPHA | RS_BLEND_ADD | RS_FILTER_LINEAR | RS_NO_TEXTURE;

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

// Texels into the PICA's tiled layout, image row y at t = 1 - y / th (Append flips v); the padding repeats the edges.
template <typename T, typename Conv>
void WriteTiles(T* pDst, s32 tw, s32 th, const u32* pSrc, s32 width, s32 height, s32 cw, s32 ch, Conv conv)
{
    for (s32 y = 0; y < th; y++)
    {
        const s32 cy = y < ch ? y : ch - 1;
        const s32 sy = ch == height ? cy : (s32)((long long)cy * height / ch);
        const u32* pRow = pSrc + (size_t)sy * width;
        T* pTiles = pDst + (size_t)(y >> 3) * (tw >> 3) * 64;
        const u32 my = (u32)kMorton[y & 7] << 1;
        for (s32 x = 0; x < tw; x++)
        {
            const s32 cx = x < cw ? x : cw - 1;
            const s32 sx = cw == width ? cx : (s32)((long long)cx * width / cw);
            pTiles[(x >> 3) * 64 + (kMorton[x & 7] | my)] = conv(pRow[sx]);
        }
    }
}

// The smallest format that holds the texels exactly, else 16 bits with the alpha kept, else RGBA8.
GPU_TEXCOLOR PickFormat(const u32* pRgba, size_t count)
{
    bool opaque = true, binary = true, fits4 = true, fits565 = true;
    for (size_t i = 0; i < count; i++)
    {
        const u32 c = pRgba[i];
        const u32 r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
        opaque = opaque && a == 255;
        binary = binary && (a == 0 || a == 255);
        // A 4-bit channel expanded by replication is a multiple of 17.
        fits4 = fits4 && ((c ^ (c >> 4)) & 0x0F0F0F0F) == 0;
        fits565 = fits565 && r == ((r & 0xF8) | (r >> 5)) && g == ((g & 0xFC) | (g >> 6)) && b == ((b & 0xF8) | (b >> 5));
        if (!binary && !fits4)
        {
            return GPU_RGBA8;
        }
    }
    if (opaque && fits565)
    {
        return GPU_RGB565;
    }
    if (fits4)
    {
        return GPU_RGBA4;
    }
    return opaque ? GPU_RGB565 : GPU_RGBA5551;
}

// Texels into a level of a texture of format fmt.
void WriteLevel(void* pData, GPU_TEXCOLOR fmt, s32 tw, s32 th, const u32* pSrc, s32 width, s32 height, s32 cw, s32 ch)
{
    switch (fmt)
    {
    case GPU_RGB565:
        WriteTiles((u16*)pData, tw, th, pSrc, width, height, cw, ch, [](u32 c) {
            return (u16)((((c >> 3) & 0x1F) << 11) | (((c >> 10) & 0x3F) << 5) | ((c >> 19) & 0x1F));
        });
        break;
    case GPU_RGBA4:
        WriteTiles((u16*)pData, tw, th, pSrc, width, height, cw, ch, [](u32 c) {
            return (u16)((((c >> 4) & 0xF) << 12) | (((c >> 12) & 0xF) << 8) | (((c >> 20) & 0xF) << 4) | (c >> 28));
        });
        break;
    case GPU_RGBA5551:
        WriteTiles((u16*)pData, tw, th, pSrc, width, height, cw, ch, [](u32 c) {
            return (u16)((((c >> 3) & 0x1F) << 11) | (((c >> 11) & 0x1F) << 6) | (((c >> 19) & 0x1F) << 1) | (c >> 31));
        });
        break;
    default:
        WriteTiles((u32*)pData, tw, th, pSrc, width, height, cw, ch, [](u32 c) { return __builtin_bswap32(c); });
        break;
    }
}

// x / 255 rounded, for x up to 255 * 255, without a division (the ARM11 has none).
u32 Div255(u32 x)
{
    x += 128;
    return (x + (x >> 8)) >> 8;
}

// A channel rounded to the nearest value of `bits` bits, expanded back by replication as the GPU does.
u32 RoundChannel(u32 v, s32 bits)
{
    if (bits == 1)
    {
        return v >= 128 ? 255 : 0;
    }
    const u32 q = Div255(v * ((1u << bits) - 1));
    return (q << (8 - bits)) | (q >> (2 * bits - 8));
}

// The next mip level: each 2x2 block averaged, rounded to what fmt holds so its truncating writes are exact.
void HalveLevel(u32* pDst, const u32* pSrc, s32 w, s32 h, GPU_TEXCOLOR fmt)
{
    static const s8 kBits[][4] = {{8, 8, 8, 8}, {8, 8, 8, 0}, {5, 5, 5, 1}, {5, 6, 5, 0}, {4, 4, 4, 4}};
    const s8* bits = kBits[fmt <= GPU_RGBA4 ? fmt : 0];
    for (s32 y = 0; y < h / 2; y++)
    {
        const u32* p0 = pSrc + (size_t)(2 * y) * w;
        const u32* p1 = p0 + w;
        for (s32 x = 0; x < w / 2; x++)
        {
            const u32 a = p0[2 * x], b = p0[2 * x + 1], c = p1[2 * x], d = p1[2 * x + 1];
            u32 out = 0;
            for (s32 k = 0; k < 4; k++)
            {
                const s32 sh = 8 * k;
                u32 v = (((a >> sh) & 0xFF) + ((b >> sh) & 0xFF) + ((c >> sh) & 0xFF) + ((d >> sh) & 0xFF) + 2) >> 2;
                v = bits[k] == 8 ? v : bits[k] == 0 ? 255 : RoundChannel(v, bits[k]);
                out |= v << sh;
            }
            pDst[(size_t)y * (w / 2) + x] = out;
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
    const f32 det = x1 * y2 - x2 * y1;
    if (fabsf(det) < 1.0f)
    {
        return false;
    }
    // Texels per logical pixel along x and y.
    const f32 dudx = (u1 * y2 - u2 * y1) / det, dudy = (u2 * x1 - u1 * x2) / det;
    const f32 dvdx = (v1 * y2 - v2 * y1) / det, dvdy = (v2 * x1 - v1 * x2) / det;
    const f32 e = 0.01f;
    const bool straight = fabsf(fabsf(dudx) - 1.0f) < e && fabsf(dudy) < e && fabsf(dvdx) < e && fabsf(fabsf(dvdy) - 1.0f) < e;
    const bool turned = fabsf(dudx) < e && fabsf(fabsf(dudy) - 1.0f) < e && fabsf(fabsf(dvdx) - 1.0f) < e && fabsf(dvdy) < e;
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
// weapon below it, the brief (scrolling when long) at the bottom; each part in its panel (HudTheme::Part).
const HudTheme::Rect kHudPanels[HudTheme::kPartCount] = {
    {2, 2, 102, 80},     // kGangs
    {2, 84, 102, 124},   // kPager
    {106, 2, 318, 50},   // kStatus
    {106, 54, 236, 124}, // kWanted
    {240, 54, 318, 124}, // kWeapon
    {2, 128, 318, 238},  // kBrief
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
    {kAll, 0, -244, {64, 130, 318, 236}, true, HudTheme::kBrief},
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

struct C3DRenderer::Impl : HudTheme::Sink
{
    C3D_RenderTarget* mTarget;
    DVLB_s* mDvlb;
    shaderProgram_s mProgram;
    s32 mUniProjection;
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
    C3D_RenderTarget* mBottom;
    C3D_Mtx mBottomProjection;
    // The target drawn on, landscape.
    s32 mTargetW, mTargetH;

    bool mInFrame;
    bool mFrameDrew;
    bool mNeedClear;
    u32 mFrame;
    // Textures the GPU may still read: freed once the next frame has begun (C3D_FrameBegin waits for the GPU).
    std::vector<C3D_Tex> mPendingFree;
    RTexture* mWhite;
    RTexture* mSurface;
    std::vector<u32> mConverted; // UploadTexture's texels after premultiplying
    std::vector<u32> mLevel;     // and its mip levels being made
    bool mWarnedVerts, mWarnedCmdBuf, mWarnedAlloc;
    C3DRenderer* mOwner;

    ScreenRect ToScreen(f32 left, f32 top, f32 right, f32 bottom) const;
    void BeginFrame();
    void Append(const RVertex* pVerts, s32 count, RTexture* pTex, u32 state, f32 offset, const ScreenRect& scissor, const HudRule* pRule);
    void Flush();
    f32 ScrollOffset();
    void DrawHud();
    void DrawQuads(const HudTheme::Quad* pQuads, s32 count, RTexture* pTex, u32 state) override;
    void ApplyState(RTexture* pTex, u32 state, const ScreenRect& scissor);
    bool Allocate(RTexture* pTex, s32 tw, s32 th, GPU_TEXCOLOR fmt, s32 levels, bool clear);
};

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
        if (inFlight)
        {
            mPendingFree.push_back(pTex->mTex);
        }
        else
        {
            C3D_TexDelete(&pTex->mTex);
        }
        pTex->mValid = false;
    }
    if (mBoundTex == &pTex->mTex)
    {
        mBoundTex = 0;
    }
    const C3D_TexInitParams params = {(u16)tw, (u16)th, (u8)levels, fmt, GPU_TEX_2D, false};
    if (!C3D_TexInitWithParams(&pTex->mTex, NULL, params))
    {
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
    pTex->mLevels = (u8)levels;
    pTex->mValid = true;
    pTex->mUsedFrame = 0;
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
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    Stats3DS_AddFrameBegin(svcGetSystemTick() - t0);
    mInFrame = true;
    mFrameDrew = false;
    mFrame++;
    for (size_t i = 0; i < mPendingFree.size(); i++)
    {
        C3D_TexDelete(&mPendingFree[i]);
    }
    mPendingFree.clear();
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
    C3D_BufInfo* pBuf = C3D_GetBufInfo();
    BufInfo_Init(pBuf);
    BufInfo_Add(pBuf, mVerts, sizeof(C3DVertex), 3, 0x210);
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
    if (!pRule)
    {
        // Nearest sampling at half size would drop every other texel.
        state |= RS_FILTER_LINEAR;
    }
    else
    {
        state |= kStateNoMip;
    }
    if (mVertCount > mBatchStart && (pTex != mBatchTex || state != mBatchState || scissor != mBatchScissor || pRule != mBatchRule))
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
    mBatchTex = pTex;
    mBatchState = state;
    mBatchScissor = scissor;
    mBatchRule = pRule;
    mFrameDrew = true;
    const f32 dx = offset + (pRule ? pRule->dx : 0);
    const f32 dy = offset + (pRule ? pRule->dy : 0);
    const f32 su = pTex ? pTex->mScaleU : 0.0f;
    const f32 sv = pTex ? pTex->mScaleV : 0.0f;
    if (pTex)
    {
        pTex->mUsedFrame = mFrame;
    }
    C3DVertex* pDst = mVerts + mVertCount;
    for (s32 i = 0; i < count; i++)
    {
        const RVertex& s = pVerts[i];
        pDst[i].x = s.x + dx;
        pDst[i].y = s.y + dy;
        pDst[i].rhw = s.rhw;
        pDst[i].rgba = s.rgba;
        pDst[i].u = s.u * su;
        pDst[i].v = 1.0f - s.v * sv;
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

void C3DRenderer::Impl::Flush()
{
    const s32 n = mVertCount - mBatchStart;
    if (n <= 0)
    {
        return;
    }
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
    ApplyState(mBatchTex, mBatchState, mBatchScissor);
    C3D_DrawArrays(GPU_TRIANGLES, mBatchStart, n);
    mBatchStart = mVertCount;
    mOwner->mDrawCalls++;
    mOwner->mVertices += (u32)n;
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

// The bottom screen, after the top one: cleared, with the HUD's batches of the frame.
void C3DRenderer::Impl::DrawHud()
{
    const f32 scroll = ScrollOffset();
    if (!mBottom)
    {
        mHudBatches.clear();
        return;
    }
    C3D_RenderTargetClear(mBottom, C3D_CLEAR_COLOR, 0x000000FF, 0);
    C3D_FrameDrawOn(mBottom);
    mTargetW = kBottomW;
    mTargetH = kBottomH;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, mUniProjection, &mBottomProjection);
    mStateValid = false;
    if (mHudPanels)
    {
        HudTheme::Draw(*mOwner, *this, kHudPanels, mHudPanels);
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
        }
        ApplyState(b.tex, b.state, b.scissor);
        C3D_DrawArrays(GPU_TRIANGLES, b.start, b.count);
        mOwner->mDrawCalls++;
        mOwner->mVertices += (u32)b.count;
    }
    mHudBatches.clear();
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
        Append(v, 6, pTex, state & kBatchStateMask, 0.0f, bottom, 0);
    }
}

void C3DRenderer::Impl::ApplyState(RTexture* pTex, u32 state, const ScreenRect& scissor)
{
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
    // Untextured draws keep a valid texture bound: citro3d reads the bound one whenever it is dirty.
    RTexture* pBind = pTex ? pTex : mWhite;
    const GPU_TEXTURE_FILTER_PARAM filter = (state & RS_FILTER_LINEAR) ? GPU_LINEAR : GPU_NEAREST;
    C3D_TexSetFilter(&pBind->mTex, filter, filter);
    C3D_TexSetFilterMipmap(&pBind->mTex, GPU_LINEAR);
    pBind->mTex.maxLevel = (state & kStateNoMip) ? 0 : pBind->mLevels;
    if (mBoundTex != &pBind->mTex || mBoundParam != pBind->mTex.param || mBoundLod != pBind->mTex.lodParam)
    {
        C3D_TexBind(0, &pBind->mTex);
        mBoundTex = &pBind->mTex;
        mBoundParam = pBind->mTex.param;
        mBoundLod = pBind->mTex.lodParam;
    }
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
    p->mNeedClear = true;
    p->mFrame = 1;
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
    FreeTexture(p->mSurface);
    p->mSurface = 0;
    FreeTexture(p->mWhite);
    p->mWhite = 0;
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
    // A frame without draws shows the targets again.
    mImpl->BeginFrame();
    mImpl->Flush();
    if (p->mFrameDrew)
    {
        p->DrawHud();
    }
    if (p->mVertCount > 0)
    {
        GSPGPU_FlushDataCache(p->mVerts, p->mVertCount * sizeof(C3DVertex));
    }
    const u64 t0 = svcGetSystemTick();
    C3D_FrameEnd(0);
    p->mInFrame = false;
    Stats3DS_Frame(svcGetSystemTick() - t0);
}

RTexture* C3DRenderer::CreateTexture()
{
    RTexture* pTex = new RTexture;
    memset(pTex, 0, sizeof(*pTex));
    return pTex;
}

void C3DRenderer::UploadTexture(RTexture* pTex, s32 width, s32 height, const u32* pRgba, RMipmaps mipmaps)
{
    Impl* p = mImpl;
    if (!p || !pTex || !pRgba || width <= 0 || height <= 0)
    {
        return;
    }
    if (pTex == p->mBatchTex)
    {
        mImpl->Flush();
    }
    const s32 cw = width < kMaxTexSize ? width : kMaxTexSize;
    const s32 ch = height < kMaxTexSize ? height : kMaxTexSize;
    const s32 tw = Pow2(cw);
    const s32 th = Pow2(ch);
    const u64 t0 = svcGetSystemTick();
    // Blended textures are premultiplied: filtered, transparent texels lend no colour.
    const size_t n = (size_t)width * height;
    bool premultiply = false;
    for (size_t i = 0; mipmaps == RMIP_ALPHA && i < n && !premultiply; i++)
    {
        premultiply = (pRgba[i] >> 24) != 0xFF;
    }
    if (premultiply)
    {
        p->mConverted.resize(n);
        for (size_t i = 0; i < n; i++)
        {
            const u32 c = pRgba[i], a = c >> 24;
            const u32 r = Div255((c & 0xFF) * a), g = Div255(((c >> 8) & 0xFF) * a), b = Div255(((c >> 16) & 0xFF) * a);
            p->mConverted[i] = r | (g << 8) | (b << 16) | (a << 24);
        }
        pRgba = &p->mConverted[0];
    }
    pTex->mPremultiplied = premultiply;
    GPU_TEXCOLOR fmt = PickFormat(pRgba, n);
    // The game's textures get mip levels down to 8x8, for the world drawn smaller than 1:1 on the top screen.
    const s32 levels = mipmaps == RMIP_NONE ? 0 : C3D_TexCalcMaxLevel(tw, th);
    if (levels > 0 && premultiply && fmt == GPU_RGBA5551)
    {
        // Averaged edges need more than one bit of alpha.
        fmt = GPU_RGBA4;
    }
    if (!p->Allocate(pTex, tw, th, fmt, levels, false))
    {
        return;
    }
    mUploads++;
    if (levels == 0)
    {
        WriteLevel(pTex->mTex.data, fmt, tw, th, pRgba, width, height, cw, ch);
    }
    else
    {
        // Level 0 padded to the texture's size, then each level halved from the one before.
        std::vector<u32>& level = p->mLevel;
        level.resize((size_t)tw * th + (size_t)tw * th / 4);
        for (s32 y = 0; y < th; y++)
        {
            const s32 cy = y < ch ? y : ch - 1;
            const u32* pRow = pRgba + (size_t)(ch == height ? cy : (s32)((long long)cy * height / ch)) * width;
            for (s32 x = 0; x < tw; x++)
            {
                const s32 cx = x < cw ? x : cw - 1;
                level[(size_t)y * tw + x] = pRow[cw == width ? cx : (s32)((long long)cx * width / cw)];
            }
        }
        u32* pCur = &level[0];
        u32* pNext = pCur + (size_t)tw * th;
        for (s32 l = 0; l <= levels; l++)
        {
            const s32 w = tw >> l, h = th >> l;
            WriteLevel(C3D_Tex2DGetImagePtr(&pTex->mTex, l, NULL), fmt, w, h, pCur, w, h, w, h);
            if (l < levels)
            {
                HalveLevel(pNext, pCur, w, h, fmt);
                u32* pTmp = pCur;
                pCur = pNext;
                pNext = pTmp;
            }
        }
    }
    C3D_TexFlush(&pTex->mTex);
    Stats3DS_AddUpload(svcGetSystemTick() - t0);
    pTex->mScaleU = (f32)cw / tw;
    pTex->mScaleV = (f32)ch / th;
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
            }
        }
        if (pTex->mValid && pTex->mUsedFrame == p->mFrame)
        {
            p->mPendingFree.push_back(pTex->mTex);
            pTex->mValid = false;
        }
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
    p->mHudGroup = hud ? &kHudGroups[group] : 0;
    return hud;
}

s32 C3DRenderer::HudScreenWidth() const
{
    return mImpl && mImpl->mBottom ? kBottomW : 0;
}
