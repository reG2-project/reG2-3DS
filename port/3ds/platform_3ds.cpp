// 3DS start-up: the game folder on the SD card and the log, in place of port/setup.
#include "../setup/setup.hpp"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

// The main thread's stack (libctru's default is 32 KB): the game has large locals.
extern "C" u32 __stacksize__ = 2 * 1024 * 1024;

static const char kGameDir[] = "sdmc:/3ds/gta2";

// Before main.cpp's EnterGameFolderAtStart (101), which opens GTA2_LOG_FILE.
__attribute__((constructor(100))) static void StartUp()
{
    osSetSpeedupEnable(true);
    chdir(kGameDir);
    setenv("GTA2_LOG_FILE", "sdmc:/3ds/gta2/gta2_log.txt", 1);
    // stderr also goes to the emulator's log (svcOutputDebugString).
    consoleDebugInit(debugDevice_SVC);
    setvbuf(stderr, NULL, _IONBF, 0);
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
