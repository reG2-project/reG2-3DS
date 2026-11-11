# Renders the device frames (designs.BUILDERS3, or those named) into assets/frames/<name>, with their 9-slice and
# status lamp in nine.json, and device_frames.png, a preview of them all. No game data needed.
# usage: python3 -I make_frames.py <assets/frames dir> [I J ...]
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from PIL import Image
import designs
from mm_lib import downsample, quantize
from make_all import soft, rgba_from

COLOURS = 48


def nine_slice(frame, n, w, h, tab=True):
    """The frame drawn at w x h as port/3ds/minimap.cpp's DrawFrame draws it."""
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    W, H = frame.size
    cap, rx, rib = n['cap'], n['rib_x'], n['rib']

    def put(x, y, sx, sy, cw, ch):
        out.alpha_composite(frame.crop((sx, sy, sx + cw, sy + ch)), (x, y))

    def run(a, b, at, across, src):
        p = a
        while p < b:
            ln = min(rib, b - p)
            put(*((p, at, rx, src, ln, cap) if across else (at, p, src, rx, cap, ln)))
            p += rib

    def edge(y, src, tx, tw):
        x = (w - tw) // 2
        if tab and tw:
            run(cap, x, y, True, src); run(x + tw, w - cap, y, True, src)
            put(x, y, tx, src, tw, cap)
        else:
            run(cap, w - cap, y, True, src)

    edge(0, 0, n['tab_x'], n['tab_w'])
    edge(h - cap, H - cap, n['btab_x'], n['btab_w'])
    run(cap, h - cap, 0, False, 0)
    run(cap, h - cap, w - cap, False, W - cap)
    for x, y, sx, sy in ((0, 0, 0, 0), (w - cap, 0, W - cap, 0), (0, h - cap, 0, H - cap), (w - cap, h - cap, W - cap, H - cap)):
        put(x, y, sx, sy, cap, cap)
    return out


def preview(frames):
    """Each frame x3, then 9-sliced at the game's sizes x2, on the bottom screen's dark brown."""
    rows = []
    for fr, n in frames:
        parts = [fr.resize((fr.width * 3, fr.height * 3), Image.NEAREST)]
        for w, h, tab in ((110, 110, True), (316, 110, True), (40, 40, False)):
            parts.append(nine_slice(fr, n, w, h, tab).resize((w * 2, h * 2), Image.NEAREST))
        row = Image.new('RGBA', (sum(p.width for p in parts) + 12 * (len(parts) + 1), max(p.height for p in parts) + 24),
                        (46, 40, 34, 255))
        x = 12
        for p in parts:
            row.alpha_composite(p, (x, 12)); x += p.width + 12
        rows.append(row)
    out = Image.new('RGBA', (max(r.width for r in rows), sum(r.height for r in rows)), (46, 40, 34, 255))
    y = 0
    for r in rows:
        out.alpha_composite(r, (0, y)); y += r.height
    return out


def main(out_dir, names):
    frames = []
    for build in designs.BUILDERS3:
        d = build()
        if names and d.name.split('_')[0] not in names:
            continue
        out = os.path.join(out_dir, d.name); os.makedirs(out, exist_ok=True)
        anchor, lx0, ly0, lx1, ly1 = d.lamp_box
        for s in (1, 2):
            # The edges rebuilt from their strips, as the game draws them, then crushed to few colours like the HUD.
            ns = {k: v * s for k, v in d.nine.items()}
            frame = nine_slice(downsample(d.rgb, d.canvas.alpha, s), ns, d.W * s, d.H * s)
            quantize(frame, COLOURS).save(os.path.join(out, 'frame_%dx.png' % s))
            lamp = downsample(d.lamp_rgb, d.lamp_alpha, s).crop((lx0 * s, ly0 * s, lx1 * s, ly1 * s))
            quantize(lamp, 16).save(os.path.join(out, 'lamp_%dx.png' % s))
            grgb, ga = d.glass
            rgba_from(np.dstack([soft(grgb[..., i], s) for i in range(3)]), soft(ga, s)).save(os.path.join(out, 'glass_%dx.png' % s))
            m = soft((d.screen_sd < 0).astype(np.float32), s)
            Image.fromarray((m * 255).astype(np.uint8), 'L').save(os.path.join(out, 'screen_mask_%dx.png' % s))
        d.nine['lamp'] = dict(anchor=anchor, x=lx0, y=ly0, w=lx1 - lx0, h=ly1 - ly0)
        with open(os.path.join(out, 'nine.json'), 'w') as f:
            json.dump(d.nine, f, indent=1)
            f.write('\n')
        frames.append((Image.open(os.path.join(out, 'frame_1x.png')).convert('RGBA'), d.nine))
        print(d.name, d.nine)
    if not names:
        preview(frames).save(os.path.join(out_dir, 'device_frames.png'))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2:])
