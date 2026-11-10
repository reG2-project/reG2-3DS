// libctru before the game's headers, whose Windows names clash with its own.
#include <3ds.h>
#include "../src/sound_job.hpp"
#include "../src/frame_loop.hpp"
#include "../src/pause_menu.hpp"
#include "log_3ds.hpp"
#include "Game.hpp"
#include "sound_obj.hpp"
#include <stdlib.h>
#include <string.h>

namespace PortSoundJob
{
namespace
{
bool sInit;
bool sOn;
bool sAllow;
Thread sThread;
LightLock sLock;
CondVar sWorkCv;
CondVar sDoneCv;
bool sBusy;
bool sPending;
bool sQuit;
u32 sFpscr;
u64 sQueuedTick;

// GTA2_3DS_STATS=<n>: every n passes, their wait for the system core, their run and the main thread's wait at the join.
u32 sInterval;
u32 sJobs;
u32 sJoinsWaited;
u64 sStartTicks;
u64 sStartMax;
u64 sRunTicks;
u64 sRunMax;
u64 sJoinTicks;
u64 sJoinMax;

double Ms(u64 ticks)
{
    return ticks * 1000.0 / SYSCLOCK_ARM11;
}

void WorkerMain(void*)
{
    LightLock_Lock(&sLock);
    while (!sQuit)
    {
        if (!sBusy)
        {
            CondVar_Wait(&sWorkCv, &sLock);
            continue;
        }
        const u32 fpscr = sFpscr;
        LightLock_Unlock(&sLock);
        // The main thread's rounding and flush modes, for the same results.
        __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
        const u64 t0 = svcGetSystemTick();
        gSound_obj.ServiceSoundEffects();
        const u64 t1 = svcGetSystemTick();
        LightLock_Lock(&sLock);
        // Azahar keeps a tick count per core: they can be a little apart.
        const u64 start = t0 > sQueuedTick ? t0 - sQueuedTick : 0;
        sStartTicks += start;
        sStartMax = start > sStartMax ? start : sStartMax;
        sRunTicks += t1 - t0;
        sRunMax = t1 - t0 > sRunMax ? t1 - t0 : sRunMax;
        sBusy = false;
        CondVar_Broadcast(&sDoneCv);
    }
    LightLock_Unlock(&sLock);
}

void Stop()
{
    if (!sThread)
    {
        return;
    }
    LightLock_Lock(&sLock);
    while (sBusy)
    {
        CondVar_Wait(&sDoneCv, &sLock);
    }
    sQuit = true;
    CondVar_Signal(&sWorkCv);
    LightLock_Unlock(&sLock);
    threadJoin(sThread, U64_MAX);
    threadFree(sThread);
    sThread = NULL;
}

void Init()
{
    sInit = true;
    const char* pOn = getenv("GTA2_3DS_SOUND_CORE1");
    const char* pInterval = getenv("GTA2_3DS_STATS");
    sInterval = pInterval ? (u32)atoi(pInterval) : 0;
    if (!(pOn && !strcmp(pOn, "1")))
    {
        return;
    }
    LightLock_Init(&sLock);
    CondVar_Init(&sWorkCv);
    CondVar_Init(&sDoneCv);
    // On the system core, of which the application gets 30 %, as the texture worker, below the stream feeder; the
    // stack of the feeder, as the pass may open a stream file.
    if (R_SUCCEEDED(APT_SetAppCpuTimeLimit(30)))
    {
        s32 priority = 0x30;
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        sThread = threadCreate(WorkerMain, NULL, 64 * 1024, priority < 0x3F ? priority + 1 : 0x3F, 1, false);
    }
    sOn = sThread != NULL;
    if (sOn)
    {
        atexit(Stop);
    }
    Log3DS_Printf("sound job: %s\n", sOn ? "on the system core" : "no thread, on the game's core");
}
} // namespace

bool Defer()
{
    if (!sInit)
    {
        Init();
    }
    // The draw's interpolation moves sprites, and the pause menu plays sounds and changes settings while it draws.
    if (!sOn || !sAllow || !gGame_0x40 || gGame_0x40->mGameState != GameState::Running_1 || PortPauseMenu::IsOpen() ||
        PortFrameLoop::Interpolated())
    {
        return false;
    }
    u32 fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    LightLock_Lock(&sLock);
    sFpscr = fpscr;
    sQueuedTick = svcGetSystemTick();
    sBusy = true;
    sPending = true;
    CondVar_Signal(&sWorkCv);
    LightLock_Unlock(&sLock);
    return true;
}

void Join()
{
    if (!sOn)
    {
        return;
    }
    LightLock_Lock(&sLock);
    if (!sPending)
    {
        LightLock_Unlock(&sLock);
        return;
    }
    const u64 t0 = svcGetSystemTick();
    const bool waited = sBusy;
    while (sBusy)
    {
        CondVar_Wait(&sDoneCv, &sLock);
    }
    const u64 wait = waited ? svcGetSystemTick() - t0 : 0;
    sPending = false;
    sJobs++;
    sJoinsWaited += waited ? 1 : 0;
    sJoinTicks += wait;
    sJoinMax = wait > sJoinMax ? wait : sJoinMax;
    LightLock_Unlock(&sLock);
    if (sInterval && sJobs >= sInterval)
    {
        Log3DS_Printf("sound job: %lu passes, start after %.3f ms (max %.3f), run %.3f ms (max %.3f), join waited in %lu, "
                      "%.3f ms per pass (max %.3f)\n",
                      sJobs, Ms(sStartTicks) / sJobs, Ms(sStartMax), Ms(sRunTicks) / sJobs, Ms(sRunMax), sJoinsWaited,
                      Ms(sJoinTicks) / sJobs, Ms(sJoinMax));
        sJobs = 0;
        sJoinsWaited = 0;
        sStartTicks = sStartMax = sRunTicks = sRunMax = sJoinTicks = sJoinMax = 0;
    }
}

void AllowInStep(bool allow)
{
    sAllow = allow;
}
} // namespace PortSoundJob
