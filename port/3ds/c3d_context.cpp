#include "c3d_context.hpp"
#include "../renderer/gl_context.hpp"
#include "../src/prof.hpp"
#include <3ds.h>

namespace
{
// Big enough for a few thousand draw calls in one frame.
const size_t kCmdBufSize = 0x100000;

C3D_RenderTarget* gTarget;
C3D_RenderTarget* gBottom;
// The top target's display transfer flags. Read from here: citro3d's library has short enums and the port int ones,
// so C3D_RenderTarget's fields after its frame buffer are not where the port's compiler puts them.
u32 sTopFlags;
// The top screen's refreshes counted at the last wait.
u32 sLastTop;
// The frame loop has waited for this frame's refresh.
bool sWaited;
// The top screen's refresh count the frame loop's frame is presented after, when sHoldPresent.
u32 sPresentAfter;
bool sHoldPresent;

// gfxInit (done by SDL) set up the top screen; C3DRenderer::Present ends the citro3d frame, which
// transfers the target to the screen and swaps, so SwapBuffers has nothing left to do.
class C3DContext : public IGLContext
{
  public:
    ~C3DContext()
    {
        if (gBottom)
        {
            C3D_RenderTargetDelete(gBottom);
            gBottom = 0;
        }
        if (gTarget)
        {
            C3D_RenderTargetDelete(gTarget);
            gTarget = 0;
        }
        C3D_Fini();
    }
    void* GetProcAddress(const char*) { return 0; }
    void GetDrawableSize(int* pW, int* pH)
    {
        *pW = 400;
        *pH = 240;
    }
    void SwapBuffers() {}
    void MakeCurrent() {}
};
// C3D_FrameSync's wait, timed in two: the top screen's next refresh, then the bottom screen's.
void WaitRefresh()
{
    const u32 top = C3D_FrameCounter(0);
    const u32 bottom = C3D_FrameCounter(1);
    {
        PORT_PROF_EXCLUDED(kWaitVsync);
        while (C3D_FrameCounter(0) == top)
        {
            gspWaitForAnyEvent();
        }
    }
    {
        PORT_PROF_EXCLUDED(kWaitBottom);
        while (C3D_FrameCounter(1) == bottom)
        {
            gspWaitForAnyEvent();
        }
    }
    PORT_PROF_COUNT(kRefreshes, C3D_FrameCounter(0) - sLastTop);
    sLastTop = C3D_FrameCounter(0);
}

// C3D_FrameEnd's hook, before the frame goes to the GPU: the wait for refresh sPresentAfter, so that it shows at the next.
void HoldPresent(void*)
{
    if (!sHoldPresent)
    {
        return;
    }
    sHoldPresent = false;
    PORT_PROF_EXCLUDED(kWaitVsync);
    while (static_cast<s32>(C3D_FrameCounter(0) - sPresentAfter) < 0)
    {
        gspWaitForAnyEvent();
    }
}
} // namespace

C3D_RenderTarget* C3DContextTarget()
{
    return gTarget;
}

void C3DContextSetStereo(bool stereo)
{
    gfxSet3D(stereo);
    C3D_RenderTargetSetOutput(0, GFX_TOP, stereo ? GFX_LEFT : GFX_RIGHT, 0);
    C3D_RenderTargetSetOutput(gTarget, GFX_TOP, stereo ? GFX_RIGHT : GFX_LEFT, sTopFlags);
}

void C3DContextTransferLeft()
{
    const u32 dim = GX_BUFFER_DIM(240, 400);
    GX_DisplayTransfer((u32*)gTarget->frameBuf.colorBuf, dim, (u32*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL), dim, sTopFlags);
}

C3D_RenderTarget* C3DContextBottomTarget()
{
    return gBottom;
}

void C3DContextFrameSync()
{
    if (!sWaited)
    {
        WaitRefresh();
    }
    sWaited = false;
}

unsigned port_WaitRefresh()
{
    const u32 last = sLastTop;
    WaitRefresh();
    sWaited = true;
    return sLastTop - last;
}

void port_HoldPresent(unsigned refreshes)
{
    sPresentAfter = sLastTop + refreshes;
    sHoldPresent = refreshes > 0;
}

void port_ResyncRefresh()
{
    sLastTop = C3D_FrameCounter(0);
}

unsigned long long port_RefreshNs()
{
    // 4481136 ticks of the system clock per refresh: 59.83 Hz.
    return 4481136ull * 1000000000ull / SYSCLOCK_ARM11;
}

// citro3d draws to the screen directly: no OpenGL window.
unsigned long long GLWindowFlags()
{
    return 0;
}

IGLContext* CreateGLContext(SDL_Window*, const char** ppError)
{
    if (gTarget)
    {
        *ppError = "citro3d context already created";
        return 0;
    }
    if (!C3D_Init(kCmdBufSize))
    {
        *ppError = "C3D_Init failed";
        return 0;
    }
    C3D_FrameEndHook(HoldPresent, 0);
    // The screens take RGB only. A console's LCD ignores alpha, but Azahar hands it to its window, where a pixel
    // drawn with alpha 0 (the menus' black panels) shows what is behind.
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, -1);
    if (!gTarget)
    {
        C3D_Fini();
        *ppError = "C3D_RenderTargetCreate failed";
        return 0;
    }
    sTopFlags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                GX_TRANSFER_OUT_FORMAT(gfxGetScreenFormat(GFX_TOP)) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_RenderTargetSetOutput(gTarget, GFX_TOP, GFX_LEFT, sTopFlags);
    // SDL draws nothing on the bottom screen: no window is on it.
    gBottom = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, -1);
    if (gBottom)
    {
        const u32 bottomFlags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(gfxGetScreenFormat(GFX_BOTTOM)) |
                                GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
        C3D_RenderTargetSetOutput(gBottom, GFX_BOTTOM, GFX_LEFT, bottomFlags);
    }
    return new C3DContext();
}
