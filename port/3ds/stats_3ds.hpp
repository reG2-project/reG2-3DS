// GTA2_3DS_STATS=<frames>: memory and renderer times in the log every <frames> frames.
#pragma once

#include <3ds/types.h>

// Ticks spent converting and uploading textures.
void Stats3DS_AddUpload(u64 ticks);
// Ticks C3D_FrameBegin waited for the previous frame and the screen.
void Stats3DS_AddFrameBegin(u64 ticks);
// Ticks the CPU spent on the bottom screen in a frame, and whether it was drawn (else it keeps its last frame).
void Stats3DS_AddBottom(u64 ticks, bool drawn);
// Once per frame, with the ticks C3D_FrameEnd took (the wait for the GPU and the screen).
void Stats3DS_Frame(u64 frameEndTicks);
