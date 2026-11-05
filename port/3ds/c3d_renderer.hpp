// The 3DS backend: GLRenderer's interface (gl_renderer.hpp) drawn with citro3d on the top screen, with the
// classic look only (no per-pixel lighting, shadows or bloom).
#pragma once

#include "../renderer/renderer.hpp"
#include <vector>

class C3DRenderer : public IRenderer
{
  public:
    typedef void* (*Loader)(const char* name);

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
    void UploadTexture(RTexture* pTex, s32 width, s32 height, const u32* pRgba, RMipmaps mipmaps) override;
    void FreeTexture(RTexture* pTex) override;
    void BeginScene() override;
    void EndScene() override;
    void DrawTriangles(const RVertex* pVerts, s32 count, RTexture* pTex, u32 state) override;
    void SetClipRect(f32 left, f32 top, f32 right, f32 bottom) override;
    const RenderOptions& Options() const override { return mOptions; }
    void SetLights(const RLight*, s32, const f32*, f32) override {}
    u32 GetDrawCallCount() const override { return mDrawCalls; }
    // The HUD's groups placed on the bottom screen, 1:1 in layout pixels.
    bool SetHudGroup(s32 group) override;
    s32 HudScreenWidth() const override;
    u32 GetUploadCount() const { return mUploads; }
    u32 GetVertexCount() const { return mVertices; }

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
