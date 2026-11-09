// Log lines kept in memory and written to gta2_log.txt in one request now and then: on the console, each
// write to the SD card takes milliseconds, and an unbuffered fprintf can make several.
#pragma once

#include <3ds/types.h>

// Kept; all that is kept is written at the end of a line once it reaches 8 KB, and at exit.
void Log3DS_Printf(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Log3DS_Flush();
// The last write: its bytes and ticks.
void Log3DS_LastWrite(u32* pBytes, u64* pTicks);
