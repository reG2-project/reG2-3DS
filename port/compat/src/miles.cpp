// Miles Sound System (the AIL_* subset GTA2 uses) on SDL3 audio: 2D samples from the game's sound
// banks, and WAV streams (radio stations, voices) decoded while they play, PCM or IMA ADPCM.
// The 3D provider is in miles3d.cpp. On the 3DS the DSP's channels play the 2D samples and streams instead.
#include "types.hpp"
#include "3rdParty/mss.h"
#include "miles3d.hpp"
#include "port_platform.hpp"
#ifdef __3DS__
#include "../../3ds/dsp_voices.hpp"
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <algorithm>
#include <mutex>
#include <vector>

#undef fopen

namespace
{
const int kOutRate = Miles3D_kRate;

std::recursive_mutex& Lock()
{
    static std::recursive_mutex sLock;
    return sLock;
}

// Held while the audio thread decodes streams without Lock(): what closes one takes it first, then Lock().
std::mutex& IoLock()
{
    static std::mutex sLock;
    return sLock;
}

struct Sample
{
    const u8* mData = NULL;
    u32 mLen = 0;
    S32 mFormat = DIG_F_MONO_16;
    bool mSigned8 = false;
    S32 mRate = 22050;
    S32 mVolume = 127;
    S32 mPan = 64;
    S32 mLoopCount = 1;
    S32 mLoopStart = 0;
    S32 mLoopEnd = -1;
    U32 mStatus = SMP_DONE;
    double mPos = 0; // in frames
    float mGain[2] = {-1, -1}; // left and right as mixed, -1 until the first frame
    int mFade = -1;            // frames left in a fade-out, -1 when not stopping
    std::vector<u8> mOwned;    // a fading copy's own frames, mData pointing here
    s32 mVoice = -1;           // its DSP channel (3DS)
};

// The frames a stream keeps decoded ahead (0.37 s at 22050 Hz), and the bytes it reads from its file at once.
const u32 kQueueFrames = 8192;
const u32 kRawBytes = 32768;

// A WAV file that the audio thread reads in large blocks and decodes ahead into a queue of frames, outside
// the game's lock (Prefetch), so the game never waits for the file system.
struct Stream
{
    FILE* mFile = NULL;
    int mFormatTag = 1;
    int mChannels = 1;
    int mRate = 22050;
    int mBits = 16;
    int mBlockAlign = 0;
    int mFramesPerBlock = 0;
    long mDataStart = 0;
    u32 mDataSize = 0;
    u32 mTotalFrames = 0;
    S32 mVolume = 127;
    S32 mPlayRate = 0;
    S32 mLoopCount = 1;
    U32 mStatus = SMP_DONE;
    float mGain = -1;
    int mFade = -1;

    // Stereo frames from the play position on, in a ring.
    std::vector<s16> mQueue = std::vector<s16>(kQueueFrames * 2);
    u32 mQueueHead = 0;
    u32 mQueued = 0;
    u32 mPos = 0;     // the file's frame at the head
    double mFrac = 0; // the position between the head and the next frame
    // Where decoding goes on: the file's frame, the plays left (0 for ever), and whether it reached the end.
    u32 mDecodeFrame = 0;
    S32 mDecodeLoops = 1;
    bool mDecodeEnd = true;
    u32 mSeeks = 0; // a start or a seek drops what a decode under way brings

    // The audio thread's alone: the data chunk's bytes from mRawOffset on.
    std::vector<u8> mRaw;
    u32 mRawOffset = 0;

    s32 mVoice = -1;   // its DSP channel (3DS)
    u32 mFedSeeks = 0; // mSeeks as the DSP's buffers start
};

struct Driver
{
    S32 mMasterVolume = 127;
    std::vector<Sample*> mSamples;
    std::vector<Stream*> mStreams;
    // Copies of samples stopped while playing, and closed streams, until their fade-out ends.
    std::vector<Sample*> mFadingSamples;
    std::vector<Stream*> mFadingStreams;
};

// A stop fades out, and a volume or pan change ramps, over this many frames (7.5 ms), as a step in the
// waveform clicks.
const int kRampFrames = kOutRate * 3 / 400;

float Ramp(float current, float target)
{
    const float step = 1.0f / kRampFrames;
    return current < 0 ? target : current + SDL_clamp(target - current, -step, step);
}

// 1 while playing, then down to 0 over a fade-out; false once it ended.
bool FadeStep(int* pFade, float* pGain)
{
    if (*pFade == 0)
    {
        return false;
    }
    *pGain = *pFade > 0 ? (float)(*pFade)-- / kRampFrames : 1.0f;
    return true;
}

Driver* sDriver;

#ifdef __3DS__
// The DSP plays the sounds, unless the CPU mixer below was asked for. It has no limiter: the game's mix
// passes full scale rarely and by little (1.3 at most, crashes and explosions), which its soft clipping takes.
bool sDsp;
#else
const bool sDsp = false;
#endif

const int kImaSteps[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
    88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int kImaIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

s16 ImaNibble(int nibble, int& predictor, int& index)
{
    const int step = kImaSteps[index];
    int diff = step >> 3;
    if (nibble & 1)
    {
        diff += step >> 2;
    }
    if (nibble & 2)
    {
        diff += step >> 1;
    }
    if (nibble & 4)
    {
        diff += step;
    }
    predictor += (nibble & 8) ? -diff : diff;
    predictor = predictor < -32768 ? -32768 : (predictor > 32767 ? 32767 : predictor);
    index += kImaIndex[nibble];
    index = index < 0 ? 0 : (index > 88 ? 88 : index);
    return (s16)predictor;
}

u16 Rd16(const u8* p)
{
    return (u16)(p[0] | (p[1] << 8));
}

u32 Rd32(const u8* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

bool OpenWav(Stream* s, const char* path)
{
    char real[PATH_MAX];
    s->mFile = fopen(port_resolve_path(path, real), "rb");
    if (!s->mFile)
    {
        return false;
    }
    u8 hdr[12];
    if (fread(hdr, 1, 12, s->mFile) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))
    {
        return false;
    }
    bool haveFmt = false;
    u8 chunk[8];
    while (fread(chunk, 1, 8, s->mFile) == 8)
    {
        const u32 size = Rd32(chunk + 4);
        if (!memcmp(chunk, "fmt ", 4))
        {
            u8 fmt[64] = {};
            const u32 n = size < sizeof(fmt) ? size : sizeof(fmt);
            if (fread(fmt, 1, n, s->mFile) != n)
            {
                return false;
            }
            fseek(s->mFile, (long)(size - n + (size & 1)), SEEK_CUR);
            s->mFormatTag = Rd16(fmt);
            s->mChannels = Rd16(fmt + 2);
            s->mRate = (int)Rd32(fmt + 4);
            s->mBlockAlign = Rd16(fmt + 12);
            s->mBits = Rd16(fmt + 14);
            s->mFramesPerBlock = size >= 20 ? Rd16(fmt + 18) : 0;
            haveFmt = true;
        }
        else if (!memcmp(chunk, "data", 4))
        {
            s->mDataStart = ftell(s->mFile);
            s->mDataSize = size;
            break;
        }
        else
        {
            fseek(s->mFile, (long)(size + (size & 1)), SEEK_CUR);
        }
    }
    if (!haveFmt || !s->mDataStart || s->mChannels < 1 || s->mChannels > 2)
    {
        return false;
    }
    if (s->mFormatTag == 1 && (s->mBits == 8 || s->mBits == 16))
    {
        s->mTotalFrames = s->mDataSize / (s->mChannels * s->mBits / 8);
        return true;
    }
    if (s->mFormatTag == 0x11 && s->mBits == 4 && s->mBlockAlign > 4 * s->mChannels)
    {
        if (!s->mFramesPerBlock)
        {
            s->mFramesPerBlock = (s->mBlockAlign - 4 * s->mChannels) * 8 / (4 * s->mChannels) + 1;
        }
        s->mTotalFrames = s->mDataSize / s->mBlockAlign * s->mFramesPerBlock;
        return true;
    }
    return false;
}

// The data chunk's bytes [offset, offset + size), read with the ones after them in one request.
const u8* RawBytes(Stream* s, u32 offset, u32 size)
{
    if (offset < s->mRawOffset || offset + size > s->mRawOffset + s->mRaw.size())
    {
        s->mRaw.resize(SDL_min(SDL_max(size, kRawBytes), s->mDataSize - offset));
        fseek(s->mFile, s->mDataStart + (long)offset, SEEK_SET);
        s->mRaw.resize(fread(s->mRaw.data(), 1, s->mRaw.size(), s->mFile));
        s->mRawOffset = offset;
        if (s->mRaw.size() < size)
        {
            return NULL;
        }
    }
    return &s->mRaw[offset - s->mRawOffset];
}

// Decodes stereo frames from frame f on, at most max and not past f's ADPCM block; 0 when the file ends short.
u32 Decode(Stream* s, u32 f, s16* out, u32 max)
{
    const int ch = s->mChannels;
    if (s->mFormatTag == 1)
    {
        const u32 bytes = s->mBits / 8;
        const u32 count = SDL_min(SDL_min(max, s->mTotalFrames - f), kRawBytes / (ch * bytes));
        const u8* p = RawBytes(s, f * ch * bytes, count * ch * bytes);
        if (!p)
        {
            return 0;
        }
        for (u32 i = 0; i < count * 2; i++)
        {
            const u32 k = i / 2 * ch + (ch == 2 ? i % 2 : 0);
            out[i] = bytes == 2 ? (s16)Rd16(p + k * 2) : (s16)((p[k] - 128) << 8);
        }
        return count;
    }
    const u32 block = f / s->mFramesPerBlock;
    const u8* p = RawBytes(s, block * s->mBlockAlign, s->mBlockAlign);
    if (!p)
    {
        return 0;
    }
    static std::vector<s16> sBlock; // the audio thread's alone
    sBlock.assign((size_t)s->mFramesPerBlock * 2, 0);
    int pred[2];
    int index[2];
    for (int c = 0; c < ch; c++)
    {
        pred[c] = (s16)Rd16(p + c * 4);
        index[c] = SDL_min(p[c * 4 + 2], 88);
        sBlock[c] = (s16)pred[c];
    }
    // After the headers, 4 bytes (8 nibbles) per channel in turn.
    const u8* q = p + 4 * ch;
    const u8* end = p + s->mBlockAlign;
    int frame = 1;
    while (q + 4 * ch <= end && frame < s->mFramesPerBlock)
    {
        for (int c = 0; c < ch; c++)
        {
            for (int k = 0; k < 8 && frame + k < s->mFramesPerBlock; k++)
            {
                const int nibble = (q[k >> 1] >> ((k & 1) * 4)) & 15;
                sBlock[(frame + k) * 2 + c] = ImaNibble(nibble, pred[c], index[c]);
            }
            q += 4;
        }
        frame += 8;
    }
    const u32 first = f - block * s->mFramesPerBlock;
    const u32 count = SDL_min(max, s->mFramesPerBlock - first);
    for (u32 i = 0; i < count; i++)
    {
        out[i * 2] = sBlock[(first + i) * 2];
        out[i * 2 + 1] = sBlock[(first + i) * 2 + ch - 1];
    }
    return count;
}

// Tops the queue up by a quarter at most (whole ADPCM blocks, one file read at most), without the game's
// lock: only the audio thread decodes, and IoLock keeps the stream open meanwhile.
void Prefetch(Stream* s)
{
    static std::vector<s16> sOut(kQueueFrames / 4 * 2); // the audio thread's alone
    u32 seeks;
    u32 frame;
    S32 loops;
    u32 room;
    {
        std::lock_guard<std::recursive_mutex> lock(Lock());
        if (s->mStatus != SMP_PLAYING || s->mDecodeEnd || kQueueFrames - s->mQueued < kQueueFrames / 4)
        {
            return;
        }
        room = kQueueFrames / 4;
        seeks = s->mSeeks;
        frame = s->mDecodeFrame;
        loops = s->mDecodeLoops;
    }
    u32 done = 0;
    bool end = false;
    const u32 unit = s->mFormatTag == 1 ? 1 : SDL_min((u32)s->mFramesPerBlock, room);
    while (room - done >= unit && !end)
    {
        if (frame >= s->mTotalFrames)
        {
            if (loops == 1)
            {
                end = true;
                break;
            }
            loops -= loops > 1;
            frame = 0;
        }
        const u32 n = Decode(s, frame, &sOut[done * 2], room - done);
        end = n == 0;
        frame += n;
        done += n;
    }
    std::lock_guard<std::recursive_mutex> lock(Lock());
    if (seeks != s->mSeeks)
    {
        return;
    }
    for (u32 i = 0; i < done; i++)
    {
        const u32 at = (s->mQueueHead + s->mQueued + i) % kQueueFrames;
        s->mQueue[at * 2] = sOut[i * 2];
        s->mQueue[at * 2 + 1] = sOut[i * 2 + 1];
    }
    s->mQueued += done;
    s->mDecodeFrame = frame;
    s->mDecodeLoops = loops;
    s->mDecodeEnd = end;
}

// Plays on from that frame, decoded again from there.
void Seek(Stream* s, u32 frame)
{
    s->mSeeks++;
    s->mQueued = 0;
    s->mFrac = 0;
    s->mPos = frame;
    s->mDecodeFrame = frame;
    s->mDecodeEnd = false;
}

// The value at a data index: 16-bit signed, or 8-bit with flip8 0x80 for unsigned data.
template <bool k16>
float SampleValue(const u8* pData, u32 idx, u8 flip8)
{
    if (k16)
    {
        Uint16 v;
        SDL_memcpy(&v, pData + idx * 2, 2);
        return (s16)SDL_Swap16LE(v) / 32768.0f;
    }
    return (s8)(pData[idx] ^ flip8) / 128.0f;
}

u32 SampleFrames(const Sample* s)
{
    const int frameBytes = ((s->mFormat & DIG_F_STEREO_MASK) ? 2 : 1) * ((s->mFormat & DIG_F_16BITS_MASK) ? 2 : 1);
    return s->mLen / frameBytes;
}

// The mixing loop for one data format, on local copies of the sample's state. kSteady: neither fading out
// nor ramping, the gains already at their targets.
template <bool kStereo, bool k16, bool kSteady>
void MixSampleAs(Sample* s, float* out, int frames, u32 loopStart, u32 loopEnd, float left, float right)
{
    const double step = (double)s->mRate / kOutRate;
    const u8* pData = s->mData;
    const u8 flip8 = s->mSigned8 ? 0 : 0x80;
    double pos = s->mPos;
    float gainL = s->mGain[0];
    float gainR = s->mGain[1];
    int fadeLeft = s->mFade;
    for (int i = 0; i < frames; i++)
    {
        float fade = 1.0f;
        if (!kSteady)
        {
            if (!FadeStep(&fadeLeft, &fade))
            {
                s->mStatus = SMP_DONE;
                break;
            }
            gainL = Ramp(gainL, left);
            gainR = Ramp(gainR, right);
        }
        u32 f = (u32)pos;
        if (f >= loopEnd)
        {
            if (s->mLoopCount == 1)
            {
                s->mStatus = SMP_DONE;
                break;
            }
            if (s->mLoopCount > 1)
            {
                s->mLoopCount--;
            }
            pos = loopStart + (pos - loopEnd);
            f = (u32)pos;
            if (f >= loopEnd)
            {
                s->mStatus = SMP_DONE;
                break;
            }
        }
        const float t = (float)(pos - f);
        const u32 f2 = f + 1 < loopEnd ? f + 1 : f;
        float l;
        float r;
        if (kStereo)
        {
            l = SampleValue<k16>(pData, f * 2, flip8) * (1 - t) + SampleValue<k16>(pData, f2 * 2, flip8) * t;
            r = SampleValue<k16>(pData, f * 2 + 1, flip8) * (1 - t) + SampleValue<k16>(pData, f2 * 2 + 1, flip8) * t;
        }
        else
        {
            l = r = SampleValue<k16>(pData, f, flip8) * (1 - t) + SampleValue<k16>(pData, f2, flip8) * t;
        }
        out[i * 2] += l * gainL * fade;
        out[i * 2 + 1] += r * gainR * fade;
        pos += step;
    }
    s->mPos = pos;
    s->mGain[0] = gainL;
    s->mGain[1] = gainR;
    s->mFade = fadeLeft;
}

void MixSample(Sample* s, float* out, int frames)
{
    const u32 total = SampleFrames(s);
    if (!s->mData || !total)
    {
        s->mStatus = SMP_DONE;
        return;
    }
    const bool stereo = (s->mFormat & DIG_F_STEREO_MASK) != 0;
    const bool b16 = (s->mFormat & DIG_F_16BITS_MASK) != 0;
    const int frameBytes = (stereo ? 2 : 1) * (b16 ? 2 : 1);
    const u32 loopStart = SDL_min((u32)SDL_max(s->mLoopStart, 0) / frameBytes, total);
    const u32 loopEnd = s->mLoopEnd < 0 ? total : SDL_min((u32)s->mLoopEnd / frameBytes, total);
    const float vol = s->mVolume / 127.0f * sDriver->mMasterVolume / 127.0f;
    const float left = vol * SDL_min(1.0f, (127 - s->mPan) / 63.5f);
    const float right = vol * SDL_min(1.0f, s->mPan / 63.5f);
    void (*mix)(Sample*, float*, int, u32, u32, float, float);
    if (s->mFade < 0 && s->mGain[0] == left && s->mGain[1] == right)
    {
        mix = stereo ? (b16 ? MixSampleAs<true, true, true> : MixSampleAs<true, false, true>)
                     : (b16 ? MixSampleAs<false, true, true> : MixSampleAs<false, false, true>);
    }
    else
    {
        mix = stereo ? (b16 ? MixSampleAs<true, true, false> : MixSampleAs<true, false, false>)
                     : (b16 ? MixSampleAs<false, true, false> : MixSampleAs<false, false, false>);
    }
    mix(s, out, frames, loopStart, loopEnd, left, right);
}

void MixStream(Stream* s, float* out, int frames)
{
    const double step = (double)(s->mPlayRate ? s->mPlayRate : s->mRate) / kOutRate;
    const float vol = s->mVolume / 127.0f * sDriver->mMasterVolume / 127.0f;
    const s16* q = s->mQueue.data();
    for (int i = 0; i < frames; i++)
    {
        // Short of frames: the end, a fade-out the queue does not cover, or the decoding late (it waits).
        if (s->mQueued < 2)
        {
            if (s->mDecodeEnd || s->mFade >= 0)
            {
                s->mStatus = SMP_DONE;
            }
            return;
        }
        float fade;
        if (!FadeStep(&s->mFade, &fade))
        {
            s->mStatus = SMP_DONE;
            return;
        }
        s->mGain = Ramp(s->mGain, vol);
        const u32 a = s->mQueueHead * 2;
        const u32 b = (s->mQueueHead + 1) % kQueueFrames * 2;
        const float t = (float)s->mFrac;
        const float gain = s->mGain * fade / 32768.0f;
        out[i * 2] += (q[a] * (1 - t) + q[b] * t) * gain;
        out[i * 2 + 1] += (q[a + 1] * (1 - t) + q[b + 1] * t) * gain;
        for (s->mFrac += step; s->mFrac >= 1 && s->mQueued; s->mFrac -= 1)
        {
            s->mQueueHead = (s->mQueueHead + 1) % kQueueFrames;
            s->mQueued--;
            s->mPos = s->mPos + 1 < s->mTotalFrames ? s->mPos + 1 : 0;
        }
    }
}

// Sounds at full scale (the frontend's are) sum past it when they overlap. Rather than clipping, the
// mix is turned down for as long as needed, starting kLookahead frames ahead of the peak.
class Limiter
{
  public:
    Limiter()
    {
        std::fill(mNeeded, mNeeded + kLookahead, 1.0f);
    }

    void Process(float* pMix, int frames)
    {
        const float kCeiling = 0.99f;
        const float release = 1.0f - expf(-1.0f / (0.15f * kOutRate));
        for (int i = 0; i < frames; i++)
        {
            float* p = &pMix[i * 2];
            const float peak = SDL_max(fabsf(p[0]), fabsf(p[1]));
            const int in = mPos;
            mDelay[in * 2] = p[0];
            mDelay[in * 2 + 1] = p[1];
            mLoud -= mNeeded[in] < 1.0f;
            mNeeded[in] = peak > kCeiling ? kCeiling / peak : 1.0f;
            mLoud += mNeeded[in] < 1.0f;
            mPos = (mPos + 1) % kLookahead;
            // A linear ramp down to each gain needed in the lookahead, the oldest frame needing its own.
            float gain = mGain + (1.0f - mGain) * release;
            for (int k = 0; mLoud && k < kLookahead; k++)
            {
                const float needed = mNeeded[(mPos + k) % kLookahead];
                gain = SDL_min(gain, needed + (1.0f - needed) * k / kLookahead);
            }
            mGain = gain;
            p[0] = mDelay[mPos * 2] * gain;
            p[1] = mDelay[mPos * 2 + 1] * gain;
        }
    }

  private:
    static const int kLookahead = 64;
    float mDelay[kLookahead * 2] = {};
    float mNeeded[kLookahead];
    int mPos = 0;
    int mLoud = 0; // entries of mNeeded below 1: without any, the gain only rises
    float mGain = 1.0f;
};

Limiter sLimiter;

void PrefetchStreams()
{
    std::lock_guard<std::mutex> io(IoLock());
    static std::vector<Stream*> sStreams; // the audio thread's alone
    {
        std::lock_guard<std::recursive_mutex> lock(Lock());
        sStreams.clear();
        if (sDriver)
        {
            sStreams = sDriver->mStreams;
        }
    }
    for (Stream* s : sStreams)
    {
        Prefetch(s);
    }
}

void SDLCALL AudioCallback(void*, SDL_AudioStream* stream, int additional, int)
{
    const int frames = additional / (int)(2 * sizeof(float));
    if (frames <= 0)
    {
        return;
    }
    PrefetchStreams();
    std::vector<float> mix((size_t)frames * 2, 0.0f);
    {
        std::lock_guard<std::recursive_mutex> lock(Lock());
        if (sDriver)
        {
            for (Sample* s : sDriver->mSamples)
            {
                if (s->mStatus == SMP_PLAYING)
                {
                    MixSample(s, mix.data(), frames);
                }
            }
            for (Stream* s : sDriver->mStreams)
            {
                if (s->mStatus == SMP_PLAYING)
                {
                    MixStream(s, mix.data(), frames);
                }
            }
            for (size_t i = 0; i < sDriver->mFadingSamples.size();)
            {
                Sample* s = sDriver->mFadingSamples[i];
                MixSample(s, mix.data(), frames);
                if (s->mStatus == SMP_PLAYING)
                {
                    i++;
                    continue;
                }
                delete s;
                sDriver->mFadingSamples.erase(sDriver->mFadingSamples.begin() + i);
            }
            for (size_t i = 0; i < sDriver->mFadingStreams.size();)
            {
                Stream* s = sDriver->mFadingStreams[i];
                MixStream(s, mix.data(), frames);
                if (s->mStatus == SMP_PLAYING)
                {
                    i++;
                    continue;
                }
                fclose(s->mFile);
                delete s;
                sDriver->mFadingStreams.erase(sDriver->mFadingStreams.begin() + i);
            }
        }
    }
    Miles3D_Render(mix.data(), frames);
    sLimiter.Process(mix.data(), frames);
    for (float& v : mix)
    {
        v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
    }
    SDL_PutAudioStreamData(stream, mix.data(), (int)(mix.size() * sizeof(float)));
}

#ifdef __3DS__
void SampleGains(const Sample* s, float* pLeft, float* pRight)
{
    const float vol = s->mVolume / 127.0f;
    *pLeft = vol * SDL_min(1.0f, (127 - s->mPan) / 63.5f);
    *pRight = vol * SDL_min(1.0f, s->mPan / 63.5f);
}

void DspPlay(Sample* s)
{
    const u32 total = SampleFrames(s);
    const bool stereo = (s->mFormat & DIG_F_STEREO_MASK) != 0;
    const bool b16 = (s->mFormat & DIG_F_16BITS_MASK) != 0;
    const int frameBytes = (stereo ? 2 : 1) * (b16 ? 2 : 1);
    const u32 loopStart = SDL_min((u32)SDL_max(s->mLoopStart, 0) / frameBytes, total);
    const u32 loopEnd = s->mLoopEnd < 0 ? total : SDL_min((u32)s->mLoopEnd / frameBytes, total);
    const DspSound sound = {s->mData, stereo, b16, s->mSigned8, loopStart, loopEnd, s->mLoopCount};
    float left;
    float right;
    SampleGains(s, &left, &right);
    s->mVoice = s->mData ? DspVoices_Play(sound, (float)s->mRate, left, right) : -1;
    s->mStatus = s->mVoice >= 0 ? SMP_PLAYING : SMP_DONE;
}

void DspSet(Sample* s)
{
    if (s->mVoice >= 0)
    {
        float left;
        float right;
        SampleGains(s, &left, &right);
        DspVoices_Set(s->mVoice, (float)s->mRate, left, right);
    }
}

void DspSet(Stream* s)
{
    if (s->mVoice >= 0)
    {
        const float gain = s->mVolume / 127.0f;
        DspVoices_Set(s->mVoice, (float)(s->mPlayRate ? s->mPlayRate : s->mRate), gain, gain);
    }
}

// Hands the stream's decoded frames to its voice in whole buffers, but for the last.
void FeedStream(Stream* s)
{
    if (s->mFedSeeks != s->mSeeks)
    {
        DspVoices_Flush(s->mVoice);
        s->mFedSeeks = s->mSeeks;
    }
    u32 room;
    s16* pOut;
    while (s->mQueued && (pOut = DspVoices_Buffer(s->mVoice, &room)) && (s->mQueued >= room || s->mDecodeEnd))
    {
        const u32 n = SDL_min(room, s->mQueued);
        for (u32 i = 0; i < n; i++)
        {
            const u32 at = (s->mQueueHead + i) % kQueueFrames;
            pOut[i * 2] = s->mQueue[at * 2];
            pOut[i * 2 + 1] = s->mQueue[at * 2 + 1];
        }
        DspVoices_Submit(s->mVoice, n);
        s->mQueueHead = (s->mQueueHead + n) % kQueueFrames;
        s->mQueued -= n;
        s->mPos = (s->mPos + n) % s->mTotalFrames;
    }
    if (s->mDecodeEnd && !s->mQueued && !DspVoices_Playing(s->mVoice))
    {
        s->mStatus = SMP_DONE;
    }
}

// Every few milliseconds on the system core: decodes the streams ahead, feeds their voices and notes the
// sounds that ended.
void FeedDsp()
{
    PrefetchStreams();
    std::lock_guard<std::recursive_mutex> lock(Lock());
    DspVoices_Update();
    if (!sDriver)
    {
        return;
    }
    for (Sample* s : sDriver->mSamples)
    {
        if (s->mVoice >= 0 && !DspVoices_Playing(s->mVoice))
        {
            DspVoices_Stop(s->mVoice, true);
            s->mVoice = -1;
            s->mStatus = SMP_DONE;
        }
    }
    for (Stream* s : sDriver->mStreams)
    {
        if (s->mStatus == SMP_PLAYING)
        {
            FeedStream(s);
        }
    }
}
#else
void DspSet(Sample*)
{
}

void DspSet(Stream*)
{
}
#endif

Sample* S(HSAMPLE h)
{
    return reinterpret_cast<Sample*>(h);
}

// Stops the sample at once for the game, while a copy of it fades out. The copy keeps the frames the fade
// reads, as the game may free the sample's data or load other sounds over it.
void StopSample(Sample* s)
{
#ifdef __3DS__
    if (s->mVoice >= 0)
    {
        DspVoices_Stop(s->mVoice, false);
        s->mVoice = -1;
    }
#endif
    const u32 first = (u32)s->mPos;
    if (!sDsp && sDriver && s->mStatus == SMP_PLAYING && s->mFade < 0 && s->mData && first < SampleFrames(s))
    {
        const u32 frameBytes = s->mLen / SampleFrames(s);
        const u32 frames = SDL_min((u32)(kRampFrames * (double)s->mRate / kOutRate) + 2, SampleFrames(s) - first);
        Sample* pCopy = new Sample(*s);
        pCopy->mOwned.assign(s->mData + first * frameBytes, s->mData + (first + frames) * frameBytes);
        pCopy->mData = pCopy->mOwned.data();
        pCopy->mLen = frames * frameBytes;
        pCopy->mPos -= first;
        pCopy->mLoopCount = 1;
        pCopy->mLoopEnd = -1;
        pCopy->mFade = kRampFrames;
        sDriver->mFadingSamples.push_back(pCopy);
    }
    s->mStatus = SMP_DONE;
}

void DropFading()
{
    if (!sDriver)
    {
        return;
    }
    for (Sample* s : sDriver->mFadingSamples)
    {
        delete s;
    }
    for (Stream* s : sDriver->mFadingStreams)
    {
        fclose(s->mFile);
        delete s;
    }
    sDriver->mFadingSamples.clear();
    sDriver->mFadingStreams.clear();
}

Stream* St(HSTREAM h)
{
    return reinterpret_cast<Stream*>(h);
}
} // namespace

#define LOCK std::lock_guard<std::recursive_mutex> lock(Lock())

// The memory the game loads its sound banks into: on the 3DS, memory the DSP plays from.
DXDEC void FAR* AILCALL AIL_mem_alloc_lock(U32 size)
{
#ifdef __3DS__
    return DspVoices_Alloc(size);
#else
    return calloc(1, size);
#endif
}

DXDEC void AILCALL AIL_mem_free_lock(void FAR* ptr)
{
#ifdef __3DS__
    DspVoices_Free(ptr);
#else
    free(ptr);
#endif
}

void port_SoundDataLoaded(const void* p, size_t len)
{
#ifdef __3DS__
    DspVoices_Written(p, len);
#endif
}

void port_StopSamplesIn(const void* p, size_t len)
{
    LOCK;
    if (!sDriver)
    {
        return;
    }
    for (Sample* s : sDriver->mSamples)
    {
        if (s->mData >= static_cast<const u8*>(p) && s->mData < static_cast<const u8*>(p) + len)
        {
            StopSample(s);
        }
    }
#ifdef __3DS__
    if (sDsp)
    {
        DspVoices_StopIn(p, len);
    }
#endif
}

DXDEC S32 AILCALL AIL_startup(void)
{
    return SDL_InitSubSystem(SDL_INIT_AUDIO) ? 1 : 0;
}

DXDEC S32 AILCALL AIL_get_preference(U32)
{
    return 0;
}

DXDEC void AILCALL AIL_shutdown(void)
{
}

DXDEC S32 AILCALL AIL_set_preference(U32, S32)
{
    return 0;
}

// Intervals of 1/60 s.
DXDEC void AILCALL AIL_delay(S32 intervals)
{
    SDL_Delay((Uint32)(intervals * 1000 / 60));
}

// The game closes and reopens the driver to switch 3D sound on or off: the SDL device stays open
// (silent while no driver), since reopening it takes a noticeable time.
static SDL_AudioStream* sDevice;
// The device plays while the game holds the driver and the system has not suspended the game.
static bool sReleased;
static bool sSuspended;

static void UpdatePause()
{
#ifdef __3DS__
    if (sDsp)
    {
        DspVoices_Pause(sReleased || sSuspended);
    }
#endif
    if (sDevice)
    {
        if (sReleased || sSuspended)
        {
            SDL_PauseAudioStreamDevice(sDevice);
        }
        else
        {
            SDL_ResumeAudioStreamDevice(sDevice);
        }
    }
}

void port_SuspendSound(bool suspended)
{
    sSuspended = suspended;
    UpdatePause();
}

DXDEC S32 AILCALL AIL_waveOutOpen(HDIGDRIVER FAR* drvr, LPHWAVEOUT FAR*, S32, LPWAVEFORMAT)
{
    LOCK;
    if (getenv("GTA2_NO_SOUND"))
    {
        return -1;
    }
#ifdef __3DS__
    static bool sChosen;
    if (!sChosen)
    {
        sChosen = true;
        // SDL's other drivers (dummy, disk) and GTA2_3DS_CPU_MIX=1 take the CPU mixer.
        const char* driver = getenv("GTA2_AUDIO_DRIVER");
        sDsp = !getenv("GTA2_3DS_CPU_MIX") && (!driver || !strcmp(driver, "n3ds")) && DspVoices_Open(FeedDsp);
        if (sDsp)
        {
            SDL_Log("Audio: the DSP's channels");
        }
    }
    if (sDsp)
    {
        DspVoices_SetMasterVolume(1.0f);
    }
#endif
    if (!sDsp && !sDevice)
    {
        const SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, kOutRate};
        sDevice = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, AudioCallback, NULL);
        if (!sDevice)
        {
            SDL_Log("Audio: %s", SDL_GetError());
            return -1;
        }
        SDL_Log("Audio: %s", SDL_GetCurrentAudioDriver());
    }
    sDriver = new Driver();
    sReleased = false;
    UpdatePause();
    *drvr = reinterpret_cast<HDIGDRIVER>(sDriver);
    return 0;
}

DXDEC void AILCALL AIL_waveOutClose(HDIGDRIVER)
{
    std::lock_guard<std::mutex> io(IoLock());
    LOCK;
#ifdef __3DS__
    for (size_t i = 0; sDriver && i < sDriver->mSamples.size(); i++)
    {
        StopSample(sDriver->mSamples[i]);
    }
    for (size_t i = 0; sDriver && i < sDriver->mStreams.size(); i++)
    {
        Stream* s = sDriver->mStreams[i];
        if (s->mVoice >= 0)
        {
            DspVoices_Stop(s->mVoice, false);
            s->mVoice = -1;
        }
    }
#endif
    DropFading();
    delete sDriver;
    sDriver = NULL;
}

DXDEC S32 AILCALL AIL_digital_handle_release(HDIGDRIVER)
{
    if (sDriver)
    {
        sReleased = true;
        UpdatePause();
    }
    return 1;
}

DXDEC S32 AILCALL AIL_digital_handle_reacquire(HDIGDRIVER)
{
    if (sDriver)
    {
        sReleased = false;
        UpdatePause();
    }
    return 1;
}

DXDEC HSAMPLE AILCALL AIL_allocate_sample_handle(HDIGDRIVER)
{
    LOCK;
    if (!sDriver)
    {
        return NULL;
    }
    Sample* s = new Sample();
    sDriver->mSamples.push_back(s);
    return reinterpret_cast<HSAMPLE>(s);
}

DXDEC void AILCALL AIL_release_sample_handle(HSAMPLE h)
{
    LOCK;
    if (sDriver && h)
    {
        StopSample(S(h));
        sDriver->mSamples.erase(std::find(sDriver->mSamples.begin(), sDriver->mSamples.end(), S(h)));
        delete S(h);
    }
}

DXDEC void AILCALL AIL_init_sample(HSAMPLE h)
{
    LOCK;
    StopSample(S(h));
    *S(h) = Sample();
}

DXDEC void AILCALL AIL_set_sample_address(HSAMPLE h, void FAR* start, U32 len)
{
    LOCK;
    StopSample(S(h));
    S(h)->mData = static_cast<const u8*>(start);
    S(h)->mLen = len;
    S(h)->mPos = 0;
}

DXDEC void AILCALL AIL_set_sample_type(HSAMPLE h, S32 format, U32 flags)
{
    LOCK;
    S(h)->mFormat = format;
    S(h)->mSigned8 = (flags & DIG_PCM_SIGN) != 0;
}

DXDEC void AILCALL AIL_start_sample(HSAMPLE h)
{
    LOCK;
    StopSample(S(h));
    S(h)->mPos = 0;
    S(h)->mGain[0] = S(h)->mGain[1] = -1;
    S(h)->mStatus = SMP_PLAYING;
#ifdef __3DS__
    if (sDsp)
    {
        DspPlay(S(h));
    }
#endif
}

DXDEC void AILCALL AIL_end_sample(HSAMPLE h)
{
    LOCK;
    StopSample(S(h));
}

DXDEC void AILCALL AIL_set_sample_playback_rate(HSAMPLE h, S32 playback_rate)
{
    LOCK;
    S(h)->mRate = playback_rate > 0 ? playback_rate : 1;
    DspSet(S(h));
}

DXDEC void AILCALL AIL_set_sample_volume(HSAMPLE h, S32 volume)
{
    LOCK;
    S(h)->mVolume = SDL_clamp(volume, 0, 127);
    DspSet(S(h));
}

DXDEC void AILCALL AIL_set_sample_pan(HSAMPLE h, S32 pan)
{
    LOCK;
    S(h)->mPan = SDL_clamp(pan, 0, 127);
    DspSet(S(h));
}

DXDEC void AILCALL AIL_set_sample_loop_count(HSAMPLE h, S32 loop_count)
{
    LOCK;
    S(h)->mLoopCount = loop_count;
}

DXDEC void AILCALL AIL_set_sample_loop_block(HSAMPLE h, S32 loop_start_offset, S32 loop_end_offset)
{
    LOCK;
    S(h)->mLoopStart = loop_start_offset;
    S(h)->mLoopEnd = loop_end_offset;
}

DXDEC U32 AILCALL AIL_sample_status(HSAMPLE h)
{
    LOCK;
    return h ? S(h)->mStatus : SMP_DONE;
}

DXDEC void AILCALL AIL_set_digital_master_volume(HDIGDRIVER, S32 master_volume)
{
    LOCK;
    if (sDriver)
    {
        sDriver->mMasterVolume = SDL_clamp(master_volume, 0, 127);
    }
    Miles3D_SetMasterVolume(SDL_clamp(master_volume, 0, 127));
#ifdef __3DS__
    if (sDsp)
    {
        DspVoices_SetMasterVolume(SDL_clamp(master_volume, 0, 127) / 127.0f);
    }
#endif
}

DXDEC HSTREAM AILCALL AIL_open_stream(HDIGDRIVER, char FAR* filename, S32)
{
    Stream* s = new Stream();
    if (!sDriver || !OpenWav(s, filename))
    {
        if (s->mFile)
        {
            fclose(s->mFile);
        }
        delete s;
        return NULL;
    }
    LOCK;
    sDriver->mStreams.push_back(s);
    return reinterpret_cast<HSTREAM>(s);
}

DXDEC void AILCALL AIL_close_stream(HSTREAM h)
{
    std::lock_guard<std::mutex> io(IoLock());
    LOCK;
    if (sDriver && h)
    {
        sDriver->mStreams.erase(std::find(sDriver->mStreams.begin(), sDriver->mStreams.end(), St(h)));
#ifdef __3DS__
        if (St(h)->mVoice >= 0)
        {
            DspVoices_Stop(St(h)->mVoice, false);
            St(h)->mStatus = SMP_DONE;
        }
#endif
        if (St(h)->mStatus == SMP_PLAYING)
        {
            St(h)->mFade = kRampFrames;
            sDriver->mFadingStreams.push_back(St(h));
            return;
        }
        fclose(St(h)->mFile);
        delete St(h);
    }
}

DXDEC void AILCALL AIL_start_stream(HSTREAM h)
{
    LOCK;
    Seek(St(h), 0);
    St(h)->mDecodeLoops = St(h)->mLoopCount;
    St(h)->mGain = -1;
    St(h)->mStatus = SMP_PLAYING;
#ifdef __3DS__
    if (sDsp)
    {
        if (St(h)->mVoice < 0)
        {
            St(h)->mVoice = DspVoices_OpenStream();
        }
        DspSet(St(h));
        St(h)->mStatus = St(h)->mVoice >= 0 ? SMP_PLAYING : SMP_DONE;
    }
#endif
}

DXDEC void AILCALL AIL_set_stream_volume(HSTREAM h, S32 volume)
{
    LOCK;
    St(h)->mVolume = SDL_clamp(volume, 0, 127);
    DspSet(St(h));
}

DXDEC S32 AILCALL AIL_stream_volume(HSTREAM h)
{
    LOCK;
    return St(h)->mVolume;
}

DXDEC void AILCALL AIL_set_stream_playback_rate(HSTREAM h, S32 rate)
{
    LOCK;
    St(h)->mPlayRate = rate;
    DspSet(St(h));
}

DXDEC S32 AILCALL AIL_stream_playback_rate(HSTREAM h)
{
    LOCK;
    return St(h)->mPlayRate ? St(h)->mPlayRate : St(h)->mRate;
}

DXDEC void AILCALL AIL_set_stream_loop_count(HSTREAM h, S32 count)
{
    LOCK;
    St(h)->mLoopCount = count;
    St(h)->mDecodeLoops = count;
}

DXDEC S32 AILCALL AIL_stream_status(HSTREAM h)
{
    LOCK;
    return h ? (S32)St(h)->mStatus : SMP_DONE;
}

DXDEC void AILCALL AIL_set_stream_ms_position(HSTREAM h, S32 milliseconds)
{
    LOCK;
    Stream* s = St(h);
    Seek(s, (u32)SDL_min((double)milliseconds * s->mRate / 1000.0, (double)s->mTotalFrames));
}

DXDEC void AILCALL AIL_stream_ms_position(HSTREAM h, S32 FAR* total_milliseconds, S32 FAR* current_milliseconds)
{
    LOCK;
    Stream* s = St(h);
    if (total_milliseconds)
    {
        *total_milliseconds = (S32)((long long)s->mTotalFrames * 1000 / s->mRate);
    }
    if (current_milliseconds)
    {
        *current_milliseconds = (S32)((long long)s->mPos * 1000 / s->mRate);
    }
}
