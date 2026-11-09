// A screen of its own for the HUD (the 3DS's bottom screen): Hud::DrawGui marks its draws in groups, and
// the renderer takes the groups it places on that screen, which it then gets in 640x480 layout pixels.
#pragma once

#include "types.hpp"

namespace PortHudScreen
{
// Kept in step with the renderer's placement table (port/3ds/c3d_renderer.cpp).
enum Group
{
    kWorld, // the world view's, and everything not listed below
    kStats, // money, multiplier, lives, health, weapon, ammo, power-ups and the clock
    kWanted,
    kGangs,
    kBrief,
    kPagers,
    kGroupCount
};

// The HUD draws that follow belong to that group; kWorld at the end of the HUD.
void SetGroup(Group group);
// True while the group being drawn is on the HUD screen: positions are the layout's, at scale 1.
bool LayoutPixels();
// False while the group goes to a HUD screen that keeps its last picture in this frame: its draws can be left out.
bool Drawn();
// The brief's line width in layout pixels: the original's, or less to fit the HUD screen.
s32 BriefLineWidth(s32 width);
} // namespace PortHudScreen
