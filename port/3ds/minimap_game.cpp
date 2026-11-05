#include "minimap_game.hpp"
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

// Water, road, pavement, field, building (lighter when taller): light streets between darker blocks, under the
// HUD's dark panels.
void KindColour(s32 z, s32 type, bool water, s32* pR, s32* pG, s32* pB)
{
    static const u8 kWater[3] = {28, 52, 88}, kRoad[3] = {146, 142, 130}, kPavement[3] = {96, 93, 86};
    static const u8 kField[3] = {62, 96, 44}, kBuilding[3] = {56, 62, 74};
    const u8* c = kPavement;
    s32 lift = 0;
    if (water || z <= 0)
    {
        c = kWater;
    }
    else if (type == ROAD)
    {
        c = kRoad;
    }
    else if (z >= 3)
    {
        c = kBuilding;
        lift = 12 * (z - 3 < 6 ? z - 3 : 6);
    }
    else if (type == FIELD)
    {
        c = kField;
    }
    *pR = c[0] + lift;
    *pG = c[1] + lift;
    *pB = c[2] + lift;
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

// The gang's colour: the bright half of its respect meter's bar (user sprite 71 + arrow - 1, as
// Hud_GangRespectBars draws it); 0 for a gang without an arrow colour, which is not a real gang.
u32 GangColour(const Gang* pGang)
{
    if (!pGang || !pGang->mArrowColour)
    {
        return 0;
    }
    const u16 sprite = gGtx_0x106C->GetSpriteTrueIndex(sprite_types_enum::user_6, (s16)(pGang->mArrowColour - 1 + 71));
    const sprite_index* pIndex = gGtx_0x106C->get_sprite_index(sprite);
    const u32* pPal = reinterpret_cast<const u32*>(
        gGtx_0x106C->GetPalData(gGtx_0x106C->get_phys_pal(gGtx_0x106C->GetTruePalette(palette_types_enum::sprites_2, sprite))));
    u32 sum = 0, n = 0;
    for (s32 pass = 0; pass < 2; pass++)
    {
        const u32 mean = n ? sum / n : 0;
        u32 r = 0, g = 0, b = 0, m = 0;
        sum = n = 0;
        for (s32 y = 0; y < pIndex->mHeight; y++)
        {
            for (s32 x = 0; x < pIndex->mWidth; x++)
            {
                const u8 index = pIndex->mpData[y * 256 + x];
                if (!index)
                {
                    continue;
                }
                const u32 c = pPal[index * 64];
                const u32 l = ((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF);
                sum += l;
                n++;
                if (pass && l >= mean)
                {
                    r += (c >> 16) & 0xFF;
                    g += (c >> 8) & 0xFF;
                    b += c & 0xFF;
                    m++;
                }
            }
        }
        if (pass && m)
        {
            return Rgba(r / m, g / m, b / m);
        }
    }
    return 0;
}

s32 Quarter(Fix16 v)
{
    return (s32)floorf(v.ToFloat() * 4.0f + 0.5f);
}

void AddMark(View* pView, Fix16 x, Fix16 y, u32 rgba, MarkKind kind)
{
    if (pView->markCount < kMaxMarks)
    {
        Mark& m = pView->marks[pView->markCount++];
        m.x4 = Quarter(x);
        m.y4 = Quarter(y);
        m.rgba = rgba;
        m.kind = kind;
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
        AddMark(pView, pGang->mInfoPhoneX, pGang->mInfoPhoneY, pView->gangRgba, kInfoPhone);
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
        // By phone type: yellow medium, red hard, green easy.
        static const u32 kPhone[4] = {0, Rgba(244, 210, 40), Rgba(226, 46, 32), Rgba(70, 206, 60)};
        AddMark(pView, x, y, kPhone[type & 3], kMissionPhone);
    }
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
    const Gang* pGang = pPlayer->get_gang_curr_location();
    // The colour of the last gang, kept: a sprite scan per frame is wasted time.
    static const Gang* sColourGang;
    static const void* sColourStyle;
    static u8 sColourArrow;
    static u32 sColour;
    const u8 arrow = pGang ? pGang->mArrowColour : 0;
    if (pGang != sColourGang || gGtx_0x106C->mTiles != sColourStyle || arrow != sColourArrow)
    {
        sColourGang = pGang;
        sColourStyle = gGtx_0x106C->mTiles;
        sColourArrow = arrow;
        sColour = GangColour(pGang);
    }
    pView->gangRgba = sColour;
    pView->gang = pView->gangRgba ? pGang : 0;
    FindPhones(pView, pPlayer, static_cast<const Gang*>(pView->gang));
    return true;
}

void ReadGround(u16* pColours, u8* pHeights)
{
    gmp_compressed_map_32* pDmap = gMap_0x370->mpDmap;
    std::vector<TileInfo> tiles(1024);
    memset(&tiles[0], 0, tiles.size() * sizeof(TileInfo));
    for (s32 y = 0; y < kMapSize; y++)
    {
        for (s32 x = 0; x < kMapSize; x++)
        {
            const gmp_col_info* pCol = reinterpret_cast<const gmp_col_info*>(&pDmap->mpColumn[*pDmap->get_base(y, x)]);
            s32 z = 0, type = AIR;
            u16 lid = 0;
            for (s32 i = pCol->mHeight - pCol->mOffset - 1; i >= 0; i--)
            {
                const gmp_block_info* pBlock = pDmap->get_block(pCol->mBlockd[i]);
                const u16 t = pBlock->mLid & 0x3FF;
                if (t && (i + pCol->mOffset == 0 || !Tile(tiles, t).clear))
                {
                    z = i + pCol->mOffset;
                    type = pBlock->mSlopeType & 3;
                    lid = t;
                    break;
                }
            }
            s32 r, g, b;
            KindColour(z, type, lid && gGtx_0x106C->IsWaterTile(lid), &r, &g, &b);
            if (lid)
            {
                // 15% of the lid's colour, scaled to the kind's brightness: some texture, the same tone.
                const TileInfo& t = Tile(tiles, lid);
                const s32 kind = r + g + b, tex = t.r + t.g + t.b > 24 ? t.r + t.g + t.b : 24;
                r = (r * 85 + t.r * kind * 15 / tex) / 100;
                g = (g * 85 + t.g * kind * 15 / tex) / 100;
                b = (b * 85 + t.b * kind * 15 / tex) / 100;
            }
            pHeights[y * kMapSize + x] = (u8)z;
            pColours[y * kMapSize + x] = (u16)(((Clamp255(r) >> 3) << 11) | ((Clamp255(g) >> 2) << 5) | (Clamp255(b) >> 3));
        }
    }
}

void ReadTerritory(const void* pGang, u8* pInside)
{
    memset(pInside, 0, kMapSize * kMapSize);
    const gmp_zone_list* pZones = gMap_0x370->mpZones;
    if (!pGang || !gMap_0x370->mpZoneData || !pZones)
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
        const u8 v = gGangPool_CA8->FindGangByName(pZone->mName) == pGang;
        for (s32 y = pZone->mY; y < pZone->mY + pZone->mH && y < kMapSize; y++)
        {
            memset(pInside + y * kMapSize + pZone->mX, v, (pZone->mX + pZone->mW < kMapSize ? pZone->mW : kMapSize - pZone->mX));
        }
    }
}
} // namespace MinimapGame
