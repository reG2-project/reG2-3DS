#include "log_3ds.hpp"
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

namespace
{
const u32 kSize = 32 * 1024;
const u32 kWriteAt = 8 * 1024;
char sBuffers[2][kSize];
char* spFill = sBuffers[0];
u32 sUsed;
u32 sDropped;
// The buffer the writer thread has, until it is written.
const char* volatile spWrite;
u32 sWriteUsed;
volatile u32 sLastBytes;
volatile u64 sLastTicks;
volatile bool sQuit;
bool sStarted;
Thread sThread;
LightLock sLock;
LightEvent sWake;

void Write(const char* p, u32 n)
{
    const u64 t0 = svcGetSystemTick();
    // One write() where fwrite would make 1 KB ones; the lock keeps other threads' fprintf(stderr) out of it.
    flockfile(stderr);
    write(fileno(stderr), p, n);
    funlockfile(stderr);
    sLastTicks = svcGetSystemTick() - t0;
    sLastBytes = n;
}

void WriterMain(void*)
{
    for (;;)
    {
        if (spWrite)
        {
            Write(spWrite, sWriteUsed);
            __dmb();
            spWrite = NULL;
        }
        else if (sQuit)
        {
            return;
        }
        else
        {
            LightEvent_Wait(&sWake);
        }
    }
}

// Under sLock, while the writer has no buffer: hands it what is kept, or writes it here without a writer.
void Hand()
{
    if (sThread && !sQuit)
    {
        sWriteUsed = sUsed;
        spWrite = spFill;
        spFill = spFill == sBuffers[0] ? sBuffers[1] : sBuffers[0];
        LightEvent_Signal(&sWake);
    }
    else
    {
        Write(spFill, sUsed);
    }
    sUsed = sDropped ? snprintf(spFill, kSize, "log: %lu lines dropped, the buffer was full\n", sDropped) : 0;
    sDropped = 0;
}

void Finish()
{
    LightLock_Lock(&sLock);
    sQuit = true;
    LightLock_Unlock(&sLock);
    if (sThread)
    {
        LightEvent_Signal(&sWake);
        threadJoin(sThread, U64_MAX);
        threadFree(sThread);
    }
    LightLock_Lock(&sLock);
    while (sUsed)
    {
        Hand();
    }
    LightLock_Unlock(&sLock);
}

// In a constructor: once the game runs, newlib's 32 atexit slots are full and atexit fails.
__attribute__((constructor(101))) void Init()
{
    LightLock_Init(&sLock);
    LightEvent_Init(&sWake, RESET_ONESHOT);
    atexit(Finish);
}

// The writer runs on the system core, below the sound's feed and the texture worker.
void Start()
{
    sStarted = true;
    sThread = threadCreate(WriterMain, NULL, 16 * 1024, 0x3F, 1, false);
    if (!sThread)
    {
        sThread = threadCreate(WriterMain, NULL, 16 * 1024, 0x3F, -2, false);
    }
}
} // namespace

void Log3DS_Printf(const char* format, ...)
{
    LightLock_Lock(&sLock);
    if (!sStarted)
    {
        Start();
    }
    const u32 room = kSize - sUsed;
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(spFill + sUsed, room, format, args);
    va_end(args);
    if (n >= 0 && (u32)n < room)
    {
        sUsed += n;
    }
    else
    {
        sDropped++;
    }
    if (sUsed && !spWrite && (sQuit || sUsed >= kWriteAt))
    {
        Hand();
    }
    LightLock_Unlock(&sLock);
}

void Log3DS_LastWrite(u32* pBytes, u64* pTicks)
{
    *pBytes = sLastBytes;
    *pTicks = sLastTicks;
}
