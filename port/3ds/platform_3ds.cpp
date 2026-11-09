// 3DS start-up: the game folder on the SD card, the log and the environment, in place of port/setup.
#include "../setup/setup.hpp"
#include "../src/platform.hpp"
#include <3ds.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_messagebox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

#ifdef GTA2_OPENDSP
#include <opendsp.h>
#endif

// The main thread's stack (libctru's default is 32 KB): the game has large locals.
extern "C" u32 __stacksize__ = 2 * 1024 * 1024;
// Fixed sizes within the Old 3DS's 64 MB (with the 3 MB image and the stack), so the emulator holds the
// game to them in either mode. The linear heap holds the sound bank (6.1 MB), which the DSP plays from.
extern "C" u32 __ctru_heap_size = 34 * 1024 * 1024;
extern "C" u32 __ctru_linear_heap_size = 22 * 1024 * 1024;

extern "C" u32 __ctru_heap;
extern "C" u32 __ctru_linear_heap;
extern "C" char* fake_heap_start;
extern "C" char* fake_heap_end;

// What the system lacked for the fixed heaps, 0 when they fit.
static u32 sHeapShortfall;
static u32 sHeapFree;

// libctru's own stops with svcBreak, before main, when the heaps do not fit (an applet's memory, a bigger
// image). Smaller heaps then, enough for StartUp's error screen.
extern "C" void __system_allocateHeaps()
{
    Handle limit;
    s64 max = 0;
    s64 used = 0;
    ResourceLimitType type = RESLIMIT_COMMIT;
    if (R_FAILED(svcGetResourceLimit(&limit, CUR_PROCESS_HANDLE)))
    {
        svcBreak(USERBREAK_PANIC);
    }
    svcGetResourceLimitLimitValues(&max, limit, &type, 1);
    svcGetResourceLimitCurrentValues(&used, limit, &type, 1);
    svcCloseHandle(limit);
    sHeapFree = static_cast<u32>(max - used) & ~0xFFFu;
    if (__ctru_heap_size + __ctru_linear_heap_size > sHeapFree)
    {
        const u32 kSmallLinear = 2 * 1024 * 1024;
        if (sHeapFree < kSmallLinear + __stacksize__ + 1024 * 1024)
        {
            svcBreak(USERBREAK_PANIC);
        }
        sHeapShortfall = __ctru_heap_size + __ctru_linear_heap_size - sHeapFree;
        __ctru_linear_heap_size = kSmallLinear;
        __ctru_heap_size = sHeapFree - kSmallLinear;
    }
    const MemPerm rw = static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE);
    if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0, __ctru_heap_size, MEMOP_ALLOC, rw)) ||
        R_FAILED(svcControlMemory(&__ctru_linear_heap, 0, 0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR, rw)))
    {
        svcBreak(USERBREAK_PANIC);
    }
    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    fake_heap_start = reinterpret_cast<char*>(__ctru_heap);
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}

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
// Loads OpenDSP's firmware, linked in, in place of the console's dspfirm.cdc. The later ndspInit and
// ndspExit calls (dsp_voices.cpp, or SDL's n3ds audio) then only count references.
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
        // ndspInit then tries dspfirm.cdc.
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

// The game's error boxes, as SDL has no message box on the 3DS: on the bottom screen until START.
// Linked with --wrap=SDL_ShowSimpleMessageBox.
extern "C" bool __wrap_SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags, const char* title, const char* message, SDL_Window*)
{
    const GSPGPU_FramebufferFormat format = gfxGetScreenFormat(GFX_BOTTOM);
    const devoptab_t* pStdout = devoptab_list[STD_OUT];
    const devoptab_t* pStderr = devoptab_list[STD_ERR];
    static PrintConsole sConsole;
    consoleInit(GFX_BOTTOM, &sConsole);
    printf("\x1b[31m%s\x1b[0m\n\n%s\n\nPress START.", title ? title : "GTA2", message ? message : "");
    while (aptMainLoop())
    {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
        {
            break;
        }
        gfxFlushBuffers();
        gspWaitForVBlank();
    }
    // Back to what SDL and citro3d set up, in case the game goes on.
    consoleClear();
    devoptab_list[STD_OUT] = pStdout;
    devoptab_list[STD_ERR] = pStderr;
    gfxSetScreenFormat(GFX_BOTTOM, format);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    return true;
}

void port_Suspend(bool suspended);

// HOME Menu, power button and sleep: called on the main thread within aptMainLoop, which returns
// only once the game is back; citro3d's and ndsp's hooks keep the GPU and DSP state.
void port_3dsAptHook(APT_HookType hook, void*)
{
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP)
    {
        port_Suspend(true);
    }
    else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP)
    {
        port_Suspend(false);
    }
}

// Before main.cpp's EnterGameFolderAtStart (101) and the game's constructors.
__attribute__((constructor(100))) static void StartUp()
{
    osSetSpeedupEnable(true);
    static aptHookCookie sAptHook;
    aptHook(&sAptHook, port_3dsAptHook, NULL);
    if (sHeapShortfall)
    {
        const u32 kMB = 1024 * 1024;
        char text[512];
        snprintf(text, sizeof(text),
                 "\x1b[31mGTA2: not enough memory\x1b[0m\n\n"
                 "Its heaps need %lu MB, %lu MB are free.\n\n"
                 "Start the Homebrew Launcher with a title\n"
                 "takeover (hold R while starting a game)\n"
                 "rather than in applet mode.\n"
                 "\nPress START to exit.",
                 (sHeapFree + sHeapShortfall + kMB - 1) / kMB, sHeapFree / kMB);
        ShowError(text);
        exit(0);
    }
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
        // Folders the game writes to and does not create, as the PC setup makes them.
        mkdir("player", 0777);
        mkdir("test", 0777);
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

namespace PortPlatform
{
bool Has(Feature feature)
{
    // Two fixed screens, the HUD on its own, the original's camera, the citro3d renderer's classic look and the
    // console's own buttons; the New 3DS's C-stick aims.
    static int sNew3ds = -1;
    if (sNew3ds < 0)
    {
        bool isNew = false;
        APT_CheckNew3DS(&isNew);
        sNew3ds = isNew;
    }
    return feature == kConsoleControls || (feature == kTwinStick && sNew3ds);
}

const wchar_t* PadButtonName(int code)
{
    // The console's labels at SDL's positions: B is the bottom button, A the right one. No stick presses.
    static const wchar_t* const kNames[12] = {L"B", L"A", L"Y", L"X", L"L", L"R", L"SELECT", L"START", L"-", L"-", L"ZL", L"ZR"};
    return code >= 0 && code < 12 ? kNames[code] : L"-";
}
} // namespace PortPlatform
