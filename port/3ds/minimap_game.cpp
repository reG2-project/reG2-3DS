#include "minimap_game.hpp"
#include "../src/pause_menu.hpp"
#include "Game.hpp"
#include "Gang.hpp"
#include "Hud.hpp"
#include "Object_5C.hpp"
#include "Ped.hpp"
#include "Player.hpp"
#include "ScriptManager.hpp"
#include "ScriptThread.hpp"
#include "gtx_0x106C.hpp"
#include "map_0x370.hpp"
#include "sprite.hpp"
#include <math.h>
#include <string.h>
#include <vector>

namespace MinimapGame
{
namespace
{
struct TileInfo
{
    bool known;
    bool clear; // mostly see-through: not the top of its column
    u8 r, g, b;
};

u32 Rgba(u32 r, u32 g, u32 b, u32 a = 255)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

u32 Clamp255(s32 v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (u32)v;
}

const TileInfo& Tile(std::vector<TileInfo>& tiles, u16 tile)
{
    TileInfo& t = tiles[tile];
    if (t.known)
    {
        return t;
    }
    // Tiles are 64x64 bytes, four side by side in rows of 256; palette entries are 64 dwords apart.
    const u8* pTexels = gGtx_0x106C->get_tile(tile);
    const u32* pPal = reinterpret_cast<const u32*>(gGtx_0x106C->GetPalData(gGtx_0x106C->get_phys_pal(tile)));
    u32 r = 0, g = 0, b = 0, n = 0;
    for (s32 y = 0; y < 64; y++)
    {
        for (s32 x = 0; x < 64; x++)
        {
            const u8 index = pTexels[y * 256 + x];
            if (index)
            {
                const u32 c = pPal[index * 64];
                r += (c >> 16) & 0xFF;
                g += (c >> 8) & 0xFF;
                b += c & 0xFF;
                n++;
            }
        }
    }
    t.known = true;
    t.clear = n < 64 * 64 * 4 / 10;
    t.r = (u8)(n ? r / n : 0);
    t.g = (u8)(n ? g / n : 0);
    t.b = (u8)(n ? b / n : 0);
    return t;
}

// The gang of the first gang zone holding the block, as Map_0x370::GetGangAtCoords finds it (without its
// iteration state).
const Gang* GangAt(s32 x, s32 y)
{
    const gmp_zone_list* pZones = gMap_0x370->mpZones;
    if (!gMap_0x370->mpZoneData || !pZones || x < 0 || y < 0 || x >= kMapSize || y >= kMapSize)
    {
        return 0;
    }
    for (s32 i = 0; i < pZones->mNumZones; i++)
    {
        const gmp_map_zone* pZone = pZones->mZoneArray[i];
        if (pZone->mZoneType == Gang_14 && pZone->ContainsPoint((u8)x, (u8)y))
        {
            return gGangPool_CA8->FindGangByName(pZone->mName);
        }
    }
    return 0;
}

// A gang the respect meters show (GangPool::FirstGang), with an arrow colour of the HUD's.
bool IsShown(const Gang& gang)
{
    return gang.mUsed && gang.HasKillRespectChange() && gang.mArrowColour >= 1 && gang.mArrowColour <= 7;
}

// The bright half of the arrow that points to a gang's phones (user sprite `arrow`, as Hud_Arrow::DrawArrow
// draws it): the colour the game gives the gang.
u32 ScanArrowColour(s32 arrow)
{
    static u32 texels[64 * 64];
    s32 w, h;
    if (!ReadUserSprite(arrow, texels, 64 * 64, &w, &h))
    {
        return 0;
    }
    u32 sum = 0, n = 0;
    for (s32 i = 0; i < w * h; i++)
    {
        if (texels[i] >> 24)
        {
            sum += (texels[i] & 0xFF) + ((texels[i] >> 8) & 0xFF) + ((texels[i] >> 16) & 0xFF);
            n++;
        }
    }
    u32 r = 0, g = 0, b = 0, m = 0;
    for (s32 i = 0; i < w * h; i++)
    {
        const u32 c = texels[i];
        if ((c >> 24) && ((c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF)) * n >= sum)
        {
            r += c & 0xFF;
            g += (c >> 8) & 0xFF;
            b += (c >> 16) & 0xFF;
            m++;
        }
    }
    return m ? Rgba(r / m, g / m, b / m) : 0;
}

const s32 kMaxSavePoints = 4;

s32 Quarter(Fix16 v)
{
    return (s32)floorf(v.ToFloat() * 4.0f + 0.5f);
}

void AddMark(View* pView, s32 x4, s32 y4, MarkKind kind, u8 arrow, u8 pip = 0)
{
    if (pView->markCount < kMaxMarks)
    {
        Mark& m = pView->marks[pView->markCount++];
        m.x4 = x4;
        m.y4 = y4;
        m.kind = kind;
        m.arrow = arrow;
        m.pip = pip;
    }
}

// The gang's live phones, under the game's phone arrow rules (Hud_Arrow::CheckVisibility): in its territory, not
// on a mission or in an arrow blocker zone, and a mission phone only with the respect it asks for.
void FindPhones(View* pView, Player* pPlayer, const Gang* pGang)
{
    if (!pGang || pPlayer->get_arrow_blocker_zone() || !gScriptManager || gScriptManager->IsOnMission() || !gHud_2B00 ||
        !gHud_2B00->mArrows.ShowGangArrows())
    {
        return;
    }
    if (pGang->mInfoPhoneX.mValue || pGang->mInfoPhoneY.mValue)
    {
        AddMark(pView, Quarter(pGang->mInfoPhoneX), Quarter(pGang->mInfoPhoneY), kInfoPhone, pGang->mArrowColour);
    }
    const s8 respect = pGang->GetRespectForPlayer(pPlayer->get_idx());
    for (u32 i = 0; i < GTA2_COUNTOF(gScriptManager->mPhoneIds); i++)
    {
        const u16 id = gScriptManager->mPhoneIds[i];
        if (!id || ((u32)gGameSave.mCarAndScriptData.mFlags >> i) & 1)
        {
            continue;
        }
        const SCR_POINTER* pPointer = static_cast<const SCR_POINTER*>(gScriptManager->GetBasePointer(id));
        const Object* pObj = pPointer->mObj;
        if (!pObj || !pObj->mSprite || !pObj->IsModel176To181())
        {
            continue;
        }
        const s32 type = GetPhoneTypeFromObjModel(pObj->mModel);
        const Fix16 x = pObj->mSprite->GetXPos(), y = pObj->mSprite->GetYPos();
        if (respect < (s8)GetMinRespectForPhoneType(type) || GangAt(x.ToInt(), y.ToInt()) != pGang)
        {
            continue;
        }
        // The pip by phone type, as Hud_Arrow::DrawArrow picks it: 1 yellow, 3 green, 2 red.
        const u8 pip = type == 1 ? 0 : type == 3 ? 1 : 2;
        AddMark(pView, Quarter(x), Quarter(y), kMissionPhone, pGang->mArrowColour, pip);
    }
}

// Each gang's nearest block to the player's block (its middle): the nearest point of each of its zones, kept if
// no earlier zone of another gang owns it. Kept while the player stays on a block. Leaves out the player's gang.
void FindTerritories(View* pView)
{
    const gmp_zone_list* pZones = gMap_0x370->mpZones;
    if (!gMap_0x370->mpZoneData || !pZones)
    {
        return;
    }
    static const void* sZones;
    static s32 sX = -1, sY = -1;
    static u8 sArrows[kMaxGangs];
    static Mark sMarks[kMaxGangs];
    static s32 sSlots[kMaxGangs];
    static s32 sCount;
    const s32 px = pView->px4 >> 2, py = pView->py4 >> 2;
    if (pZones != sZones || px != sX || py != sY || memcmp(pView->arrows, sArrows, sizeof(sArrows)))
    {
        sZones = pZones;
        sX = px;
        sY = py;
        memcpy(sArrows, pView->arrows, sizeof(sArrows));
        sCount = 0;
        for (s32 slot = 0; slot < kMaxGangs; slot++)
        {
            const Gang* pGang = &gGangPool_CA8->mGangList[slot];
            if (!pView->arrows[slot])
            {
                continue;
            }
            s32 best = -1, bx = 0, by = 0;
            for (s32 i = 0; i < pZones->mNumZones; i++)
            {
                const gmp_map_zone* pZone = pZones->mZoneArray[i];
                if (pZone->mZoneType != Gang_14 || gGangPool_CA8->FindGangByName(pZone->mName) != pGang)
                {
                    continue;
                }
                const s32 x = px < pZone->mX ? pZone->mX : px >= pZone->mX + pZone->mW ? pZone->mX + pZone->mW - 1 : px;
                const s32 y = py < pZone->mY ? pZone->mY : py >= pZone->mY + pZone->mH ? pZone->mY + pZone->mH - 1 : py;
                const s32 d = (x - px) * (x - px) + (y - py) * (y - py);
                if ((best < 0 || d < best) && GangAt(x, y) == pGang)
                {
                    best = d;
                    bx = x;
                    by = y;
                }
            }
            if (best >= 0)
            {
                sSlots[sCount] = slot;
                Mark& m = sMarks[sCount++];
                m.x4 = bx * 4 + 2;
                m.y4 = by * 4 + 2;
                m.kind = kTerritory;
                m.arrow = pGang->mArrowColour;
                m.pip = 0;
            }
        }
    }
    for (s32 i = 0; i < sCount; i++)
    {
        if (sSlots[i] + 1 != pView->gang)
        {
            AddMark(pView, sMarks[i].x4, sMarks[i].y4, kTerritory, sMarks[i].arrow);
        }
    }
}

// The churches where the player saves: the middle of each DO_SAVE_GAME command's area, read once per script.
void FindSavePoints(View* pView)
{
    static char sScript[sizeof(ScriptManager::mFullScrFileName)];
    static s32 sX4[kMaxSavePoints], sY4[kMaxSavePoints], sCount;
    if (!gScriptManager)
    {
        return;
    }
    if (strcmp(sScript, gScriptManager->mFullScrFileName))
    {
        strcpy(sScript, gScriptManager->mFullScrFileName);
        sCount = 0;
        for (u16 i = 0; i < GTA2_COUNTOF(gScriptManager->mBasePointers) && sCount < kMaxSavePoints; i++)
        {
            const SCR_DO_SAVE_GAME* pCmd = static_cast<const SCR_DO_SAVE_GAME*>(gScriptManager->GetBasePointer(i));
            if (pCmd && pCmd->mType == SCRCMD_DO_SAVE_GAME)
            {
                sX4[sCount] = Quarter(pCmd->mRect.mPos.x);
                sY4[sCount++] = Quarter(pCmd->mRect.mPos.y);
            }
        }
    }
    for (s32 i = 0; i < sCount; i++)
    {
        AddMark(pView, sX4[i], sY4[i], kSavePoint, 0);
    }
}

// Sprite `sprite` (an index into all the style's sprites) as RGBA, as ReadUserSprite.
bool ReadSprite(u16 sprite, u32* pRgba, s32 maxTexels, s32* pW, s32* pH)
{
    const sprite_index* pIndex = gGtx_0x106C->get_sprite_index(sprite);
    if (!pIndex || !pIndex->mpData || pIndex->mWidth * pIndex->mHeight > maxTexels)
    {
        return false;
    }
    // Sprite pages are 256 bytes wide; palette entries are 64 dwords apart.
    const u32* pPal = reinterpret_cast<const u32*>(
        gGtx_0x106C->GetPalData(gGtx_0x106C->get_phys_pal(gGtx_0x106C->GetTruePalette(palette_types_enum::sprites_2, sprite))));
    *pW = pIndex->mWidth;
    *pH = pIndex->mHeight;
    for (s32 y = 0; y < pIndex->mHeight; y++)
    {
        for (s32 x = 0; x < pIndex->mWidth; x++)
        {
            const u8 index = pIndex->mpData[y * 256 + x];
            const u32 c = pPal[index * 64];
            pRgba[y * pIndex->mWidth + x] = index ? Rgba((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF) : 0;
        }
    }
    return true;
}
} // namespace

bool ReadView(View* pView)
{
    pView->markCount = 0;
    if (!gGame_0x40 || !gGame_0x40->mLocalPlayer || !gGangPool_CA8 || !gMap_0x370 || !gGtx_0x106C)
    {
        return false;
    }
    Player* pPlayer = gGame_0x40->mLocalPlayer;
    Fix16 x, y, z;
    pPlayer->get_pos(&x, &y, &z);
    pView->px4 = Quarter(x);
    pView->py4 = Quarter(y);
    const Ped* pPed = pPlayer->GetActivePed();
    pView->heading = pPed ? pPed->GetRotation().rValue : 0;
    for (s32 i = 0; i < kMaxGangs; i++)
    {
        const Gang& gang = gGangPool_CA8->mGangList[i];
        pView->arrows[i] = IsShown(gang) ? gang.mArrowColour : 0;
    }
    const Gang* pGang = pPlayer->get_gang_curr_location();
    const s32 slot = pGang ? (s32)(pGang - gGangPool_CA8->mGangList) : -1;
    pView->gang = slot >= 0 && slot < kMaxGangs && pView->arrows[slot] ? slot + 1 : 0;
    FindSavePoints(pView);
    if (pView->gang)
    {
        FindPhones(pView, pPlayer, pGang);
    }
    FindTerritories(pView);
    return true;
}

u16 ReadGround(u16* pColours, u8* pHeights)
{
    gmp_compressed_map_32* pDmap = gMap_0x370->mpDmap;
    std::vector<TileInfo> tiles(1024);
    memset(&tiles[0], 0, tiles.size() * sizeof(TileInfo));
    std::vector<u16> lids(kMapSize * kMapSize);
    u32 wr = 0, wg = 0, wb = 0, wn = 0;
    for (s32 y = 0; y < kMapSize; y++)
    {
        for (s32 x = 0; x < kMapSize; x++)
        {
            const gmp_col_info* pCol = reinterpret_cast<const gmp_col_info*>(&pDmap->mpColumn[*pDmap->get_base(y, x)]);
            s32 z = 0;
            u16 lid = 0;
            for (s32 i = pCol->mHeight - pCol->mOffset - 1; i >= 0; i--)
            {
                const gmp_block_info* pBlock = pDmap->get_block(pCol->mBlockd[i]);
                const u16 t = pBlock->mLid & 0x3FF;
                if (t && (i + pCol->mOffset == 0 || !Tile(tiles, t).clear))
                {
                    z = i + pCol->mOffset;
                    lid = t;
                    break;
                }
            }
            if (lid && gGtx_0x106C->IsWaterTile(lid))
            {
                const TileInfo& t = Tile(tiles, lid);
                wr += t.r;
                wg += t.g;
                wb += t.b;
                wn++;
            }
            pHeights[y * kMapSize + x] = (u8)z;
            lids[y * kMapSize + x] = lid;
        }
    }
    // Where no block has a lid, the city's mean water colour.
    const u16 none = wn ? (u16)((((wr / wn) >> 3) << 11) | (((wg / wn) >> 2) << 5) | ((wb / wn) >> 3)) : 0;
    for (s32 i = 0; i < kMapSize * kMapSize; i++)
    {
        if (!lids[i])
        {
            pColours[i] = none;
            continue;
        }
        // The lid's mean colour, lighter as the block stands taller (as in the minimap study's lid render).
        const TileInfo& t = Tile(tiles, lids[i]);
        const s32 h = pHeights[i] - 2, k = 208 + 12 * (h < 0 ? 0 : h > 6 ? 6 : h);
        pColours[i] = (u16)(((Clamp255(t.r * k >> 8) >> 3) << 11) | ((Clamp255(t.g * k >> 8) >> 2) << 5) | (Clamp255(t.b * k >> 8) >> 3));
    }
    return none;
}

void ReadOwners(const u8* pArrows, u8* pOwners)
{
    memset(pOwners, 0, kMapSize * kMapSize);
    const gmp_zone_list* pZones = gMap_0x370->mpZones;
    if (!gMap_0x370->mpZoneData || !pZones)
    {
        return;
    }
    // Last zone first: where zones overlap, the first one in the list owns the block.
    for (s32 i = pZones->mNumZones - 1; i >= 0; i--)
    {
        const gmp_map_zone* pZone = pZones->mZoneArray[i];
        if (pZone->mZoneType != Gang_14)
        {
            continue;
        }
        const Gang* pGang = gGangPool_CA8->FindGangByName(pZone->mName);
        const s32 slot = pGang ? (s32)(pGang - gGangPool_CA8->mGangList) : 0;
        const u8 v = pGang && pArrows[slot] ? (u8)(slot + 1) : 0;
        for (s32 y = pZone->mY; y < pZone->mY + pZone->mH && y < kMapSize; y++)
        {
            memset(pOwners + y * kMapSize + pZone->mX, v, (pZone->mX + pZone->mW < kMapSize ? pZone->mW : kMapSize - pZone->mX));
        }
    }
}

bool PauseMenuOpen()
{
    return PortPauseMenu::IsOpen();
}

u32 ArrowColour(s32 arrow)
{
    // Kept per style: a sprite scan per frame is wasted time.
    static const void* sStyle;
    static u32 sColours[8];
    if (gGtx_0x106C->mTiles != sStyle)
    {
        sStyle = gGtx_0x106C->mTiles;
        memset(sColours, 0, sizeof(sColours));
    }
    if (arrow < 1 || arrow > 7)
    {
        return 0;
    }
    if (!sColours[arrow])
    {
        sColours[arrow] = ScanArrowColour(arrow);
    }
    return sColours[arrow];
}
bool ReadUserSprite(s32 n, u32* pRgba, s32 maxTexels, s32* pW, s32* pH)
{
    return ReadSprite(gGtx_0x106C->GetSpriteTrueIndex(sprite_types_enum::user_6, (s16)n), pRgba, maxTexels, pW, pH);
}

bool ReadPauseFontGlyph(char c, u32* pRgba, s32 maxTexels, s32* pW, s32* pH)
{
    const u16 glyph = gGtx_0x106C->GetSpriteIdxFromFont(gStylePauseFont, (s16)(c - 33));
    return ReadSprite(gGtx_0x106C->GetSpriteTrueIndex(sprite_types_enum::font_7, (s16)glyph), pRgba, maxTexels, pW, pH);
}
} // namespace MinimapGame
