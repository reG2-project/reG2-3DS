#include "dsp_voices.hpp"
#include "log_3ds.hpp"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
const int kChannels = 24;
const u32 kStreamBuffers = 4;
const u32 kStreamFrames = 1024;
// The DSP ramps a gain change over its next frame (160 samples): a fade-out ends within this many frames.
const u32 kFadeFrames = 3;
const s64 kFeedPeriodNs = 10 * 1000 * 1000;

struct Channel
{
    enum State
    {
        kFree,
        kBusy,
        kFading
    };
    State mState = kFree;
    u32 mStopFrame = 0;
    // The memory it plays from, for DspVoices_StopIn.
    const u8* mData = NULL;
    u32 mBytes = 0;
    ndspWaveBuf mBuffers[kStreamBuffers] = {};
    u32 mNext = 0;
    s16* mStream = NULL;   // a stream's buffers, in linear memory
    u8* mSigned = NULL;    // unsigned 8-bit frames made signed, in linear memory
    u32 mSignedBytes = 0;
};

Channel sChannels[kChannels];
bool sPaused;
void (*sFeed)();
Thread sThread;
volatile bool sQuit;

void SetMix(int ch, float left, float right)
{
    float mix[12] = {left, right};
    ndspChnSetMix(ch, mix);
}

void Clear(int ch)
{
    Channel& c = sChannels[ch];
    ndspChnWaveBufClear(ch);
    memset(c.mBuffers, 0, sizeof(c.mBuffers));
    c.mNext = 0;
    c.mData = NULL;
    c.mState = Channel::kFree;
}

// A free channel, else the one faded out longest.
int Acquire()
{
    DspVoices_Update();
    int fading = -1;
    for (int ch = 0; ch < kChannels; ch++)
    {
        const Channel& c = sChannels[ch];
        if (c.mState == Channel::kFree)
        {
            return ch;
        }
        if (c.mState == Channel::kFading && (fading < 0 || (s32)(c.mStopFrame - sChannels[fading].mStopFrame) < 0))
        {
            fading = ch;
        }
    }
    if (fading >= 0)
    {
        Clear(fading);
    }
    return fading;
}

void Start(int ch, u16 format, float rate, float left, float right)
{
    sChannels[ch].mState = Channel::kBusy;
    ndspChnSetFormat(ch, format);
    ndspChnSetRate(ch, rate);
    SetMix(ch, left, right);
    ndspChnSetPaused(ch, sPaused);
}

// With GTA2_3DS_STATS, the share of the system core the feed takes, and the channels in use, every 5 s.
struct Stats
{
    bool mOn = false;
    u64 mStart = 0;
    u64 mBusy = 0;
    u64 mMax = 0;
    int mVoicesMax = 0;
} sStats;

void FeedThread(void*)
{
    while (!sQuit)
    {
        const u64 t0 = svcGetSystemTick();
        sFeed();
        if (sStats.mOn)
        {
            const u64 t1 = svcGetSystemTick();
            sStats.mBusy += t1 - t0;
            sStats.mMax = t1 - t0 > sStats.mMax ? t1 - t0 : sStats.mMax;
            int voices = 0;
            for (const Channel& c : sChannels)
            {
                voices += c.mState != Channel::kFree;
            }
            sStats.mVoicesMax = voices > sStats.mVoicesMax ? voices : sStats.mVoicesMax;
            if (!sStats.mStart)
            {
                sStats.mStart = t0;
            }
            else if (t1 - sStats.mStart >= 5 * (u64)SYSCLOCK_ARM11)
            {
                Log3DS_Printf("dsp: feed %.2f %% of the time (at most %.2f ms), voices %d at most\n",
                              sStats.mBusy * 100.0 / (t1 - sStats.mStart), sStats.mMax * 1000.0 / SYSCLOCK_ARM11, sStats.mVoicesMax);
                sStats.mStart = t1;
                sStats.mBusy = 0;
                sStats.mMax = 0;
                sStats.mVoicesMax = 0;
            }
        }
        svcSleepThread(kFeedPeriodNs);
    }
}

void Close()
{
    sQuit = true;
    threadJoin(sThread, U64_MAX);
    threadFree(sThread);
    for (int ch = 0; ch < kChannels; ch++)
    {
        ndspChnReset(ch);
    }
    ndspExit();
}
} // namespace

bool DspVoices_Open(void (*pFeed)())
{
    if (R_FAILED(ndspInit()))
    {
        return false;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetClippingMode(NDSP_CLIP_SOFT);
    for (int ch = 0; ch < kChannels; ch++)
    {
        ndspChnReset(ch);
        ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
    }
    sFeed = pFeed;
    sStats.mOn = getenv("GTA2_3DS_STATS") != NULL;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    priority = priority - 1 < 0x19 ? 0x19 : (priority - 1 > 0x2F ? 0x2F : priority - 1);
    // On the system core, of which the application gets 30 %, else on the game's.
    if (R_SUCCEEDED(APT_SetAppCpuTimeLimit(30)))
    {
        sThread = threadCreate(FeedThread, NULL, 64 * 1024, priority, 1, false);
    }
    if (!sThread)
    {
        sThread = threadCreate(FeedThread, NULL, 64 * 1024, priority, -2, false);
    }
    if (!sThread)
    {
        ndspExit();
        return false;
    }
    atexit(Close);
    return true;
}

s32 DspVoices_Play(const DspSound& sound, float rate, float left, float right)
{
    const u32 frameBytes = (sound.mStereo ? 2 : 1) * (sound.m16 ? 2 : 1);
    const u32 bytes = sound.mLoopEnd * frameBytes;
    const bool convert = !sound.m16 && !sound.mSigned8;
    if (!bytes || (!convert && !osConvertVirtToPhys(sound.mData)))
    {
        return -1;
    }
    const int ch = Acquire();
    if (ch < 0)
    {
        return -1;
    }
    Channel& c = sChannels[ch];
    const u8* pData = static_cast<const u8*>(sound.mData);
    if (convert)
    {
        if (c.mSignedBytes < bytes)
        {
            linearFree(c.mSigned);
            c.mSigned = static_cast<u8*>(linearAlloc(bytes));
            c.mSignedBytes = c.mSigned ? bytes : 0;
            if (!c.mSigned)
            {
                return -1;
            }
        }
        for (u32 i = 0; i < bytes; i++)
        {
            c.mSigned[i] = pData[i] ^ 0x80;
        }
        DSP_FlushDataCache(c.mSigned, bytes);
        pData = c.mSigned;
    }
    c.mData = static_cast<const u8*>(sound.mData);
    c.mBytes = bytes;
    Start(ch, NDSP_CHANNELS(sound.mStereo ? 2 : 1) | NDSP_ENCODING(sound.m16 ? NDSP_ENCODING_PCM16 : NDSP_ENCODING_PCM8), rate, left, right);
    // Up to the loop's end, then the loop again: Miles's first pass starts at the sound's start.
    ndspWaveBuf* pBuffers = c.mBuffers;
    pBuffers[0].data_vaddr = pData;
    pBuffers[0].nsamples = sound.mLoopEnd;
    ndspChnWaveBufAdd(ch, &pBuffers[0]);
    if (sound.mLoopCount != 1 && sound.mLoopStart < sound.mLoopEnd)
    {
        // A count above 1 plays the loop once more: the game only uses 0 and 1.
        pBuffers[1].data_vaddr = pData + sound.mLoopStart * frameBytes;
        pBuffers[1].nsamples = sound.mLoopEnd - sound.mLoopStart;
        pBuffers[1].looping = sound.mLoopCount == 0;
        ndspChnWaveBufAdd(ch, &pBuffers[1]);
    }
    return ch;
}

s32 DspVoices_OpenStream()
{
    const int ch = Acquire();
    if (ch < 0)
    {
        return -1;
    }
    Channel& c = sChannels[ch];
    if (!c.mStream)
    {
        c.mStream = static_cast<s16*>(linearAlloc(kStreamBuffers * kStreamFrames * 2 * sizeof(s16)));
        if (!c.mStream)
        {
            return -1;
        }
    }
    Start(ch, NDSP_FORMAT_STEREO_PCM16, NDSP_SAMPLE_RATE, 0, 0);
    return ch;
}

void DspVoices_Set(s32 voice, float rate, float left, float right)
{
    ndspChnSetRate(voice, rate);
    SetMix(voice, left, right);
}

bool DspVoices_Playing(s32 voice)
{
    for (const ndspWaveBuf& b : sChannels[voice].mBuffers)
    {
        if (b.status == NDSP_WBUF_QUEUED || b.status == NDSP_WBUF_PLAYING)
        {
            return true;
        }
    }
    return false;
}

void DspVoices_Stop(s32 voice, bool now)
{
    if (now || !DspVoices_Playing(voice))
    {
        Clear(voice);
        return;
    }
    SetMix(voice, 0, 0);
    sChannels[voice].mState = Channel::kFading;
    sChannels[voice].mStopFrame = ndspGetFrameCount();
}

void DspVoices_Update()
{
    const u32 frame = ndspGetFrameCount();
    for (int ch = 0; ch < kChannels; ch++)
    {
        const Channel& c = sChannels[ch];
        if (c.mState == Channel::kFading && (frame - c.mStopFrame >= kFadeFrames || !DspVoices_Playing(ch)))
        {
            Clear(ch);
        }
    }
}

void DspVoices_StopIn(const void* p, size_t len)
{
    const u8* pStart = static_cast<const u8*>(p);
    bool stopped = false;
    for (int ch = 0; ch < kChannels; ch++)
    {
        const Channel& c = sChannels[ch];
        if (c.mState != Channel::kFree && c.mData && c.mData < pStart + len && c.mData + c.mBytes > pStart)
        {
            Clear(ch);
            stopped = true;
        }
    }
    // The DSP reads on until its next frame.
    const u32 frame = ndspGetFrameCount();
    for (int i = 0; stopped && i < 20 && ndspGetFrameCount() - frame < 2; i++)
    {
        svcSleepThread(1000 * 1000);
    }
}

s16* DspVoices_Buffer(s32 voice, u32* pFrames)
{
    Channel& c = sChannels[voice];
    const ndspWaveBuf& b = c.mBuffers[c.mNext];
    if (b.status != NDSP_WBUF_FREE && b.status != NDSP_WBUF_DONE)
    {
        return NULL;
    }
    *pFrames = kStreamFrames;
    return c.mStream + c.mNext * kStreamFrames * 2;
}

void DspVoices_Submit(s32 voice, u32 frames)
{
    Channel& c = sChannels[voice];
    s16* pFrames = c.mStream + c.mNext * kStreamFrames * 2;
    DSP_FlushDataCache(pFrames, frames * 2 * sizeof(s16));
    ndspWaveBuf& b = c.mBuffers[c.mNext];
    memset(&b, 0, sizeof(b));
    b.data_vaddr = pFrames;
    b.nsamples = frames;
    ndspChnWaveBufAdd(voice, &b);
    c.mNext = (c.mNext + 1) % kStreamBuffers;
}

void DspVoices_Flush(s32 voice)
{
    Channel& c = sChannels[voice];
    ndspChnWaveBufClear(voice);
    memset(c.mBuffers, 0, sizeof(c.mBuffers));
    c.mNext = 0;
}

void DspVoices_Pause(bool paused)
{
    sPaused = paused;
    for (int ch = 0; ch < kChannels; ch++)
    {
        ndspChnSetPaused(ch, paused);
    }
}

void DspVoices_SetMasterVolume(float volume)
{
    ndspSetMasterVol(volume);
}

void* DspVoices_Alloc(size_t size)
{
    void* p = linearAlloc(size);
    if (p)
    {
        memset(p, 0, size);
    }
    return p;
}

void DspVoices_Free(void* p)
{
    if (p)
    {
        linearFree(p);
    }
}

void DspVoices_Written(const void* p, size_t len)
{
    DSP_FlushDataCache(p, len);
}
