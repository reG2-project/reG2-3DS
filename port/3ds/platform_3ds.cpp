// 3DS start-up: the game folder on the SD card, the log and the environment, in place of port/setup.
#include "../setup/setup.hpp"
#include <3ds.h>
#include <SDL3/SDL_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef GTA2_OPENDSP
#include <opendsp.h>
#endif

// The main thread's stack (libctru's default is 32 KB): the game has large locals.
extern "C" u32 __stacksize__ = 2 * 1024 * 1024;
// Fixed sizes within the Old 3DS's 64 MB (with the 3 MB image and the stack), so the emulator holds the
// game to them in either mode. The game peaks near 22 MB of heap (frontend) and 7 MB of linear heap.
extern "C" u32 __ctru_heap_size = 40 * 1024 * 1024;
extern "C" u32 __ctru_linear_heap_size = 16 * 1024 * 1024;

static const char kGameDir[] = "sdmc:/3ds/gta2";

static void SDLCALL LogToStderr(void*, int, SDL_LogPriority, const char* message)
{
    fprintf(stderr, "%s\n", message);
}

// gta2_env.txt: KEY=VALUE lines, the environment the port's options and tests read.
static void LoadEnvironment()
{
    FILE* f = fopen("gta2_env.txt", "r");
    if (!f)
    {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = 0;
        if (char* pEq = strchr(line, '='))
        {
            *pEq = 0;
            setenv(line, pEq + 1, 1);
        }
    }
    fclose(f);
}

#ifdef GTA2_OPENDSP
// Loads OpenDSP's firmware, linked in, in place of the console's dspfirm.cdc. SDL's n3ds audio calls
// ndspInit and ndspExit itself, which then only count references.
static void StartDsp()
{
    const char* driver = getenv("GTA2_AUDIO_DRIVER");
    if (getenv("GTA2_NO_SOUND") || (driver && strcmp(driver, "n3ds") != 0))
    {
        return;
    }
    const Result rc = opendspInit();
    if (R_FAILED(rc))
    {
        // SDL then tries dspfirm.cdc.
        fprintf(stderr, "OpenDSP: error 0x%08lX\n", rc);
        ndspUseComponent(NULL, 0, 0, 0);
        return;
    }
    u32 caps = 0;
    opendspGetCapabilities(&caps);
    fprintf(stderr, "OpenDSP: capabilities 0x%lX\n", caps);
    atexit(opendspExit);
}
#endif

// Before main.cpp's EnterGameFolderAtStart (101) and the game's constructors.
__attribute__((constructor(100))) static void StartUp()
{
    osSetSpeedupEnable(true);
    chdir(kGameDir);
    // The log: stderr and SDL's, unbuffered so that it survives a crash.
    freopen("gta2_log.txt", "w", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);
    SDL_SetLogOutputFunction(LogToStderr, NULL);
    LoadEnvironment();
#ifdef GTA2_OPENDSP
    StartDsp();
#endif
}

bool port_PrepareGameFolder(bool)
{
    struct stat st;
    if (stat("data/fstyle.sty", &st) != 0)
    {
        fprintf(stderr, "gta2: no game data in %s/data\n", kGameDir);
        return false;
    }
    return true;
}
