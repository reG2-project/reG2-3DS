// What the minimap shows, read from the game's state. Apart from minimap.cpp: the game's headers and the
// renderer's (gta2_api.hpp) define the same DirectX-era structs.
#pragma once

#include "types.hpp"

namespace MinimapGame
{
const s32 kMapSize = 256;
const s32 kMaxMarks = 40;

enum MarkKind
{
    kMissionPhone,
    kInfoPhone,
};

struct Mark
{
    s32 x4, y4; // quarter blocks
    u32 rgba;
    MarkKind kind;
};

struct View
{
    s32 px4, py4;     // the player, in quarter blocks
    s32 heading;      // Ang16: 1440 to the turn, anticlockwise
    const void* gang; // the gang whose territory the player is in, 0 for none
    u32 gangRgba;
    Mark marks[kMaxMarks];
    s32 markCount;
};

// False without a player in a city.
bool ReadView(View* pView);
// Each block's top as a colour (RGB565, by block kind with a little of its lid's colour) and its height.
void ReadGround(u16* pColours, u8* pHeights);
// 1 for each block of the gang's territory, else 0; kMapSize x kMapSize.
void ReadTerritory(const void* pGang, u8* pInside);
} // namespace MinimapGame
