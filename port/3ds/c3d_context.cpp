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
// The top screen's refreshes counted at the last wait.
u32 sLastTop;

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
} // namespace

C3D_RenderTarget* C3DContextTarget()
{
    return gTarget;
}

C3D_RenderTarget* C3DContextBottomTarget()
{
    return gBottom;
}

void C3DContextFrameSync()
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
    const u32 flags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                      GX_TRANSFER_OUT_FORMAT(gfxGetScreenFormat(GFX_TOP)) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_RenderTargetSetOutput(gTarget, GFX_TOP, GFX_LEFT, flags);
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
