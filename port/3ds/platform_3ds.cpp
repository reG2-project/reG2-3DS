// 3DS start-up: the game folder on the SD card, the log and the environment, in place of port/setup.
#include "../setup/setup.hpp"
#include <3ds.h>
#include <SDL3/SDL_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

#ifdef GTA2_OPENDSP
#include <opendsp.h>
#endif

// The main thread's stack (libctru's default is 32 KB): the game has large locals.
extern "C" u32 __stacksize__ = 2 * 1024 * 1024;
// Fixed sizes within the Old 3DS's 64 MB (with the 3 MB image and the stack), so the emulator holds the
// game to them in either mode. The game peaks near 22 MB of heap (frontend) and 7 MB of linear heap.
extern "C" u32 __ctru_heap_size = 40 * 1024 * 1024;
extern "C" u32 __ctru_linear_heap_size = 16 * 1024 * 1024;

extern "C" int __system_argc;
extern "C" char** __system_argv;

// The game folder: the .3dsx's own folder when it holds the data, else this one.
static const char kFallbackDir[] = "sdmc:/3ds/gta2";
static char sTried[2][256];
static int sTriedCount;
static bool sHasData;

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

static bool HasData(const char* dir)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/data/fstyle.sty", dir);
    struct stat st;
    return stat(path, &st) == 0;
}

static bool TryFolder(const char* dir)
{
    for (int i = 0; i < sTriedCount; i++)
    {
        if (strcmp(sTried[i], dir) == 0)
        {
            return false;
        }
    }
    snprintf(sTried[sTriedCount], sizeof(sTried[0]), "%s", dir);
    sTriedCount++;
    return HasData(dir) && chdir(dir) == 0;
}

// The .3dsx's folder from the path the launcher passes (hbmenu: "sdmc:/3ds/gta2/gta2.3dsx"); none from
// 3dslink, a CIA or an emulator that gives no arguments.
static bool TryLaunchFolder()
{
    if (__system_argc < 1 || !__system_argv[0] || strncmp(__system_argv[0], "sdmc:/", 6) != 0)
    {
        return false;
    }
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", __system_argv[0]);
    char* pSlash = strrchr(dir, '/');
    if (!pSlash || pSlash - dir < 6)
    {
        return false;
    }
    *pSlash = 0;
    return TryFolder(dir);
}

// A message on the top screen until START, before SDL owns the screens.
static void ShowError(const char* text)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    fputs(text, stdout);
    while (aptMainLoop())
    {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
        {
            break;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
}

// Before main.cpp's EnterGameFolderAtStart (101) and the game's constructors.
__attribute__((constructor(100))) static void StartUp()
{
    osSetSpeedupEnable(true);
    sHasData = TryLaunchFolder() || TryFolder(kFallbackDir);
    if (!sHasData)
    {
        return;
    }
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
    if (sHasData)
    {
        char cwd[256];
        fprintf(stderr, "gta2: started from %s, game folder %s\n", __system_argc > 0 && __system_argv[0] ? __system_argv[0] : "(no path)",
                getcwd(cwd, sizeof(cwd)) ? cwd : "?");
        return true;
    }
    std::string text = "\x1b[31mGTA2: no game data found\x1b[0m\n\nLooked for data/fstyle.sty in:\n";
    for (int i = 0; i < sTriedCount; i++)
    {
        text += "  ";
        text += sTried[i];
        text += "/\n";
    }
    text += "\nCopy the PC version's data folder (fstyle.sty,\n"
            "e.gxt, the cities' .gmp, .sty and .scr files)\n"
            "to the folder of gta2.3dsx or to sdmc:/3ds/gta2,\n"
            "as data/.\n"
            "\nPress START to exit.";
    ShowError(text.c_str());
    return false;
}
