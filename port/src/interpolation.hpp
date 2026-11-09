// Drawing between two logic steps: the sprites around the drawn view (cars, peds, objects, particles) and the
// view camera are drawn at prev + (cur - prev) * alpha, then put back exactly, so the logic only ever sees whole steps.
#pragma once

#include "types.hpp"

namespace PortInterp
{
// Before a logic step: the positions it starts from.
void Snapshot();
// Nothing to interpolate from (load, new game).
void Clear();
// Around a draw; alpha in [0, 1] is the fraction of the next step already elapsed.
void Apply(f32 alpha);
// Once the draw's view is set: the area the next snapshot keeps the sprites of.
void Drawn();
void Restore();
// Sprites drawn in between, drawn where they are after a jump, drawn unturned after a snap round; then zeroed.
void TakeCounts(u32* pMoved, u32* pJumped, u32* pSnapped);
} // namespace PortInterp
