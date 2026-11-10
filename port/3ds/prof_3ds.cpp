#include "../src/prof.hpp"
#include "../src/logic_opts.hpp"
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
    "present.frameend", "tex.convert", "tex.upload", "wait.vsync", "wait.bottom", "wait.gpu", "gpu", "c3d.cpu", 0};
const char* const kCountNames[] = {
    "drive", "cars.visible", "peds.onscreen", "map.lookups", "collide.tests", "grid.cells", "steps", "prims",
    "drawcalls", "vertices", "states", "binds", "uploads", "lights", "litprims", "lighttests", "litverts", "nearlights", "tiles", "sprites", "ambient", "pumps", "events", "late", "refreshes"};
static_assert(sizeof(kPartNames) / sizeof(kPartNames[0]) == kPartCount, "a name per part");
static_assert(sizeof(kCountNames) / sizeof(kCountNames[0]) == kCountCount, "a name per count");

// GTA2_3DS_PROF_PMU=1: the two ARM11 event counters too, a pair of events per block of lines in turn.
const u32 kEventPairs[][2] = {
    {PERFCOUNTEREVT_CORE_DATA_CACHE_READ_MISS, PERFCOUNTEREVT_CORE_INST_CACHE_MISS},
    {PERFCOUNTEREVT_CORE_STALL_BY_DATA_HAZARD, PERFCOUNTEREVT_CORE_STALL_BY_LACK_OF_INST},
    {PERFCOUNTEREVT_CORE_DATA_CACHE_WRITE_MISS, PERFCOUNTEREVT_CORE_MAIN_TLB_MISS},
    {PERFCOUNTEREVT_CORE_INST_EXECUTED, PERFCOUNTEREVT_CORE_BRANCH_MISS_PREDICTED},
};
const u32 kEventPairCount = sizeof(kEventPairs) / sizeof(kEventPairs[0]);

struct Reading
{
    u64 ticks;
    u32 events[2];
};

struct Stat
{
    u64 cur;
    u64 sum;
    u64 max;
    s64 self;
    u64 events[2];
    s64 eventsSelf[2];
    u32 calls;
    bool active;
};

struct Entry
{
    Reading r0;
    u64 excluded0;
    u64 child;
    u32 eventsChild[2];
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
bool sPmu;
u32 sEventPair;
// GTA2_3DS_PROF_AB=1: the logic options (logic_opts.hpp) set as asked in one block of lines, all off in the next.
bool sAB;
u32 sBlocks;
signed char sOptions[PortLogic::kOptionCount];
// What a tick read, a scope and a section count of the two events.
u32 sTickEvents[2];
u32 sScopeEvents[2];
u32 sSectionEvents[2];

Reading Read()
{
    Reading r;
    if (sPmu)
    {
        u64 v = 0;
        svcControlPerformanceCounter(&v, PERFCOUNTEROP_GET_VALUE, PERFCOUNTERREG_CORE_COUNT_REG_0, 0);
        r.events[0] = (u32)v;
        svcControlPerformanceCounter(&v, PERFCOUNTEROP_GET_VALUE, PERFCOUNTERREG_CORE_COUNT_REG_1, 0);
        r.events[1] = (u32)v;
    }
    else
    {
        r.events[0] = r.events[1] = 0;
    }
    r.ticks = svcGetSystemTick();
    return r;
}

bool Excluded(u32 part)
{
    return part == kTexConvert || part == kTexUpload || part == kWaitVsync || part == kWaitBottom || part == kWaitGpu;
}

double Ms(double ticks)
{
    return ticks * 1000.0 / SYSCLOCK_ARM11;
}

void PopTop(const Reading& now)
{
    Entry& e = sStack[--sDepth];
    // The timers started inside, and its own: a tick read for a scope, a whole section's cost for a section.
    const u32 scopes = (sScopes - (e.timers0 & 0xFFFF)) & 0xFFFF;
    const u32 sections = (sSections - (e.timers0 >> 16)) & 0xFFFF;
    const u64 timers = (u64)scopes * sScopeTicks + (u64)sections * sSectionTicks +
                       (e.section ? sSectionTicks : sTickTicks);
    const u64 left = sExcluded - e.excluded0 + timers;
    const u64 raw = now.ticks - e.r0.ticks > left ? now.ticks - e.r0.ticks - left : 0;
    const u64 est = raw * e.scale;
    Stat& s = sParts[e.part];
    s.cur += est;
    s.self += (s64)est - (s64)e.child;
    s.calls += e.scale;
    s.active = false;
    u32 events[2] = {0, 0};
    for (int i = 0; i < 2 && sPmu; i++)
    {
        const u32 own = scopes * sScopeEvents[i] + sections * sSectionEvents[i] +
                        (e.section ? sSectionEvents[i] : sTickEvents[i]);
        const u32 n = now.events[i] - e.r0.events[i];
        events[i] = (n > own ? n - own : 0) * e.scale;
        s.events[i] += events[i];
        s.eventsSelf[i] += (s64)events[i] - (s64)e.eventsChild[i];
    }
    if (Excluded(e.part))
    {
        sExcluded += raw;
    }
    else if (sDepth)
    {
        Entry& parent = sStack[sDepth - 1];
        parent.child += est;
        parent.eventsChild[0] += events[0];
        parent.eventsChild[1] += events[1];
    }
}

int PushAt(Part part, unsigned scale, bool section, const Reading* pNow)
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
    e.eventsChild[0] = e.eventsChild[1] = 0;
    e.excluded0 = sExcluded;
    s.active = true;
    sScopes += section ? 0 : 1;
    sSections += section ? 1 : 0;
    e.timers0 = (sScopes & 0xFFFF) | (sSections << 16);
    e.r0 = pNow ? *pNow : Read();
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

// What the tick reads, scopes and sections cost, in ticks and events. Leaves the stack as it found it.
void Calibrate()
{
    const int n = 1000;
    const u32 scopes = sScopes;
    const u32 sections = sSections;
    const Entry parent = sDepth ? sStack[sDepth - 1] : Entry();
    sTickTicks = sScopeTicks = sSectionTicks = 0;
    memset(sTickEvents, 0, sizeof(sTickEvents));
    memset(sScopeEvents, 0, sizeof(sScopeEvents));
    memset(sSectionEvents, 0, sizeof(sSectionEvents));
    Reading r0 = Read();
    for (int i = 0; i < n; i++)
    {
        Read();
    }
    Reading r1 = Read();
    u32 tickTicks = (u32)((r1.ticks - r0.ticks) / n);
    u32 tickEvents[2] = {(r1.events[0] - r0.events[0]) / n, (r1.events[1] - r0.events[1]) / n};
    for (int i = 0; i < n; i++)
    {
        Pop(Push(kCalibrate, 1));
    }
    r0 = Read();
    sScopeTicks = (u32)((r0.ticks - r1.ticks) / n);
    u32 scopeEvents[2] = {(r0.events[0] - r1.events[0]) / n, (r0.events[1] - r1.events[1]) / n};
    for (int i = 0; i < n; i++)
    {
        Section(kCalibrate);
    }
    EndSection();
    r1 = Read();
    sSectionTicks = (u32)((r1.ticks - r0.ticks) / n);
    for (int i = 0; i < 2; i++)
    {
        sSectionEvents[i] = (r1.events[i] - r0.events[i]) / n;
        sScopeEvents[i] = scopeEvents[i];
        sTickEvents[i] = tickEvents[i];
    }
    sTickTicks = tickTicks;
    memset(&sParts[kCalibrate], 0, sizeof(sParts[kCalibrate]));
    sScopes = scopes;
    sSections = sections;
    if (sDepth)
    {
        sStack[sDepth - 1] = parent;
    }
}

// The next pair of events on the two counters, and what the timers count of them.
void NextEvents()
{
    const u32* pPair = kEventPairs[sEventPair];
    // libctru's wrapper always stores the result: a NULL output faults on the console (Azahar lets it pass).
    u64 unused;
    svcControlPerformanceCounter(&unused, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_0, pPair[0]);
    svcControlPerformanceCounter(&unused, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_1, pPair[1]);
    Calibrate();
    const Reading now = Read();
    for (int i = 0; i < sDepth; i++)
    {
        sStack[i].r0.events[0] = now.events[0];
        sStack[i].r0.events[1] = now.events[1];
        sStack[i].eventsChild[0] = sStack[i].eventsChild[1] = 0;
    }
}

void StartPmu()
{
    u64 out;
    const Result rc = svcControlPerformanceCounter(&out, PERFCOUNTEROP_ENABLE, 0, 0);
    sPmu = R_SUCCEEDED(rc);
    Log3DS_Printf("prof: event counters %s (%08lx)\n", sPmu ? "on" : "off", (u32)rc);
    if (sPmu)
    {
        NextEvents();
    }
}

void LogCosts()
{
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

void PrintEvents(const char* label, u32 first, u32 last, u32 units)
{
    Add("prof pmu %s %lu,%lu:", label, kEventPairs[sEventPair][0], kEventPairs[sEventPair][1]);
    for (u32 i = first; i < last; i++)
    {
        const Stat& s = sParts[i];
        if (s.calls && kPartNames[i])
        {
            Add(" %s=%.1f/%.1f/%.1f/%.1f", kPartNames[i], (double)s.events[0] / units, (double)s.events[1] / units,
                (double)s.eventsSelf[0] / units, (double)s.eventsSelf[1] / units);
        }
    }
    EndLine();
}

void Print()
{
    // Each value: average per step (or frame), maximum in one, average without the parts under it, calls.
    const u32 steps = sSteps ? sSteps : 1;
    // The times leave the timers' own cost out; this is what they added to the frame.
    u32 logBytes;
    u64 logTicks;
    Log3DS_LastWrite(&logBytes, &logTicks);
    u32 options = 0;
    for (u32 i = 0; i < PortLogic::kOptionCount; i++)
    {
        options |= PortLogic::On(static_cast<PortLogic::Option>(i)) ? 1u << i : 0;
    }
    Log3DS_Printf("prof: %lu frames, %lu steps, %.0f scopes and %.0f sections per frame (%.2f ms); last log write %lu bytes in %.2f ms; options %lu\n",
                  sFrames, sSteps, (double)sScopes / sFrames, (double)sSections / sFrames,
                  Ms((u64)sScopes * sScopeTicks + (u64)sSections * sSectionTicks) / sFrames, logBytes, Ms(logTicks), options);
    PrintParts("step", 0, kStepParts, steps);
    PrintParts("frame", kStepParts, kPartCount, sFrames);
    if (sPmu)
    {
        // Per step or frame: the two events, then without the parts under it.
        PrintEvents("step", 0, kStepParts, steps);
        PrintEvents("frame", kStepParts, kPartCount, sFrames);
    }
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
    const Reading now = Read();
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
    const Reading now = Read();
    if (sDepth && sStack[sDepth - 1].section)
    {
        PopTop(now);
    }
    PushAt(part, gPrimScale, true, &now);
}

void EndSection()
{
    if (gOn && gPrimScale != kSkip && sDepth && sStack[sDepth - 1].section)
    {
        PopTop(Read());
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
        const char* pAB = getenv("GTA2_3DS_PROF_AB");
        sAB = gOn && pAB && atoi(pAB) > 0;
        for (u32 i = 0; i < PortLogic::kOptionCount && sAB; i++)
        {
            sOptions[i] = PortLogic::On(static_cast<PortLogic::Option>(i)) ? 1 : 0;
        }
        const char* pPmu = getenv("GTA2_3DS_PROF_PMU");
        if (gOn && pPmu && atoi(pPmu) > 0)
        {
            StartPmu();
        }
        if (gOn)
        {
            Calibrate();
            LogCosts();
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
        {
            Scope log(kLog);
            Print();
        }
        sBlocks++;
        if (sAB)
        {
            for (u32 i = 0; i < PortLogic::kOptionCount; i++)
            {
                PortLogic::gOn[i] = sBlocks % 2 ? 0 : sOptions[i];
            }
        }
        // A section open here would be taken by the calibration's. With sAB, each pair sees both settings.
        if (sPmu && (!sAB || sBlocks % 2 == 0) && !(sDepth && sStack[sDepth - 1].section))
        {
            sEventPair = (sEventPair + 1) % kEventPairCount;
            NextEvents();
        }
    }
}
} // namespace PortProf
