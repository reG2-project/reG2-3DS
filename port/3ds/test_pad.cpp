// Scripted buttons for tests, as GTA2_INPUT is for keys: test/pad_input.txt in the game folder holds
// lines "<first frame> <last frame> <hex KEY_* mask>", frames as port_FrameCount counts them.
#include <3ds.h>
#include <stdio.h>
#include <vector>

unsigned port_FrameCount();

extern "C" void __real_hidScanInput();
extern "C" u32 __real_hidKeysDown();
extern "C" u32 __real_hidKeysUp();

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

void Load()
{
    sLoaded = true;
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
