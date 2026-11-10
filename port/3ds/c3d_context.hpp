// The 3DS context of CreateGLContext: citro3d and the top screen's render target (240x400, rotated, 1:1).
#pragma once

#include <citro3d.h>

// Null until CreateGLContext has succeeded.
C3D_RenderTarget* C3DContextTarget();
// The bottom screen's target (240x320, rotated, 1:1), for the HUD.
C3D_RenderTarget* C3DContextBottomTarget();
// C3D_FrameSync's wait for both screens' next refresh, the top's and then the bottom's timed apart.
void C3DContextFrameSync();
