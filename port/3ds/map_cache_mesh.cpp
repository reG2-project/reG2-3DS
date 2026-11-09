#include "map_cache_mesh.hpp"
#include "c3d_renderer.hpp"
#include <3ds.h>

IRenderer* port_MapCacheRenderer();
RTexture* port_MapCacheTexture(STexture* pTexture, bool alpha);
bool port_MapCacheLit();
bool port_MapCacheNeedsCpuLight(f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1);

namespace MapCacheMesh
{
namespace
{
// The backend: vid_system.hpp makes C3DRenderer the renderer on the 3DS.
C3DRenderer* sRenderer;
}

bool BeginFrame(uint32_t* pFrame)
{
    sRenderer = static_cast<C3DRenderer*>(port_MapCacheRenderer());
    if (!sRenderer)
    {
        return false;
    }
    *pFrame = sRenderer->MeshFrame();
    return true;
}

RTexture* TileTexture(STexture* pTexture, bool alpha)
{
    return port_MapCacheTexture(pTexture, alpha);
}

bool Texture(RTexture* pTex, TexturePlace* pPlace)
{
    return sRenderer && sRenderer->MeshTexture(pTex, pPlace);
}

bool Lit()
{
    return port_MapCacheLit();
}

bool NeedsCpuLight(float x0, float y0, float z0, float x1, float y1, float z1)
{
    return port_MapCacheNeedsCpuLight(x0, y0, z0, x1, y1, z1);
}

void DrawAll(const Draw* pDraws, int32_t count, bool lit)
{
    if (sRenderer)
    {
        sRenderer->DrawMeshes(pDraws, count, lit);
    }
}

uint32_t FreedTextures()
{
    return C3DRenderer::FreedTextures();
}

Vertex* Alloc(size_t count)
{
    return (Vertex*)linearAlloc(count * sizeof(Vertex));
}

void Free(void* p)
{
    linearFree(p);
}

void Flush(const void* p, size_t bytes)
{
    GSPGPU_FlushDataCache(p, bytes);
}

uint32_t LinearFreeKb()
{
    return (uint32_t)(linearSpaceFree() / 1024);
}

uint64_t Ticks()
{
    return svcGetSystemTick();
}

void TakeDrawTicks(uint64_t* pTicks)
{
    C3DRenderer::TakeMeshTicks(pTicks);
}

float TicksToMs(uint64_t ticks)
{
    return ticks * (1000.0f / SYSCLOCK_ARM11);
}
} // namespace MapCacheMesh
