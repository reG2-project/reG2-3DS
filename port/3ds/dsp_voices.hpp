// The DSP's channels (ndsp), on which miles.cpp plays its samples and streams on the 3DS in place of its CPU
// mixer. Called under miles.cpp's lock. A voice is a channel, -1 for none.
#pragma once

#include "types.hpp"
#include <stddef.h>

struct DspSound
{
    const void* mData; // in linear memory, unless unsigned 8-bit
    bool mStereo;
    bool m16;
    bool mSigned8;
    u32 mLoopStart; // in frames, the end being the sound's end
    u32 mLoopEnd;
    s32 mLoopCount; // 0 for ever
};

// Starts the DSP and a thread on the system core that calls pFeed every few milliseconds; false without a DSP.
bool DspVoices_Open(void (*pFeed)());
// -1 when no channel is free or the data is not in linear memory.
s32 DspVoices_Play(const DspSound& sound, float rate, float left, float right);
// A voice for a stream, silent until set: stereo 16-bit frames handed over in buffers.
s32 DspVoices_OpenStream();
void DspVoices_Set(s32 voice, float rate, float left, float right);
// Whether it has frames left to play.
bool DspVoices_Playing(s32 voice);
// Frees the voice's channel, at once or after a fade-out.
void DspVoices_Stop(s32 voice, bool now);
// Frees the channels of voices faded out.
void DspVoices_Update();
// Stops at once what plays from that memory, before it is written over.
void DspVoices_StopIn(const void* p, size_t len);

// A stream voice's next free buffer and its size in frames, NULL when all are queued.
s16* DspVoices_Buffer(s32 voice, u32* pFrames);
void DspVoices_Submit(s32 voice, u32 frames);
// Drops the buffers queued.
void DspVoices_Flush(s32 voice);

void DspVoices_Pause(bool paused);
void DspVoices_SetMasterVolume(float volume);

// Zeroed memory the DSP can play from (linear).
void* DspVoices_Alloc(size_t size);
void DspVoices_Free(void* p);
// After the CPU wrote sounds there, as the DSP reads the memory and not the CPU's cache.
void DspVoices_Written(const void* p, size_t len);
