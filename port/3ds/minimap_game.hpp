// What the minimap shows, read from the game's state. Apart from minimap.cpp: the game's headers and the
// renderer's (gta2_api.hpp) define the same DirectX-era structs.
#pragma once

#include "types.hpp"

namespace MinimapGame
{
const s32 kMapSize = 256;
const s32 kMaxMarks = 40;
const s32 kMaxGangs = 10; // GangPool's slots
// The HUD's user sprites: a gang's phone arrow is sprite `arrow` (1 to 7) with a phone's pip, kPipSprite + pip, in
// its middle (Hud_Arrow::DrawArrow); its respect meter icon is kIconSprite + arrow (Hud_GangRespectBars).
const s32 kMaxArrow = 7;
const s32 kPipSprite = 8;
const s32 kPipCount = 3;
const s32 kIconSprite = 63;
// The respect meter's mission lamps under each bar: green, yellow, red (Hud_GangRespectBars).
const s32 kMeterLampSprite = 46;

enum MarkKind
{
    kMissionPhone,
    kInfoPhone,
    kTerritory, // a gang's nearest block, for each gang but the one whose territory the player is in
    kSavePoint, // a church where the player saves: a DO_SAVE_GAME command's area
};

struct Mark
{
    s32 x4, y4; // quarter blocks
    MarkKind kind;
    u8 arrow; // the gang's arrow colour
    u8 pip;   // a mission phone's pip: 0 yellow (medium), 1 green (easy), 2 red (hard)
};

struct View
{
    s32 px4, py4; // the player, in quarter blocks
    s32 heading;  // Ang16: 1440 to the turn
    s32 gang;     // 1 + the pool slot of the gang whose territory the player is in, 0 for none
    // That gang's opinion of the player, by the respect meter's bars: 1 likes (20 and over), -1 hates (-20 and under).
    s32 opinion;
    u8 arrows[kMaxGangs]; // each slot's arrow colour, 0 for a gang the respect meters leave out
    Mark marks[kMaxMarks];
    s32 markCount;
};

// False without a player in a city.
bool ReadView(View* pView);
bool PauseMenuOpen();
// Each block's top as a colour (RGB565, its lid tile's, lighter when taller) and its height; returns the colour
// given where no block has a lid.
u16 ReadGround(u16* pColours, u8* pHeights);
// Each block's gang, 1 + its pool slot when `arrows` keeps it, else 0; kMapSize x kMapSize.
void ReadOwners(const u8* pArrows, u8* pOwners);
// The colour of gang arrow `arrow` (RGBA), 0 for none.
u32 ArrowColour(s32 arrow);
// The style's user sprite `n` as RGBA, rows of *pW texels; false when it is missing or over maxTexels.
bool ReadUserSprite(s32 n, u32* pRgba, s32 maxTexels, s32* pW, s32* pH);
// The pause menu font's glyph for `c`, as ReadUserSprite.
bool ReadPauseFontGlyph(char c, u32* pRgba, s32 maxTexels, s32* pW, s32* pH);
} // namespace MinimapGame
