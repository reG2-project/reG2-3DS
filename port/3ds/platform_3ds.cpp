// 3DS start-up: the game folder on the SD card, the log and the environment, in place of port/setup.
#include "../setup/setup.hpp"
#include <3ds.h>
#include <SDL3/SDL_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// The main thread's stack (libctru's default is 32 KB): the game has large locals.
extern "C" u32 __stacksize__ = 2 * 1024 * 1024;

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
