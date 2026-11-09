#include "hud_screen.hpp"
#include "Camera.hpp"

#if defined(GTA2_PORT_SDL)
bool port_SetHudGroup(s32 group);
bool port_HudScreenDue();
s32 port_HudScreenWidth();
#endif

namespace PortHudScreen
{
namespace
{
// The brief's text starts at this x of the layout (Hud_Brief::DrawBrief), right of the face.
const s32 kBriefTextX = 64;
const s32 kBriefRightMargin = 4;

bool sLayoutPixels;
bool sDrawn = true;
Fix16 sUiScale;
} // namespace

void SetGroup(Group group)
{
#if defined(GTA2_PORT_SDL)
    const bool taken = port_SetHudGroup(group);
    sDrawn = !taken || port_HudScreenDue();
#else
    const bool taken = false;
#endif
    if (taken == sLayoutPixels)
    {
        return;
    }
    // The HUD's sprites and texts take their size from the UI scale: 1 keeps their pixels.
    if (taken)
    {
        sUiScale = gViewCamera->mUiScale;
        gViewCamera->mUiScale = Fix16(1);
    }
    else
    {
        gViewCamera->mUiScale = sUiScale;
    }
    sLayoutPixels = taken;
}

bool LayoutPixels()
{
    return sLayoutPixels;
}

bool Drawn()
{
    return sDrawn;
}

s32 BriefLineWidth(s32 width)
{
#if defined(GTA2_PORT_SDL)
    const s32 screen = port_HudScreenWidth() - kBriefTextX - kBriefRightMargin;
    return screen > 0 && screen < width ? screen : width;
#else
    return width;
#endif
}
} // namespace PortHudScreen
