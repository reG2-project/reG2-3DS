// The 3DS backend: GLRenderer's interface (gl_renderer.hpp) drawn with citro3d on the top screen, with the
// classic look only (no per-pixel lighting, shadows or bloom).
#pragma once

#include "../renderer/renderer.hpp"
#include "map_cache_mesh.hpp"
#include <vector>

// Texel (x, y) of a texture `width` texels wide in the PICA's layout: 8x8 tiles in rows, Morton order inside.
inline u32 TiledIndex(s32 x, s32 y, s32 width)
{
    const u32 morton = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return ((u32)(y >> 3) * (u32)(width >> 3) + (u32)(x >> 3)) * 64 + morton;
}

class C3DRenderer : public IRenderer
{
  public:
    typedef void* (*Loader)(const char* name);

    // A bottom screen draw state: nearest sampling (the bottom screen's own draws are linear otherwise).
    static const u32 kStateNearest = 1u << 28;

    C3DRenderer();
    ~C3DRenderer();

    // The loader is OpenGL's; citro3d needs none.
    bool Init(Loader loader, const char** ppError);
    void Shutdown();

    void SetLogicalSize(s32 width, s32 height);
    void SetSmoothMagnify(bool) {}
    s32 LogicalWidth() const { return mLogicalW; }
    s32 LogicalHeight() const { return mLogicalH; }
    s32 TargetWidth() const { return mLogicalW; }
    s32 TargetHeight() const { return mLogicalH; }

    void Clear(u8 r, u8 g, u8 b, s32 left, s32 top, s32 right, s32 bottom);
    // The game's 16-bit CPU surface (RGB565); pixels equal to key are left out.
    void CompositeSurface(const u16* pPixels, s32 pitchBytes, s32 width, s32 height, u16 key);
    void FinishFrame();
    // Ends the frame: citro3d transfers it to the top screen.
    void Present(s32 drawableW, s32 drawableH);
    void ThrottleFrames() {}
    void SetGamma(f32, f32, f32) {}
    void ReadLogical(std::vector<u8>& out) { out.assign((size_t)mLogicalW * mLogicalH * 4, 0); }
    void SetVertexOffset(f32, f32) {}
    void SetSampleShift(f32) {}
    void SetOptions(const RenderOptions& options) { mOptions = options; }
    s32 MaxSamples() const { return 0; }

    RTexture* CreateTexture() override;
    void UploadTexture(RTexture* pTex, s32 width, s32 height, const u32* pRgba, RMipmaps mipmaps, u32 texels = 0) override;
    void FreeTexture(RTexture* pTex) override;
    void SetEvictable(RTexture* pTex) override;
    bool Evicted(RTexture* pTex) const override;
    void BeginScene() override;
    void EndScene() override;
    void DrawTriangles(const RVertex* pVerts, s32 count, RTexture* pTex, u32 state) override;
    void SetClipRect(f32 left, f32 top, f32 right, f32 bottom) override;
    const RenderOptions& Options() const override { return mOptions; }
    void SetLights(const RLight* pLights, s32 count, const f32* pAmbient, f32 unlitAmbient) override;
    s32 MaxShaderLights() const override;
    u32 GetDrawCallCount() const override { return mDrawCalls; }
    // The HUD's groups placed on the bottom screen, 1:1 in layout pixels.
    bool SetHudGroup(s32 group) override;
    bool HudScreenDue() override;
    bool KeepHudGroup(u32 key) override;
    s32 HudScreenWidth() const override;
    u32 GetUploadCount() const { return mUploads; }
    // Storage for a size x size texture (a power of two) that the caller fills in the tiled order (TiledIndex), then
    // UnlockTiled: RGB565 texels, or with alphaOnly A8 ones drawn in the vertex colour. Null if it does not fit.
    void* LockTiled(RTexture* pTex, s32 size, bool alphaOnly);
    void UnlockTiled(RTexture* pTex);
    u32 GetVertexCount() const { return mVertices; }

    // Static meshes in linear memory projected by their own vertex shader (map_cache.v.pica).
    typedef MapCacheMesh::Vertex MeshVertex;
    typedef MapCacheMesh::Draw MeshDraw;
    typedef MapCacheMesh::TexturePlace MeshTexturePlace;
    // Begins the frame if it has not begun; its number.
    u32 MeshFrame();
    // False when the texture has no storage. Marks it drawn in this frame, so it is not evicted before the meshes.
    bool MeshTexture(RTexture* pTex, MeshTexturePlace* pPlace);
    // Draws in order after what DrawTriangles took so far; lit: the shader lights the vertices. False without the shader.
    bool DrawMeshes(const MeshDraw* pDraws, s32 count, bool lit);
    // Counts FreeTexture calls: textures a mesh points at may be gone.
    static u32 FreedTextures();
    // DrawMeshes' ticks since the last call: the batch before, the program switch, the draws, the switch back, the
    // first draw's C3D_DrawArrays and the others'.
    static void TakeMeshTicks(uint64_t* pTicks);

  private:
    struct Impl;
    Impl* mImpl;
    RenderOptions mOptions;
    s32 mLogicalW;
    s32 mLogicalH;
    u32 mDrawCalls;
    u32 mUploads;
    u32 mVertices;
};
