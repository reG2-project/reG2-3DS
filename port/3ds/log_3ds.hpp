// Log lines kept in memory and written to gta2_log.txt by a thread of their own, 8 KB at a time: on the console,
// each write to the SD card takes milliseconds, up to a fifth of a second.
#pragma once

#include <3ds/types.h>

// One whole line per call, from any thread; written once 8 KB are kept, and at exit.
void Log3DS_Printf(const char* format, ...) __attribute__((format(printf, 1, 2)));
// The last write: its bytes and ticks.
void Log3DS_LastWrite(u32* pBytes, u64* pTicks);
