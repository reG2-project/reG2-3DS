// The HUD screen's backdrop: a dark textured background and a recessed panel behind each HUD part drawn.
#pragma once

#include "types.hpp"

class IRenderer;
struct RTexture;

namespace HudTheme
{
// The panels of the HUD screen's layout, each behind the HUD parts placed in it.
enum Part
{
    kGangs,
    kPager,
    kStatus, // clock, multiplier, lives, health, money
    kWanted, // wanted level and power-ups
    kWeapon,
    kBrief,
    kPartCount
};

struct Rect
{
    s16 x0, y0, x1, y1;
};

// A rectangle in HUD screen pixels, with uv and colour per corner: top left, top right, bottom right, bottom left.
struct Quad
{
    f32 x0, y0, x1, y1;
    f32 u0, v0, u1, v1;
    u32 rgba[4];
};

class Sink
{
  public:
    // One draw on the HUD screen: plain quads (pTex null) or textured ones times their colour; state is RS_* bits.
    virtual void DrawQuads(const Quad* pQuads, s32 count, RTexture* pTex, u32 state) = 0;
};

// Draws the backdrop of a frame whose HUD drew in `parts` (a bit per Part); pPanels[part] is each part's panel.
void Draw(IRenderer& renderer, Sink& sink, const Rect* pPanels, u32 parts);
void Shutdown(IRenderer& renderer);
} // namespace HudTheme
