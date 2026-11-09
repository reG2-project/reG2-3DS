// GTA2_3DS_STATS=<frames>: memory and renderer times in the log every <frames> frames.
#pragma once

#include <3ds/types.h>

// Ticks spent converting and uploading textures.
void Stats3DS_AddUpload(u64 ticks);
// Ticks C3D_FrameBegin waited for the previous frame and the screen.
void Stats3DS_AddFrameBegin(u64 ticks);
// Ticks the CPU spent on the bottom screen in a frame, and whether it was drawn (else it keeps its last frame).
void Stats3DS_AddBottom(u64 ticks, bool drawn);
// The textures with storage and their bytes, those of the evictable ones, and the evictions and allocation
// failures so far.
void Stats3DS_SetTextures(u32 count, u32 bytes, u32 cachedBytes, u32 evictions, u32 failures);
// The upload worker's ticks and jobs so far, and the jobs the main thread wrote as it needed them first.
void Stats3DS_SetUploadWorker(u64 ticks, u32 jobs, u32 mainJobs);
// Once per frame, with the ticks C3D_FrameEnd took (the wait for the GPU and the screen).
void Stats3DS_Frame(u64 frameEndTicks);
// Whether the main thread's sleeps count in Stats3DS_WaitTicks (two tick reads each).
void Stats3DS_CountSleeps(bool on);
// Ticks the main thread has waited so far: C3D_FrameBegin, C3D_FrameEnd and, when counted, its sleeps.
u64 Stats3DS_WaitTicks();
