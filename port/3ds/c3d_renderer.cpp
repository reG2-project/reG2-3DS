#include "c3d_renderer.hpp"
#include "c3d_context.hpp"
#include "stats_3ds.hpp"
#include <3ds.h>
#include <citro3d.h>
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
// The top screen's target, landscape: 800x480, which the display transfer halves into the 400x240 screen.
const s32 kScreenW = 800;
const s32 kScreenH = 480;
const s32 kMaxTexSize = 1024;
// GLRenderer's default vertex offset: D3D pixel centres are on integers, the PICA's on halves.
const f32 kPixelOffset = 0.5f - 1.0f / 2048.0f;
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

struct ScreenRect
{
    s32 x0, y0, x1, y1;
    bool operator!=(const ScreenRect& o) const { return x0 != o.x0 || y0 != o.y0 || x1 != o.x1 || y1 != o.y1; }
};

s32 Round(f32 v)
{
    return (s32)(v < 0.0f ? v - 0.5f : v + 0.5f);
}
} // namespace

struct C3DRenderer::Impl
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

    bool mInFrame;
    bool mNeedClear;
    u32 mFrame;
    // Textures the GPU may still read: freed once the next frame has begun (C3D_FrameBegin waits for the GPU).
    std::vector<C3D_Tex> mPendingFree;
    RTexture* mWhite;
    RTexture* mSurface;
    bool mWarnedVerts, mWarnedCmdBuf, mWarnedAlloc;
    C3DRenderer* mOwner;

    ScreenRect ToScreen(f32 left, f32 top, f32 right, f32 bottom) const;
    void BeginFrame();
    void Append(const RVertex* pVerts, s32 count, RTexture* pTex, u32 state, f32 offset, const ScreenRect& scissor);
    void Flush();
    void ApplyState(RTexture* pTex, u32 state, const ScreenRect& scissor);
    bool Allocate(RTexture* pTex, s32 tw, s32 th, GPU_TEXCOLOR fmt, bool clear);
};

// Storage for new texels: kept when the GPU is done with it and it has the size and format, else replaced.
bool C3DRenderer::Impl::Allocate(RTexture* pTex, s32 tw, s32 th, GPU_TEXCOLOR fmt, bool clear)
{
    if (pTex->mValid)
    {
        const bool inFlight = pTex->mUsedFrame == mFrame;
        if (!inFlight && pTex->mTex.width == tw && pTex->mTex.height == th && pTex->mTex.fmt == fmt)
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
    if (!C3D_TexInit(&pTex->mTex, (u16)tw, (u16)th, fmt))
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

void C3DRenderer::Impl::Append(const RVertex* pVerts, s32 count, RTexture* pTex, u32 state, f32 offset, const ScreenRect& scissor)
{
    if (mVertCount > mBatchStart && (pTex != mBatchTex || state != mBatchState || scissor != mBatchScissor))
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
        pDst[i].x = s.x + offset;
        pDst[i].y = s.y + offset;
        pDst[i].rhw = s.rhw;
        pDst[i].rgba = s.rgba;
        pDst[i].u = s.u * su;
        pDst[i].v = 1.0f - s.v * sv;
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

void C3DRenderer::Impl::ApplyState(RTexture* pTex, u32 state, const ScreenRect& scissor)
{
    if (!mStateValid || scissor != mAppliedScissor)
    {
        // Mtx_OrthoTilt turns the screen: x runs down the 800 rows of the 480x800 target, y across its columns.
        C3D_SetScissor(GPU_SCISSOR_NORMAL, kScreenH - scissor.y1, kScreenW - scissor.x1, kScreenH - scissor.y0, kScreenW - scissor.x0);
        mAppliedScissor = scissor;
    }
    if (!mStateValid || state != mAppliedState)
    {
        if (state & RS_BLEND_ADD)
        {
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
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
        mAppliedState = state;
    }
    mStateValid = true;
    // Untextured draws keep a valid texture bound: citro3d reads the bound one whenever it is dirty.
    RTexture* pBind = pTex ? pTex : mWhite;
    const GPU_TEXTURE_FILTER_PARAM filter = (state & RS_FILTER_LINEAR) ? GPU_LINEAR : GPU_NEAREST;
    C3D_TexSetFilter(&pBind->mTex, filter, filter);
    if (mBoundTex != &pBind->mTex || mBoundParam != pBind->mTex.param)
    {
        C3D_TexBind(0, &pBind->mTex);
        mBoundTex = &pBind->mTex;
        mBoundParam = pBind->mTex.param;
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
    mImpl->Append(v, 6, 0, RS_NO_TEXTURE, 0.0f, mImpl->mLogicalRect);
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
    if (!p->Allocate(pTex, Pow2(width), Pow2(height), GPU_RGBA5551, true))
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
    p->Append(v, 6, p->mSurface, RS_BLEND_ALPHA, 0.0f, p->mLogicalRect);
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
    // A frame without draws shows the target again.
    mImpl->BeginFrame();
    mImpl->Flush();
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

void C3DRenderer::UploadTexture(RTexture* pTex, s32 width, s32 height, const u32* pRgba, RMipmaps)
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
    const GPU_TEXCOLOR fmt = PickFormat(pRgba, (size_t)width * height);
    if (!p->Allocate(pTex, tw, th, fmt, false))
    {
        return;
    }
    mUploads++;
    void* pData = pTex->mTex.data;
    switch (fmt)
    {
    case GPU_RGB565:
        WriteTiles((u16*)pData, tw, th, pRgba, width, height, cw, ch, [](u32 c) {
            return (u16)((((c >> 3) & 0x1F) << 11) | (((c >> 10) & 0x3F) << 5) | ((c >> 19) & 0x1F));
        });
        break;
    case GPU_RGBA4:
        WriteTiles((u16*)pData, tw, th, pRgba, width, height, cw, ch, [](u32 c) {
            return (u16)((((c >> 4) & 0xF) << 12) | (((c >> 12) & 0xF) << 8) | (((c >> 20) & 0xF) << 4) | (c >> 28));
        });
        break;
    case GPU_RGBA5551:
        WriteTiles((u16*)pData, tw, th, pRgba, width, height, cw, ch, [](u32 c) {
            return (u16)((((c >> 3) & 0x1F) << 11) | (((c >> 11) & 0x1F) << 6) | (((c >> 19) & 0x1F) << 1) | (c >> 31));
        });
        break;
    default:
        WriteTiles((u32*)pData, tw, th, pRgba, width, height, cw, ch, [](u32 c) { return __builtin_bswap32(c); });
        break;
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
    p->Append(pVerts, count, pTex, state & kBatchStateMask, (state & RS_PIXEL_EXACT) ? 0.0f : kPixelOffset, p->mClip);
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
