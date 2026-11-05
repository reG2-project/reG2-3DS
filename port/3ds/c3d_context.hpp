// The 3DS context of CreateGLContext: citro3d and the top screen's render target (240x400, rotated).
#pragma once

#include <citro3d.h>

// Null until CreateGLContext has succeeded.
C3D_RenderTarget* C3DContextTarget();
