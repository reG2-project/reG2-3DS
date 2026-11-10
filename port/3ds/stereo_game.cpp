#include "stereo_game.hpp"
#include "Camera.hpp"
#include "Game.hpp"
#include "Player.hpp"

namespace StereoGame
{
float PlayerDepth()
{
    if (!gGame_0x40 || !gGame_0x40->mLocalPlayer || !gViewCamera)
    {
        return 0.0f;
    }
    Fix16 x, y, z;
    gGame_0x40->mLocalPlayer->get_pos(&x, &y, &z);
    // sprite.cpp's rhw: 1 / (camera z + 8 - z).
    return gViewCamera->mCurPos.mZ.ToFloat() + 8.0f - z.ToFloat();
}
} // namespace StereoGame
