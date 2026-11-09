// The bottom screen's minimap, north up: the ground around the player, the gangs' territories, the phones of the one
// the player is in, from what the game holds in memory. Nothing secret: no pickups, tokens, frenzies or garages.
#pragma once

#include "hud_theme.hpp"

class C3DRenderer;

namespace Minimap
{
// Takes this frame's state; returns its signature, 0 when there is no map to show (no city or player).
u32 Update(C3DRenderer& renderer);
// Draws the map in the tube frame `frame` (outer edges, bottom screen pixels) after an Update that returned non 0.
void Draw(C3DRenderer& renderer, HudTheme::Sink& sink, const HudTheme::Rect& frame);
void Shutdown(C3DRenderer& renderer);
} // namespace Minimap
