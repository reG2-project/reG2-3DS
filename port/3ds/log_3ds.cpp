#include "log_3ds.hpp"
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

namespace
{
char sBuffer[32 * 1024];
const u32 kFlushAt = 8 * 1024;
u32 sUsed;
bool sAtExit;
u32 sLastBytes;
u64 sLastTicks;

void FlushAtExit()
{
    Log3DS_Flush();
}
} // namespace

void Log3DS_Printf(const char* format, ...)
{
    if (!sAtExit)
    {
        sAtExit = true;
        atexit(FlushAtExit);
    }
    for (int tries = 0; tries < 2; tries++)
    {
        va_list args;
        va_start(args, format);
        const int n = vsnprintf(sBuffer + sUsed, sizeof(sBuffer) - sUsed, format, args);
        va_end(args);
        if (n >= 0 && sUsed + n < sizeof(sBuffer))
        {
            sUsed += n;
            // Whole lines only: other writers to stderr go between two writes.
            if (sUsed >= kFlushAt && sBuffer[sUsed - 1] == '\n')
            {
                Log3DS_Flush();
            }
            return;
        }
        Log3DS_Flush();
    }
}

void Log3DS_Flush()
{
    if (!sUsed)
    {
        return;
    }
    const u64 t0 = svcGetSystemTick();
    // stderr is unbuffered: fwrite would split this into 1 KB writes, write() makes one.
    write(STDERR_FILENO, sBuffer, sUsed);
    sLastTicks = svcGetSystemTick() - t0;
    sLastBytes = sUsed;
    sUsed = 0;
}

void Log3DS_LastWrite(u32* pBytes, u64* pTicks)
{
    *pBytes = sLastBytes;
    *pTicks = sLastTicks;
}
