#include "fps_overlay.hpp"
#include "stats_3ds.hpp"
#include "../renderer/renderer.hpp"
#include "../src/frame_loop.hpp"
#include "../src/settings.hpp"
#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace FpsOverlay
{
namespace
{
// Refreshed 4 times per second, with rates over the last second.
const u64 kRefreshTicks = SYSCLOCK_ARM11 / 4;
const s32 kSamples = 5;
const s32 kLines = 2;
const s32 kMaxChars = 32;
const s32 kCharW = 6;
const s32 kLineH = 9;
const s32 kMargin = 2;

// 5x7 glyphs for ' ' to 'Z', a byte per column, bit 0 at the top.
const u8 kFont[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7F, 0x14, 0x7F, 0x14},
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62}, {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00}, {0x00, 0x56, 0x36, 0x00, 0x00},
    {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06},
    {0x32, 0x49, 0x79, 0x41, 0x3E}, {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01}, {0x3E, 0x41, 0x49, 0x49, 0x7A},
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
};
static_assert(sizeof(kFont) / sizeof(kFont[0]) == 'Z' - ' ' + 1, "a glyph per character");

bool sInit;
bool sForced;
bool sOn;
RTexture* spTex;
s32 sW, sH;
// Counts at each refresh, the newest at sNewest.
struct Sample
{
    u64 tick;
    u32 frames;
    u32 steps;
    u64 wait;
};
Sample sSamples[kSamples];
s32 sCount;
s32 sNewest;
u32 sFrames;
std::vector<u32> sPixels;

void Init()
{
    sInit = true;
    const char* pOn = getenv("GTA2_3DS_FPS");
    sForced = pOn && atoi(pOn) != 0;
}

// The text as an image: white glyphs with a dark shadow on a translucent black box. Bytes r, g, b, a.
void Render(const char lines[kLines][kMaxChars + 1])
{
    s32 chars = 0;
    for (s32 l = 0; l < kLines; l++)
    {
        const s32 n = (s32)strlen(lines[l]);
        chars = n > chars ? n : chars;
    }
    sW = chars * kCharW + 2 * kMargin - 1;
    sH = kLines * kLineH + 2 * kMargin - 2;
    sPixels.assign((size_t)(sW * sH), 0xA0000000u);
    for (s32 l = 0; l < kLines; l++)
    {
        for (s32 c = 0; lines[l][c]; c++)
        {
            char ch = lines[l][c];
            ch = ch >= 'a' && ch <= 'z' ? (char)(ch - 32) : ch;
            if (ch < ' ' || ch > 'Z')
            {
                continue;
            }
            const u8* pGlyph = kFont[ch - ' '];
            for (s32 x = 0; x < 5; x++)
            {
                for (s32 y = 0; y < 7; y++)
                {
                    if (!(pGlyph[x] >> y & 1))
                    {
                        continue;
                    }
                    const s32 px = kMargin + c * kCharW + x;
                    const s32 py = kMargin + l * kLineH + y;
                    sPixels[(size_t)(py * sW + px)] = 0xFFFFFFFFu;
                    u32& shadow = sPixels[(size_t)((py + 1) * sW + px + 1)];
                    shadow = shadow == 0xFFFFFFFFu ? shadow : 0xFF000000u;
                }
            }
        }
    }
}
} // namespace

RTexture* Update(IRenderer& renderer, s32* pW, s32* pH)
{
    if (!sInit)
    {
        Init();
    }
    const bool on = sForced || PortSettings::ShowFps();
    if (on != sOn)
    {
        sOn = on;
        sCount = 0;
        Stats3DS_CountSleeps(on);
    }
    if (!sOn)
    {
        return 0;
    }
    sFrames++;
    const u64 now = svcGetSystemTick();
    if (!sCount || now - sSamples[sNewest].tick >= kRefreshTicks)
    {
        sNewest = (sNewest + 1) % kSamples;
        const Sample cur = {now, sFrames, PortFrameLoop::StepCount(), Stats3DS_WaitTicks()};
        sSamples[sNewest] = cur;
        sCount = sCount < kSamples ? sCount + 1 : kSamples;
        const Sample& old = sSamples[(sNewest + kSamples - (sCount - 1)) % kSamples];
        char lines[kLines][kMaxChars + 1];
        if (sCount > 1)
        {
            const u64 ticks = cur.tick - old.tick;
            const u64 waited = cur.wait - old.wait;
            const u32 frames = cur.frames - old.frames;
            const double seconds = (double)ticks / SYSCLOCK_ARM11;
            const u64 busy = ticks > waited ? ticks - waited : 0;
            snprintf(lines[0], sizeof(lines[0]), "FPS %.1f  LPS %.1f  CPU %.1fMS", frames / seconds, (cur.steps - old.steps) / seconds,
                     busy * 1000.0 / SYSCLOCK_ARM11 / frames);
        }
        else
        {
            snprintf(lines[0], sizeof(lines[0]), "FPS -  LPS -  CPU -");
        }
        const struct mallinfo mi = mallinfo();
        const u32 linear = envGetLinearHeapSize() - linearSpaceFree();
        snprintf(lines[1], sizeof(lines[1]), "HEAP %.1fM  LINEAR %.1fM", mi.uordblks / 1048576.0, linear / 1048576.0);
        Render(lines);
        if (!spTex)
        {
            spTex = renderer.CreateTexture();
        }
        renderer.UploadTexture(spTex, sW, sH, &sPixels[0], RMIP_NONE);
    }
    *pW = sW;
    *pH = sH;
    return spTex;
}

void Shutdown(IRenderer& renderer)
{
    if (spTex)
    {
        renderer.FreeTexture(spTex);
        spTex = 0;
    }
}
} // namespace FpsOverlay
