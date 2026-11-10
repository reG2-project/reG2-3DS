// The game's side of C3DRenderer's stereo: the game's headers and the renderer's clash.
#pragma once

namespace StereoGame
{
// The local player's depth as the game projects a sprite there (1 / rhw), 0 without a player.
float PlayerDepth();
} // namespace StereoGame
