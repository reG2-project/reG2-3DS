// Backend interface between the gbh_* layer (render DLL) and the OpenGL core (video DLL).
// Both sides are built by the same compiler, so a plain virtual interface is safe across the DLLs.
#pragma once

#include "gta2_api.hpp"

struct RVertex
{
    f32 x, y;  // logical screen pixels
    f32 rhw;   // 1/w, used for perspective correct interpolation
    u32 rgba;  // bytes in memory: r, g, b, a
    f32 u, v;  // normalised texture coordinates
    // Per pixel or shader lighting only (lit != 0): world position, colour before lighting, shade 0..1.
    f32 wx, wy, wz;
    u32 unlit;
    f32 shade;
    f32 lit;
};

// Optional enhancements; all off is the faithful renderer.
struct RenderOptions
{
    bool mTrueColour = false;       // 8-bit palette colours and an RGBA8 target
    f32 mBloom = 0.0f;              // 0..1, glow from additive draws
    bool mPerPixelLighting = false; // gbh_AddLight lights evaluated per fragment
    s32 mTextureFilter = 0;         // minified textures: 0 bilinear, 1 trilinear, 2 anisotropic
    s32 mMsaa = 0;                  // samples per pixel of the render target, 0 or 1 for none
    bool mSoftShadows = false;      // sprite shadows with soft edges, overlaps darkened once
    u32 mGeneration = 0;            // bumped on every change
};

// A light as the original's LightVertex uses it (already filtered to active lights).
struct RLight
{
    f32 x, y, z;
    f32 radius;
    f32 ceiling; // the height its light fades out above
    f32 r, g, b; // colour times brightness
    f32 dx, dy;              // a beam's direction on the ground
    f32 width, beam;         // a beam's gaussian sigma at the lamp, and 1 for a beam, 0 for a point light
};

enum RenderStateBits
{
    RS_ALPHA_TEST = 1 << 0,   // discard texels with alpha < 0.5 (palette index 0)
    RS_BLEND_ALPHA = 1 << 1,  // src_alpha, 1 - src_alpha
    RS_BLEND_ADD = 1 << 2,    // additive
    RS_FILTER_LINEAR = 1 << 3,
    RS_NO_TEXTURE = 1 << 4,
    RS_PIXEL_EXACT = 1 << 5,  // no D3D pixel centre offset: integer quads copy texels 1:1 (DirectDraw blits)
    // Soft shadow (mSoftShadows): texture alpha blurred by the vertex red channel (texels * 32), times the vertex
    // alpha, into a shadow mask that FinishFrame applies; what is drawn later over it lightens the mask.
    RS_SHADOW = 1 << 6,
};

// Mip levels to build for a texture: none, or averaged with or without regard to transparent texels.
enum RMipmaps
{
    RMIP_NONE,
    RMIP_OPAQUE, // drawn without blending: every texel counts
    RMIP_ALPHA,  // blended: transparent texels lend no colour
};

// What every texel of an upload is, when the caller knows: the AND of TexelBits over its texels, so a
// renderer can choose how to store them without a scan. Without RTX_KNOWN nothing is known.
enum RTexelBits
{
    RTX_KNOWN = 1 << 0,
    RTX_OPAQUE = 1 << 1, // alpha 255
    RTX_BINARY = 1 << 2, // alpha 0 or 255
    RTX_4444 = 1 << 3,   // each channel a 4-bit value expanded by replication
    RTX_565 = 1 << 4,    // red, green and blue 5, 6 and 5-bit values expanded by replication
};

// A texel's RTexelBits; in an RMIP_ALPHA upload a texel of alpha 0 counts as transparent black.
inline u32 TexelBits(u32 c, RMipmaps mipmaps)
{
    if (mipmaps == RMIP_ALPHA && (c >> 24) == 0)
    {
        c = 0;
    }
    const u32 r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
    const bool fits565 = r == ((r & 0xF8) | (r >> 5)) && g == ((g & 0xFC) | (g >> 6)) && b == ((b & 0xF8) | (b >> 5));
    return RTX_KNOWN | (a == 255 ? RTX_OPAQUE : 0) | (a == 0 || a == 255 ? RTX_BINARY : 0) |
           (((c ^ (c >> 4)) & 0x0F0F0F0F) == 0 ? RTX_4444 : 0) | (fits565 ? RTX_565 : 0);
}

struct RTexture;

class IRenderer
{
  public:
    virtual ~IRenderer() {}

    // Texture contents are given as RGBA8, top row first, texels as RTexelBits. The backend may defer GL work.
    virtual RTexture* CreateTexture() = 0;
    virtual void UploadTexture(RTexture* pTex, s32 width, s32 height, const u32* pRgba, RMipmaps mipmaps, u32 texels = 0) = 0;
    virtual void FreeTexture(RTexture* pTex) = 0;
    // A texture the backend may drop when its memory runs short, as its owner can upload it again; Evicted
    // then tells it to (also after an upload that did not fit, once per frame).
    virtual void SetEvictable(RTexture*) {}
    virtual bool Evicted(RTexture*) const { return false; }

    virtual void BeginScene() = 0;
    virtual void EndScene() = 0;

    // Triangles (count multiple of 3).
    virtual void DrawTriangles(const RVertex* pVerts, s32 count, RTexture* pTex, u32 state) = 0;

    virtual void SetClipRect(f32 left, f32 top, f32 right, f32 bottom) = 0;

    virtual const RenderOptions& Options() const = 0;
    // Lights for per pixel lighting; ambient red, green and blue in 0..255 units as gbh_SetAmbient stores it,
    // and how far it takes on the unlit colour (gbh_api.cpp's Ambient).
    virtual void SetLights(const RLight* pLights, s32 count, const f32* pAmbient, f32 unlitAmbient) = 0;
    // A renderer that lights vertices (RVertex lit) in its vertex shader with SetLights' point lights, as gbh_api.cpp's
    // LightVertex: how many of the first lights it takes, -1 for none.
    virtual s32 MaxShaderLights() const { return -1; }

    // Stats exposed through gbh_GetGlobals.
    virtual u32 GetDrawCallCount() const = 0;

    // The HUD draws that follow belong to that group (PortHudScreen::Group, port/src/hud_screen.hpp). True if the
    // renderer shows the group on a HUD screen of its own: its draws then come in 640x480 layout pixels.
    virtual bool SetHudGroup(s32) { return false; }
    // Whether the HUD screen may be redrawn in this frame: only then are its groups' draws wanted.
    virtual bool HudScreenDue() { return true; }
    // The HUD screen group's draws that follow depend only on key: true if the renderer draws again those it kept for it,
    // and the caller leaves its own out; else it keeps them.
    virtual bool KeepHudGroup(u32) { return false; }
    // How much of that screen's width the brief may take, in layout pixels; 0 without one.
    virtual s32 HudScreenWidth() const { return 0; }
};

// Lets the render DLL find the backend from the SVideo pointer the game hands to gbh_InitDLL.
static const u32 kGLVideoMagic = 0x4C47324Cu; // "L2GL"

struct GLVideoHeader
{
    SVideo mVideo; // must stay first: the game only sees an SVideo*
    u32 mMagic;
    IRenderer* mRenderer;
    const u32* mFlipCounter; // number of Vid_FlipBuffers calls so far
};
