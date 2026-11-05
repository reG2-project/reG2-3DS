#include "stats_3ds.hpp"
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
}

void Stats3DS_AddBottom(u64 ticks, bool drawn)
{
    sBottomTicks += ticks;
    sBottomMaxTicks = ticks > sBottomMaxTicks ? ticks : sBottomMaxTicks;
    sBottomDrawn += drawn ? 1 : 0;
}

void Stats3DS_Frame(u64 frameEndTicks)
{
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
        fprintf(stderr, "3ds: %.1f ms per frame, frame begin %.2f ms, end %.2f ms, uploads %.1f per frame %.2f ms, bottom %.3f ms (max %.3f, drawn %lu of %lu); heap %u KB (peak %u KB, arena %u KB), linear %lu KB (peak %lu KB)\n",
                Ms(now - sLastTick) / n, Ms(sFrameBeginTicks) / n, Ms(sFrameEndTicks) / n, sUploads / n, Ms(sUploadTicks) / n, Ms(sBottomTicks) / n, Ms(sBottomMaxTicks), sBottomDrawn, sInterval, mi.uordblks / 1024,
                (u32)(sPeakHeap / 1024), mi.arena / 1024, linearUsed / 1024, sPeakLinear / 1024);
    }
    sLastTick = now;
    sFrameEndTicks = 0;
    sFrameBeginTicks = 0;
    sUploadTicks = 0;
    sUploads = 0;
    sBottomTicks = 0;
    sBottomMaxTicks = 0;
    sBottomDrawn = 0;
}
