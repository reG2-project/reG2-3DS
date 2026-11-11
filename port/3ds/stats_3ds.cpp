#include "stats_3ds.hpp"
#include "log_3ds.hpp"
#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>

namespace
{
u32 sInterval;
bool sInit;
u32 sFrames;
u64 sUploadTicks;
u32 sUploads;
u64 sFrameEndTicks;
u64 sFrameBeginTicks;
u64 sBottomTicks;
u64 sBottomMaxTicks;
u32 sBottomDrawn;
u64 sLastTick;
size_t sPeakHeap;
u32 sPeakLinear;
u32 sLinearTotal;
u64 sWaitTicks;
u32 sTexCount, sTexBytes, sCachedBytes, sEvictions, sAllocFails;
u32 sVramBytes, sCopies, sCopies0, sDrains;
bool sWorker;
u64 sWorkerTicks, sWorkerTicks0;
u32 sWorkerJobs, sWorkerJobs0, sMainJobs, sMainJobs0;
bool sCountSleeps;

double Ms(u64 ticks)
{
    return ticks * 1000.0 / SYSCLOCK_ARM11;
}

void Init()
{
    sInit = true;
    const char* pInterval = getenv("GTA2_3DS_STATS");
    sInterval = pInterval ? (u32)atoi(pInterval) : 0;
    sLinearTotal = envGetLinearHeapSize();
    bool isNew3DS = false;
    APT_CheckNew3DS(&isNew3DS);
    fprintf(stderr, "3ds: %s, memory type %lu, application region %lu KB, heap %lu KB, linear heap %lu KB\n",
            isNew3DS ? "New 3DS" : "Old 3DS", osGetApplicationMemType(), osGetMemRegionSize(MEMREGION_APPLICATION) / 1024,
            envGetHeapSize() / 1024, envGetLinearHeapSize() / 1024);
}
} // namespace

void Stats3DS_AddUpload(u64 ticks)
{
    sUploadTicks += ticks;
    sUploads++;
}

void Stats3DS_AddFrameBegin(u64 ticks)
{
    sFrameBeginTicks += ticks;
    sWaitTicks += ticks;
}

void Stats3DS_AddWait(u64 ticks)
{
    sWaitTicks += ticks;
}

void Stats3DS_AddBottom(u64 ticks, bool drawn)
{
    sBottomTicks += ticks;
    sBottomMaxTicks = ticks > sBottomMaxTicks ? ticks : sBottomMaxTicks;
    sBottomDrawn += drawn ? 1 : 0;
}

void Stats3DS_SetTextures(u32 count, u32 bytes, u32 cachedBytes, u32 evictions, u32 failures)
{
    sTexCount = count;
    sTexBytes = bytes;
    sCachedBytes = cachedBytes;
    sEvictions = evictions;
    sAllocFails = failures;
}

void Stats3DS_SetVram(u32 bytes, u32 copies, u32 drains)
{
    sVramBytes = bytes;
    sCopies = copies;
    sDrains = drains;
}

void Stats3DS_SetUploadWorker(u64 ticks, u32 jobs, u32 mainJobs)
{
    sWorker = true;
    sWorkerTicks = ticks;
    sWorkerJobs = jobs;
    sMainJobs = mainJobs;
}

void Stats3DS_Frame(u64 frameEndTicks)
{
    sWaitTicks += frameEndTicks;
    if (!sInit)
    {
        Init();
    }
    if (!sInterval)
    {
        return;
    }
    sFrameEndTicks += frameEndTicks;
    const struct mallinfo mi = mallinfo();
    if ((size_t)mi.uordblks > sPeakHeap)
    {
        sPeakHeap = mi.uordblks;
    }
    const u32 linearUsed = sLinearTotal - linearSpaceFree();
    if (linearUsed > sPeakLinear)
    {
        sPeakLinear = linearUsed;
    }
    if (++sFrames % sInterval)
    {
        return;
    }
    const u64 now = svcGetSystemTick();
    if (sLastTick)
    {
        const double n = sInterval;
        Log3DS_Printf("3ds: %.1f ms per frame, frame begin %.2f ms, end %.2f ms, uploads %.1f per frame %.2f ms, bottom %.3f ms (max %.3f, drawn %lu of %lu); heap %u KB (peak %u KB, arena %u KB), linear %lu KB (peak %lu KB, free %lu KB); textures %lu, %lu KB (evictable %lu KB, VRAM %lu KB, free %lu KB), evicted %lu, failed %lu, VRAM copies %.1f per frame, waits %lu\n",
                Ms(now - sLastTick) / n, Ms(sFrameBeginTicks) / n, Ms(sFrameEndTicks) / n, sUploads / n, Ms(sUploadTicks) / n, Ms(sBottomTicks) / n, Ms(sBottomMaxTicks), sBottomDrawn, sInterval, mi.uordblks / 1024,
                (u32)(sPeakHeap / 1024), mi.arena / 1024, linearUsed / 1024, sPeakLinear / 1024, (u32)linearSpaceFree() / 1024, sTexCount, sTexBytes / 1024, sCachedBytes / 1024, sVramBytes / 1024, (u32)vramSpaceFree() / 1024, sEvictions, sAllocFails,
                (sCopies - sCopies0) / n, sDrains);
        if (sWorker)
        {
            Log3DS_Printf("3ds: upload worker %.1f %% of the time (%.2f ms per frame), %lu jobs; %lu written on the main thread\n",
                          (sWorkerTicks - sWorkerTicks0) * 100.0 / (now - sLastTick), Ms(sWorkerTicks - sWorkerTicks0) / n,
                          sWorkerJobs - sWorkerJobs0, sMainJobs - sMainJobs0);
        }
    }
    sCopies0 = sCopies;
    sWorkerTicks0 = sWorkerTicks;
    sWorkerJobs0 = sWorkerJobs;
    sMainJobs0 = sMainJobs;
    sLastTick = now;
    sFrameEndTicks = 0;
    sFrameBeginTicks = 0;
    sUploadTicks = 0;
    sUploads = 0;
    sBottomTicks = 0;
    sBottomMaxTicks = 0;
    sBottomDrawn = 0;
}

void Stats3DS_CountSleeps(bool on)
{
    sCountSleeps = on;
}

u64 Stats3DS_WaitTicks()
{
    return sWaitTicks;
}

// Linked in place of svcSleepThread (--wrap). libctru's threads have a Thread, the main thread none.
extern "C" void __real_svcSleepThread(s64 ns);
extern "C" void __wrap_svcSleepThread(s64 ns)
{
    if (!sCountSleeps || threadGetCurrent())
    {
        __real_svcSleepThread(ns);
        return;
    }
    const u64 t0 = svcGetSystemTick();
    __real_svcSleepThread(ns);
    sWaitTicks += svcGetSystemTick() - t0;
}
