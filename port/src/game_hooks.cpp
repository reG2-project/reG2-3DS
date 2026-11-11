// Game state the platform layer asks about (declared in compat/src/port_platform.hpp).
#include "Car.hpp"
#include "Frontend.hpp"
#include "Game.hpp"
#include "Ped.hpp"
#include "Player.hpp"
#include "PublicTransport.hpp"
#include "Camera.hpp"
#include "pause_menu.hpp"
#include "hud_screen.hpp"
#include "settings.hpp"
#include "sprite.hpp"

bool port_GameRunning()
{
    return gGame_0x40 && !gFrontend;
}

void port_TestCar(int model, float dx, float dy)
{
    Player* pPlayer = port_GameRunning() ? gGame_0x40->mLocalPlayer : NULL;
    Ped* pPed = pPlayer ? pPlayer->mPlayerPed : NULL;
    if (!pPed || !pPed->mGameObject || pPed->get_car())
    {
        return;
    }
    const Sprite* pSprite = pPed->GetSprite();
    Car* pCar = gCarManager_6C->SpawnCarAt(pSprite->mXy.x + Fix16(dx), pSprite->mXy.y + Fix16(dy), pSprite->mZpos, Ang16(0),
                                           model, Fix16(1));
    if (!pCar)
    {
        return;
    }
    // As the ENTER OR EXIT control does next to a car.
    pPed->SetObjective2(objectives_enum::no_obj_0);
    pPed->SetObjective(objectives_enum::enter_car_as_driver_35);
    pPed->SetTargetObjectiveCar(pCar);
    pPed->set_enter_car_as_passenger(0);
    pPed->set_target_car_door(0);
}

namespace
{
struct BoomCar
{
    Car* mpCar;
    s32 mPhase; // 0 parked, 1 burning, 2 blown up
};
BoomCar sBoom[32];

bool InPool(const Car* pCar)
{
    for (Car* p = gCar_BC_Pool ? gCar_BC_Pool->GetFirstCar_420E50() : NULL; p; p = p->mpNext)
    {
        if (p == pCar)
        {
            return true;
        }
    }
    return false;
}

void DamageTo(Car* pCar, s32 target)
{
    Fix16_Point centre(kFpZero_6777CC, kFpZero_6777CC);
    for (s32 i = 0; i < 400 && pCar->mDamage < target && !pCar->IsMaxDamage(); i++)
    {
        pCar->AccumulateDamage(200, &centre);
    }
}
} // namespace

int port_TestBoom(unsigned step, int count, float spacing)
{
    const unsigned kStagger = 20, kBurn = 75;
    count = count < 32 ? count : 32;
    Player* pPlayer = port_GameRunning() ? gGame_0x40->mLocalPlayer : NULL;
    Ped* pPed = pPlayer ? pPlayer->mPlayerPed : NULL;
    if (step == 0)
    {
        const Sprite* pSprite = pPed && pPed->mGameObject ? pPed->GetSprite() : NULL;
        for (s32 i = 0; i < count; i++)
        {
            const f32 dx = ((i / 2) - (count - 1) / 4.0f) * spacing;
            const f32 dy = i % 2 ? spacing : -spacing;
            sBoom[i].mPhase = 0;
            sBoom[i].mpCar = pSprite ? gCarManager_6C->SpawnCarAt(pSprite->mXy.x + Fix16(dx), pSprite->mXy.y + Fix16(dy),
                                                                  pSprite->mZpos, Ang16(0), i % 2 ? 50 : 42, Fix16(1))
                                     : NULL;
        }
    }
    s32 left = 0;
    for (s32 i = 0; i < count; i++)
    {
        BoomCar& b = sBoom[i];
        if (!b.mpCar || b.mPhase == 2 || !InPool(b.mpCar))
        {
            b.mpCar = NULL;
            continue;
        }
        left++;
        if (b.mPhase == 0 && step >= i * kStagger)
        {
            DamageTo(b.mpCar, 26000);
            b.mPhase = 1;
        }
        if (b.mPhase == 1 && step >= i * kStagger + kBurn)
        {
            DamageTo(b.mpCar, 32000);
            b.mPhase = 2;
        }
    }
    return left;
}

void port_TestWanted(int stars)
{
    Player* pPlayer = port_GameRunning() ? gGame_0x40->mLocalPlayer : NULL;
    Ped* pPed = pPlayer ? pPlayer->mPlayerPed : NULL;
    if (pPed && pPed->get_wanted_star_count() < stars)
    {
        pPed->set_wanted_star_count(static_cast<u8>(stars));
    }
}

int port_TestTrain(int index)
{
    Player* pPlayer = port_GameRunning() ? gGame_0x40->mLocalPlayer : NULL;
    Ped* pPed = pPlayer ? pPlayer->mPlayerPed : NULL;
    if (!pPed || !gPublicTransport_181C)
    {
        return 0;
    }
    if (pPed->IsInTrain())
    {
        pPlayer->mGameCamera.SetSmoothFollow();
        return 3;
    }
    if (pPed->get_objective() == objectives_enum::enter_train_37)
    {
        return 2;
    }
    for (s32 i = 0; i < 10; i++)
    {
        const Train& train = gPublicTransport_181C->mTrainArray[i];
        Car* pWagon = NULL;
        for (s32 w = 1; train.mCarriages[0] && w <= train.mWagonCount && !pWagon; w++)
        {
            pWagon = train.mCarriages[w] && train.mCarriages[w]->GetCarInfoIdx() == car_model_enum::TRAIN ? train.mCarriages[w] : NULL;
        }
        if (!pWagon || index-- > 0)
        {
            continue;
        }
        // Stop phase 7: doors open, the platform's peds boarding (PublicTransport::PublicTransportService).
        for (u8 door = 0; train.mStopPhase == 7 && pPed->mGameObject && door < static_cast<u8>(pWagon->GetDoorCount()); door++)
        {
            if (pWagon->IsDoorAccessible(door))
            {
                Fix16 x, y;
                pWagon->GetDoorWorldPosition(door, &x, &y);
                pPed->TeleportToCoord(x, y);
                pPlayer->mGameCamera.SetSnapFollow();
                // As the ENTER OR EXIT control does next to a train (Player::HandleControls).
                pPed->SetObjective2(objectives_enum::no_obj_0);
                pPed->SetObjective(objectives_enum::enter_train_37);
                pPed->SetTargetObjectiveCar(pWagon);
                pPed->set_enter_car_as_passenger(0);
                pPed->set_target_car_door(0);
                return 2;
            }
        }
        return 1;
    }
    return 0;
}

const char* port_TestTeleport(float x, float y, float degrees)
{
    Player* pPlayer = port_GameRunning() ? gGame_0x40->mLocalPlayer : NULL;
    Ped* pPed = pPlayer ? pPlayer->mPlayerPed : NULL;
    Car* pCar = pPed ? pPed->get_car() : NULL;
    if (!pPed || (!pCar && !pPed->mGameObject))
    {
        return "nobody";
    }
    if (pCar)
    {
        pCar->mCarSprite->set_ang_lazy(Ang16(static_cast<s32>(degrees * 4.0f) % 1440));
    }
    pPed->TeleportToCoord(Fix16(x), Fix16(y));
    return pCar ? (pCar->mPhysics ? "vehicle" : "parked vehicle") : "player";
}

bool port_FrontendActive()
{
    return gFrontend != NULL;
}

bool port_MenuUsesMouse()
{
    return port_FrontendActive() || PortPauseMenu::IsOpen();
}

bool port_QuitMessageShown()
{
    return port_GameRunning() && gGame_0x40->mLocalPlayer &&
           (gGame_0x40->mLocalPlayer->mShowQuitMessage || PortPauseMenu::IsOpen());
}

namespace
{
f32 ToF(Fix16 v)
{
    return v.mValue / 16384.0f;
}

Fix16 FromF(f32 v)
{
    return Fix16(static_cast<s32>(v * 16384.0f), 0);
}

// The 640 (or 480) wide layout scaled by the HUD scale, and what the safe area has beyond it: what lies
// in the outer sixths of the layout stays at that edge of the safe area, the rest is centred with it.
f32 Offset(f32 pos, f32 layoutSize, s32 screenSize)
{
    const f32 margin = screenSize * PortSettings::SafeArea();
    const f32 extra = screenSize - 2 * margin - layoutSize * ToF(gViewCamera->mUiScale);
    return margin + (pos < layoutSize / 6 ? 0.0f : (pos > layoutSize * 5 / 6 ? extra : extra / 2));
}
} // namespace

Fix16 PortUiX(Fix16 x)
{
    if (PortHudScreen::LayoutPixels())
    {
        return x;
    }
    return FromF(ToF(x) * ToF(gViewCamera->mUiScale) + Offset(ToF(x), 640.0f, gViewCamera->mScreenPxWidth));
}

Fix16 PortUiY(Fix16 y)
{
    if (PortHudScreen::LayoutPixels())
    {
        return y;
    }
    return FromF(ToF(y) * ToF(gViewCamera->mUiScale) + Offset(ToF(y), 480.0f, gViewCamera->mScreenPxHeight));
}

// Text centred in the layout stays centred; otherwise its start (left edge) or its end (right edge)
// decides, so text beside a face or an icon keeps its place.
Fix16 PortUiStringX(const wchar_t* pStr, Fix16 x, u16 font)
{
    if (PortHudScreen::LayoutPixels())
    {
        return x;
    }
    const f32 width = static_cast<f32>(Frontend::GetMaxTextWidth(const_cast<wchar_t*>(pStr), font));
    const f32 start = ToF(x);
    const f32 centred = (640.0f - width) / 2;
    f32 anchor = 320.0f;
    if (start - centred > 1.5f || centred - start > 1.5f)
    {
        anchor = start < 640.0f / 6 ? 0.0f : (start + width > 640.0f * 5 / 6 ? 640.0f : 320.0f);
    }
    return FromF(start * ToF(gViewCamera->mUiScale) + Offset(anchor, 640.0f, gViewCamera->mScreenPxWidth));
}

// The layout fits the safe area's height on wide screens, its width on tall ones. Scaled up, it can go
// past the height (its edges stay anchored) but not the width, which its texts span. In fixed point:
// the defaults give the scale the screen alone gives.
Fix16 PortHudUiScale(u32 width, u32 height)
{
    const Fix16 safe = FromF(1 - 2 * PortSettings::SafeArea());
    const Fix16 byWidth = Fix16(width) / 640 * safe;
    const Fix16 byHeight = Fix16(height) / 480 * safe;
    const Fix16 scale = (byHeight < byWidth ? byHeight : byWidth) * FromF(PortSettings::HudScale());
    return scale < byWidth ? scale : byWidth;
}

Fix16 PortScreenUiScale()
{
    const Fix16 byWidth = Fix16(gViewCamera->mScreenPxWidth) / 640;
    const Fix16 byHeight = Fix16(gViewCamera->mScreenPxHeight) / 480;
    return byHeight < byWidth ? byHeight : byWidth;
}

Fix16 PortViewWidthFactor()
{
    const s32 width = gViewCamera->mScreenPxWidth;
    const s32 width_4_3 = gViewCamera->mScreenPxHeight * 4 / 3;
    return width > width_4_3 && width_4_3 > 0 ? FromF(static_cast<f32>(width) / width_4_3) : Fix16(1);
}
