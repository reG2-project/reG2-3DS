#include "c3d_context.hpp"
#include "../renderer/gl_context.hpp"
#include <3ds.h>

namespace
{
// Big enough for a few thousand draw calls in one frame.
const size_t kCmdBufSize = 0x100000;

C3D_RenderTarget* gTarget;

// gfxInit (done by SDL) set up the top screen; C3DRenderer::Present ends the citro3d frame, which
// transfers the target to the screen and swaps, so SwapBuffers has nothing left to do.
class C3DContext : public IGLContext
{
  public:
    ~C3DContext()
    {
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
    return new C3DContext();
}
