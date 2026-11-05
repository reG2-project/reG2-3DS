#include "hud_theme.hpp"
#include "../renderer/renderer.hpp"

namespace HudTheme
{
namespace
{
u32 Rgba(u32 r, u32 g, u32 b, u32 a = 255)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

Quad MakeQuad(f32 x0, f32 y0, f32 x1, f32 y1, f32 u0, f32 v0, f32 u1, f32 v1, u32 rgba)
{
    const Quad q = {x0, y0, x1, y1, u0, v0, u1, v1, {rgba, rgba, rgba, rgba}};
    return q;
}

// Plain alpha blended rectangles, drawn together.
class Shapes
{
  public:
    explicit Shapes(Sink& sink) : mSink(sink), mCount(0) {}
    void Fill(s32 x0, s32 y0, s32 x1, s32 y1, u32 rgba)
    {
        if (x0 < x1 && y0 < y1)
        {
            mQuads[mCount++] = MakeQuad((f32)x0, (f32)y0, (f32)x1, (f32)y1, 0.0f, 0.0f, 0.0f, 0.0f, rgba);
        }
    }
    // Recessed: dark top and left edges, light bottom and right ones, corner pixels left out.
    void Panel(const Rect& r)
    {
        const u32 fill = Rgba(0, 0, 0, 150), shade = Rgba(0, 0, 0, 220), light = Rgba(255, 255, 255, 40);
        Fill(r.x0 + 1, r.y0 + 1, r.x1 - 1, r.y1 - 1, fill);
        Fill(r.x0 + 1, r.y0, r.x1 - 1, r.y0 + 1, shade);
        Fill(r.x0, r.y0 + 1, r.x0 + 1, r.y1 - 1, shade);
        Fill(r.x0 + 1, r.y1 - 1, r.x1 - 1, r.y1, light);
        Fill(r.x1 - 1, r.y0 + 1, r.x1, r.y1 - 1, light);
    }
    void Finish()
    {
        mSink.DrawQuads(mQuads, mCount, 0, RS_NO_TEXTURE | RS_BLEND_ALPHA);
    }

  private:
    Sink& mSink;
    Quad mQuads[5 * kPartCount];
    s32 mCount;
};
} // namespace

void Draw(IRenderer&, Sink& sink, const Rect* pPanels, u32 parts)
{
    Shapes shapes(sink);
    for (s32 i = 0; i < kPartCount; i++)
    {
        if (parts & (1u << i))
        {
            shapes.Panel(pPanels[i]);
        }
    }
    shapes.Finish();
}
} // namespace HudTheme
