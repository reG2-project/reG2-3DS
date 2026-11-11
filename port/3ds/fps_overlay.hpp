// SHOW FPS (or GTA2_3DS_FPS=1): frames and logic steps per second, the main thread's CPU time per frame and the memory in use,
// in the top screen's corner, refreshed a few times per second.
#pragma once

#include "types.hpp"

class IRenderer;
struct RTexture;

namespace FpsOverlay
{
// Where the overlay goes, in top screen pixels.
const s32 kX = 2;
const s32 kY = 2;

// Once per drawn frame: the overlay's texture and its size in pixels, null when it is off.
RTexture* Update(IRenderer& renderer, s32* pW, s32* pH);
void Shutdown(IRenderer& renderer);
} // namespace FpsOverlay
