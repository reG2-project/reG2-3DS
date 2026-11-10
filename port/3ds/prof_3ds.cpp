#include "../src/prof.hpp"
#include "log_3ds.hpp"
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace PortProf
{
bool gOn;
unsigned gPrimScale;
unsigned gCounts[kCountCount];

namespace
{
// prof_summary.py reads these names.
const char* const kPartNames[] = {
    "step", "controls", "players", "lights", "cranes", "crushers", "generators", "peds", "peds.ped", "peds.ped.ai",
    "peds.ped.walk", "peds.ped.still", "peds.ped.car", "peds.ped.down", "peds.ped.idle", "peds.ped.move", "peds.ped.map",
    "peds.dummies", "transport", "garages", "cars", "cars.car", "cars.car.ai", "cars.car.physics",
    "cars.car.physics.collide", "cars.car.physics.forces", "cars.car.physics.move", "cars.car.physics.scare",
    "cars.car.physics.skid", "cars.car.parked", "cars.car.driven", "cars.car.moving", "cars.car.other", "cars.car.grid",
    "cars.car.despawn", "cars.car.doors", "cars.car.misc", "cars.traffic",
    "trafficlights", "objects", "scripts", "crews", "ambulance", "police", "particles", "tileanim", "hud", "doors",
    "firetrucks", "scores", "sound", "sound.entities", "sound.queues", "paused", "collide",
    "frame", "loop.next", "loop.beforestep", "loop.afterstep", "loop.begindraw", "loop.enddraw", "loop.fps", "log",
    "inputs", "pump", "pump.pad", "idle", "draw", "draw.sort", "draw.setup", "draw.map", "draw.scores",
    "draw.hud", "draw.end", "prim", "prim.uv", "prim.turn", "prim.clip", "prim.resolve",
    "prim.setup", "prim.light", "prim.verts", "prim.c3d", "flush", "present", "present.bottom", "present.overlay",
    "present.frameend", "tex.convert", "tex.upload", "wait.vsync", "wait.gpu", "gpu", "c3d.cpu", 0};
const char* const kCountNames[] = {
    "drive", "cars.visible", "peds.onscreen", "map.lookups", "collide.tests", "grid.cells", "steps", "prims",
    "drawcalls", "vertices", "states", "binds", "uploads", "lights", "litprims", "lighttests", "litverts", "nearlights", "tiles", "sprites", "ambient", "pumps", "events", "late"};
static_assert(sizeof(kPartNames) / sizeof(kPartNames[0]) == kPartCount, "a name per part");
static_assert(sizeof(kCountNames) / sizeof(kCountNames[0]) == kCountCount, "a name per count");

struct Stat
{
    u64 cur;
    u64 sum;
    u64 max;
    s64 self;
    u32 calls;
    bool active;
};

struct Entry
{
    u64 t0;
    u64 excluded0;
    u64 child;
    u32 timers0;
    u16 part;
    u16 scale;
    bool section;
};

const int kMaxDepth = 32;
Entry sStack[kMaxDepth];
int sDepth;
Stat sParts[kPartCount];
u32 sCountSum[kCountCount];
u32 sCountMax[kCountCount];
// Ticks of the excluded scopes so far: an entry leaves out what they took while it was open.
u64 sExcluded;
bool sInit;
u32 sInterval;
u32 sSample = 4;
u32 sRng = 1;
u32 sSteps;
u32 sFrames;
// Scopes and sections started, and what one costs in ticks: taken out of the entries they run in.
u32 sScopes;
u32 sSections;
u32 sTickTicks;
u32 sScopeTicks;
u32 sSectionTicks;
u64 sLastFrame;

bool Excluded(u32 part)
{
    return part == kTexConvert || part == kTexUpload || part == kWaitVsync || part == kWaitGpu;
}

double Ms(double ticks)
{
    return ticks * 1000.0 / SYSCLOCK_ARM11;
}

void PopTop(u64 now)
{
    Entry& e = sStack[--sDepth];
    // The timers started inside, and its own: a tick read for a scope, a whole section's cost for a section.
    const u64 timers = (u64)((sScopes - (e.timers0 & 0xFFFF)) & 0xFFFF) * sScopeTicks +
                       (u64)((sSections - (e.timers0 >> 16)) & 0xFFFF) * sSectionTicks +
                       (e.section ? sSectionTicks : sTickTicks);
    const u64 left = sExcluded - e.excluded0 + timers;
    const u64 raw = now - e.t0 > left ? now - e.t0 - left : 0;
    const u64 est = raw * e.scale;
    Stat& s = sParts[e.part];
    s.cur += est;
    s.self += (s64)est - (s64)e.child;
    s.calls += e.scale;
    s.active = false;
    if (Excluded(e.part))
    {
        sExcluded += raw;
    }
    else if (sDepth)
    {
        sStack[sDepth - 1].child += est;
    }
}

int PushAt(Part part, unsigned scale, bool section, u64 now)
{
    Stat& s = sParts[part];
    if (scale == kSkip || s.active || sDepth == kMaxDepth)
    {
        return -1;
    }
    Entry& e = sStack[sDepth];
    e.part = (u16)part;
    e.scale = (u16)(scale ? scale : 1);
    e.section = section;
    e.child = 0;
    e.excluded0 = sExcluded;
    s.active = true;
    sScopes += section ? 0 : 1;
    sSections += section ? 1 : 0;
    e.timers0 = (sScopes & 0xFFFF) | (sSections << 16);
    e.t0 = now ? now : svcGetSystemTick();
    return sDepth++;
}

void Fold(u32 first, u32 last, u32 countFirst, u32 countLast)
{
    for (u32 i = first; i < last; i++)
    {
        Stat& s = sParts[i];
        s.sum += s.cur;
        s.max = s.cur > s.max ? s.cur : s.max;
        s.cur = 0;
    }
    for (u32 i = countFirst; i < countLast; i++)
    {
        sCountSum[i] += gCounts[i];
        sCountMax[i] = gCounts[i] > sCountMax[i] ? gCounts[i] : sCountMax[i];
        gCounts[i] = 0;
    }
}

void Calibrate()
{
    const int n = 1000;
    u64 t0 = svcGetSystemTick();
    for (int i = 0; i < n; i++)
    {
        svcGetSystemTick();
    }
    sTickTicks = (u32)((svcGetSystemTick() - t0) / n);
    t0 = svcGetSystemTick();
    for (int i = 0; i < n; i++)
    {
        Pop(Push(kCalibrate, 1));
    }
    sScopeTicks = (u32)((svcGetSystemTick() - t0) / n);
    t0 = svcGetSystemTick();
    for (int i = 0; i < n; i++)
    {
        Section(kCalibrate);
    }
    EndSection();
    sSectionTicks = (u32)((svcGetSystemTick() - t0) / n);
    memset(&sParts[kCalibrate], 0, sizeof(sParts[kCalibrate]));
    sScopes = 0;
    sSections = 0;
    Log3DS_Printf("prof: every %lu frames, 1 primitive in %lu timed; a tick read takes %.3f us, a scope %.3f us, a section %.3f us\n",
            sInterval, sSample, Ms(sTickTicks) * 1000.0, Ms(sScopeTicks) * 1000.0, Ms(sSectionTicks) * 1000.0);
}

// A line made in pieces goes to the log whole: other threads log too.
char sLine[4096];
u32 sLineUsed;

__attribute__((format(printf, 1, 2))) void Add(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(sLine + sLineUsed, sizeof(sLine) - sLineUsed, format, args);
    va_end(args);
    if (n > 0)
    {
        sLineUsed = sLineUsed + n < sizeof(sLine) ? sLineUsed + n : sizeof(sLine) - 1;
    }
}

void EndLine()
{
    Log3DS_Printf("%s\n", sLine);
    sLineUsed = 0;
}

void PrintParts(const char* label, u32 first, u32 last, u32 units)
{
    Add("prof %s:", label);
    for (u32 i = first; i < last; i++)
    {
        const Stat& s = sParts[i];
        if (s.calls && kPartNames[i])
        {
            Add(" %s=%.3f/%.3f/%.3f/%.1f", kPartNames[i], Ms(s.sum) / units, Ms(s.max), Ms(s.self) / units,
                (double)s.calls / units);
        }
    }
    EndLine();
}

void PrintCounts(u32 first, u32 last, u32 units, bool newLine)
{
    for (u32 i = first; i < last; i++)
    {
        Add(" %s=%.2f/%lu", kCountNames[i], (double)sCountSum[i] / units, sCountMax[i]);
    }
    if (newLine)
    {
        EndLine();
    }
}

void Print()
{
    // Each value: average per step (or frame), maximum in one, average without the parts under it, calls.
    const u32 steps = sSteps ? sSteps : 1;
    // The times leave the timers' own cost out; this is what they added to the frame.
    u32 logBytes;
    u64 logTicks;
    Log3DS_LastWrite(&logBytes, &logTicks);
    Log3DS_Printf("prof: %lu frames, %lu steps, %.0f scopes and %.0f sections per frame (%.2f ms); last log write %lu bytes in %.2f ms\n",
                  sFrames, sSteps, (double)sScopes / sFrames, (double)sSections / sFrames,
                  Ms((u64)sScopes * sScopeTicks + (u64)sSections * sSectionTicks) / sFrames, logBytes, Ms(logTicks));
    PrintParts("step", 0, kStepParts, steps);
    PrintParts("frame", kStepParts, kPartCount, sFrames);
    Add("prof count:");
    PrintCounts(0, kStepCounts, steps, false);
    PrintCounts(kStepCounts, kCountCount, sFrames, true);
    memset(sParts, 0, sizeof(sParts));
    memset(sCountSum, 0, sizeof(sCountSum));
    memset(sCountMax, 0, sizeof(sCountMax));
    for (int i = 0; i < sDepth; i++)
    {
        sParts[sStack[i].part].active = true;
    }
    sSteps = 0;
    sFrames = 0;
    sScopes = 0;
    sSections = 0;
}
} // namespace

int Push(Part part, unsigned scale)
{
    return PushAt(part, scale, false, 0);
}

void Pop(int index)
{
    const u64 now = svcGetSystemTick();
    while (sDepth > index)
    {
        PopTop(now);
    }
}

void Section(Part part)
{
    if (!gOn || gPrimScale == kSkip)
    {
        return;
    }
    const u64 now = svcGetSystemTick();
    if (sDepth && sStack[sDepth - 1].section)
    {
        PopTop(now);
    }
    PushAt(part, gPrimScale, true, now);
}

void EndSection()
{
    if (gOn && gPrimScale != kSkip && sDepth && sStack[sDepth - 1].section)
    {
        PopTop(svcGetSystemTick());
    }
}

unsigned SamplePrimitive()
{
    sRng = sRng * 1664525u + 1013904223u;
    return (sRng >> 8) % sSample ? kSkip : sSample;
}

void AddMs(Part part, float ms)
{
    Stat& s = sParts[part];
    s.cur += (u64)(ms * (SYSCLOCK_ARM11 / 1000.0));
    s.self += (s64)(ms * (SYSCLOCK_ARM11 / 1000.0));
    s.calls++;
}

void Reset()
{
    if (!sInit)
    {
        sInit = true;
        const char* pInterval = getenv("GTA2_3DS_PROF");
        sInterval = pInterval ? (u32)atoi(pInterval) : 0;
        const char* pSample = getenv("GTA2_3DS_PROF_SAMPLE");
        sSample = pSample && atoi(pSample) > 0 ? (u32)atoi(pSample) : sSample;
        gOn = sInterval > 0;
        if (gOn)
        {
            Calibrate();
        }
    }
    for (u32 i = 0; i < kPartCount; i++)
    {
        sParts[i].cur = 0;
    }
    memset(gCounts, 0, sizeof(gCounts));
    sLastFrame = 0;
}

void EndStep()
{
    Fold(0, kStepParts, 0, kStepCounts);
    sSteps++;
    gCounts[kSteps]++;
}

void EndFrame()
{
    const u64 now = svcGetSystemTick();
    if (sLastFrame)
    {
        // Late: three refreshes of the screen (50 ms) or more instead of two.
        gCounts[kLateFrames] += now - sLastFrame > SYSCLOCK_ARM11 / 1000 * 42 ? 1 : 0;
        sParts[kFrame].cur += now - sLastFrame;
        sParts[kFrame].self += (s64)(now - sLastFrame);
        sParts[kFrame].calls++;
    }
    sLastFrame = now;
    Fold(kStepParts, kPartCount, kStepCounts, kCountCount);
    if (++sFrames >= sInterval)
    {
        Scope log(kLog);
        Print();
    }
}
} // namespace PortProf
