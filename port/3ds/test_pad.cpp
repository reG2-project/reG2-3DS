// Scripted buttons for tests, as GTA2_INPUT is for keys: test/pad_input.txt in the game folder holds
// lines "<first frame> <last frame> <hex KEY_* mask>", frames as port_FrameCount counts them. The
// KEY_CPAD_* and KEY_CSTICK_* bits push the circle pad and the C-stick all the way.
// GTA2_TEST_SUSPEND=<frame>,<seconds>: HOME's suspend and resume at that frame, with the main thread held
// in between as aptMainLoop holds it, then the steps run in the second after the resume are logged.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

unsigned port_FrameCount();
void port_3dsAptHook(APT_HookType hook, void*);

extern "C" void __real_hidScanInput();
extern "C" u32 __real_hidKeysDown();
extern "C" u32 __real_hidKeysUp();
extern "C" void __real_hidCircleRead(circlePosition* pPos);
extern "C" void __real_irrstCstickRead(circlePosition* pPos);

namespace
{
struct Press
{
    unsigned first;
    unsigned last;
    unsigned long keys;
};

std::vector<Press> sPresses;
bool sLoaded;
u32 sHeld;
u32 sPrevHeld;
unsigned sSuspendFrame;
unsigned sSuspendSeconds;
u64 sResumeMs;
unsigned sResumeFrame;

void Load()
{
    sLoaded = true;
    if (const char* pSuspend = getenv("GTA2_TEST_SUSPEND"))
    {
        sscanf(pSuspend, "%u,%u", &sSuspendFrame, &sSuspendSeconds);
    }
    FILE* f = fopen("test/pad_input.txt", "r");
    if (!f)
    {
        return;
    }
    Press p;
    while (fscanf(f, "%u %u %lx", &p.first, &p.last, &p.keys) == 3)
    {
        sPresses.push_back(p);
    }
    fclose(f);
}
} // namespace

extern "C" void __wrap_hidScanInput()
{
    __real_hidScanInput();
    if (!sLoaded)
    {
        Load();
    }
    const unsigned frame = port_FrameCount();
    if (sSuspendFrame && frame >= sSuspendFrame)
    {
        sSuspendFrame = 0;
        fprintf(stderr, "test suspend: frame %u, %u s\n", frame, sSuspendSeconds);
        port_3dsAptHook(APTHOOK_ONSUSPEND, NULL);
        svcSleepThread(sSuspendSeconds * 1000000000LL);
        port_3dsAptHook(APTHOOK_ONRESTORE, NULL);
        sResumeMs = osGetTime();
        sResumeFrame = frame;
    }
    if (sResumeMs && osGetTime() - sResumeMs >= 1000)
    {
        fprintf(stderr, "test suspend: %u steps in the second after the resume\n", frame - sResumeFrame);
        sResumeMs = 0;
    }
    sPrevHeld = sHeld;
    sHeld = 0;
    for (const Press& p : sPresses)
    {
        if (frame >= p.first && frame <= p.last)
        {
            sHeld |= p.keys;
        }
    }
}

extern "C" u32 __wrap_hidKeysDown()
{
    return __real_hidKeysDown() | (sHeld & ~sPrevHeld);
}

extern "C" u32 __wrap_hidKeysUp()
{
    return __real_hidKeysUp() | (sPrevHeld & ~sHeld);
}

namespace
{
// libctru's full deflection.
const s16 kStickMax = 156;

void PushStick(circlePosition* pPos, u32 right, u32 left, u32 up, u32 down)
{
    const s16 dx = (sHeld & right ? kStickMax : 0) - (sHeld & left ? kStickMax : 0);
    const s16 dy = (sHeld & up ? kStickMax : 0) - (sHeld & down ? kStickMax : 0);
    if (pPos && (dx || dy))
    {
        pPos->dx = dx;
        pPos->dy = dy;
    }
}
} // namespace

extern "C" void __wrap_hidCircleRead(circlePosition* pPos)
{
    __real_hidCircleRead(pPos);
    PushStick(pPos, KEY_CPAD_RIGHT, KEY_CPAD_LEFT, KEY_CPAD_UP, KEY_CPAD_DOWN);
}

extern "C" void __wrap_irrstCstickRead(circlePosition* pPos)
{
    __real_irrstCstickRead(pPos);
    PushStick(pPos, KEY_CSTICK_RIGHT, KEY_CSTICK_LEFT, KEY_CSTICK_UP, KEY_CSTICK_DOWN);
}
