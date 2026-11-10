// The 3DS context of CreateGLContext: citro3d and the top screen's render target (240x400, rotated, 1:1).
#pragma once

#include <citro3d.h>

// Null until CreateGLContext has succeeded.
C3D_RenderTarget* C3DContextTarget();
// Stereo: gfxSet3D, and citro3d's frame end transfers the top target to the right eye's framebuffer (the renderer
// transfers the left eye's itself); else to the left one only. Called between C3D_FrameBegin and C3D_FrameEnd.
void C3DContextSetStereo(bool stereo);
// Queues the top target's display transfer to the left eye's framebuffer.
void C3DContextTransferLeft();
// The bottom screen's target (240x320, rotated, 1:1), for the HUD.
C3D_RenderTarget* C3DContextBottomTarget();
// C3D_FrameSync's wait for both screens' next refresh, unless the frame loop has waited for this frame's
// (port_WaitRefresh, frame_loop.cpp).
void C3DContextFrameSync();
// After a suspend: the refreshes while away are not counted.
void port_ResyncRefresh();
