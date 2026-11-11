# Pre-render helpers: height fields and materials in native HUD pixels, supersampled, lit from the top
# left like GTA2's HUD sprites, then box-downsampled and palette-reduced like a 90s pre-render.
import numpy as np
from PIL import Image, ImageFont

SS = 16  # supersamples per native HUD pixel

LIGHT = np.array([-1.0, -1.25, 1.35]); LIGHT /= np.linalg.norm(LIGHT)
HALF = LIGHT + np.array([0, 0, 1.0]); HALF /= np.linalg.norm(HALF)


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        ys, xs = np.mgrid[0:h*SS, 0:w*SS].astype(np.float32)
        self.x = (xs + 0.5) / SS
        self.y = (ys + 0.5) / SS
        self.h_ = np.zeros((h*SS, w*SS), np.float32)        # height, native px
        self.alb = np.zeros((h*SS, w*SS, 3), np.float32)     # albedo 0..1
        self.spec = np.zeros((h*SS, w*SS), np.float32)       # specular strength
        self.shin = np.full((h*SS, w*SS), 20.0, np.float32)  # specular exponent
        self.emit = np.zeros((h*SS, w*SS, 3), np.float32)    # self-lit (lamps)
        self.alpha = np.zeros((h*SS, w*SS), np.float32)

    def paint(self, mask, rgb=None, spec=None, shin=None, height=None, add=None, alpha=1.0):
        m = mask > 0
        if rgb is not None:
            self.alb[m] = np.array(rgb, np.float32) / 255.0
        if spec is not None:
            self.spec[m] = spec
        if shin is not None:
            self.shin[m] = shin
        if height is not None:
            self.h_[m] = (height[m] if np.ndim(height) else height)
        if add is not None:
            self.h_ += np.where(m, add, 0)
        self.alpha[m] = alpha


def hexrgb(s):
    s = s.lstrip('#'); return tuple(int(s[i:i+2], 16) for i in (0, 2, 4))


# Signed distances in native px (negative inside).
def sd_circle(c, cx, cy, r):
    return np.hypot(c.x - cx, c.y - cy) - r


def sd_rrect(c, x0, y0, x1, y1, r):
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hx, hy = (x1 - x0) / 2 - r, (y1 - y0) / 2 - r
    qx = np.abs(c.x - cx) - hx; qy = np.abs(c.y - cy) - hy
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r


def sd_chamfer(c, x0, y0, x1, y1, k):
    """Rectangle with 45 degree cut corners of size k."""
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hx, hy = (x1 - x0) / 2, (y1 - y0) / 2
    ax = np.abs(c.x - cx); ay = np.abs(c.y - cy)
    d1 = np.maximum(ax - hx, ay - hy)
    d2 = (ax + ay - (hx + hy - k)) / np.sqrt(2)
    return np.maximum(d1, d2)


def blur(a, r):
    """Box blur x3 (close to gaussian), radius in samples."""
    r = int(r)
    if r < 1:
        return a
    out = a.astype(np.float32)
    for _ in range(3):
        for ax in (0, 1):
            p = np.concatenate([np.repeat(np.take(out, [0], ax), r + 1, ax), out,
                                np.repeat(np.take(out, [-1], ax), r, ax)], ax)
            cs = np.cumsum(p, ax)
            n = out.shape[ax]
            out = (np.take(cs, range(2*r + 1, 2*r + 1 + n), ax) - np.take(cs, range(0, n), ax)) / (2*r + 1)
    return out


def profile_round(d, width, height):
    """Half-round bead: d = distance inside the edge (px), 0..width."""
    t = np.clip(d / width, 0, 1)
    return height * np.sqrt(np.clip(1 - (2*t - 1)**2, 0, 1))


def profile_bevel(d, width, height):
    """Flat top with a rounded 45-ish bevel of the given width."""
    t = np.clip(d / width, 0, 1)
    return height * np.sin(t * np.pi / 2)


def shade(c, ambient=0.20, diffuse=1.05, ao_r=2.5, ao_k=0.55, light_rgb=(1.0, 0.97, 0.90)):
    h = c.h_
    gy, gx = np.gradient(blur(h, SS // 8), 1.0 / SS)
    n = np.stack([-gx, -gy, np.ones_like(h)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    ndl = np.clip(n @ LIGHT, 0, 1)
    ndh = np.clip(n @ HALF, 0, 1)
    # ambient occlusion from the local height deficit
    ao = 1 - ao_k * np.clip((blur(h, ao_r * SS) - h) / 2.0, 0, 1)
    lit = c.alb * (ambient + diffuse * ndl)[..., None] * ao[..., None]
    lit += (c.spec * ndh ** c.shin)[..., None] * np.array(light_rgb)
    lit += c.emit
    return np.clip(lit, 0, 1), ao


def downsample(rgb, alpha, scale, colours=None, hard_alpha=True):
    """rgb/alpha at SS; returns an RGBA PIL image at `scale` x native size."""
    H, W = alpha.shape
    w = int(round(W / SS * scale)); h = int(round(H / SS * scale))
    pre = np.dstack([rgb * alpha[..., None], alpha])
    chans = [np.asarray(Image.fromarray(pre[..., i].astype(np.float32), 'F').resize((w, h), Image.BOX)) for i in range(4)]
    a = chans[3]
    col = np.dstack(chans[:3]) / np.maximum(a, 1e-4)[..., None]
    if hard_alpha:
        a = (a > 0.5).astype(np.float32)
    out = np.dstack([np.clip(col, 0, 1) * 255, a * 255]).astype(np.uint8)
    im = Image.fromarray(out, 'RGBA')
    if colours:
        im = quantize(im, colours)
    return im


def quantize(im, colours):
    a = np.asarray(im)[..., 3]
    q = im.convert('RGB').quantize(colours, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE, kmeans=3).convert('RGB')
    out = np.dstack([np.asarray(q), a])
    return Image.fromarray(out.astype(np.uint8), 'RGBA')


def glyph_mask(c, glyph, x0, y0, px=1.0):
    """Pixel glyph (list of strings, '#' = on) placed at native (x0, y0), each cell px native pixels."""
    m = np.zeros_like(c.x)
    for j, row in enumerate(glyph):
        for i, ch in enumerate(row):
            if ch == '#':
                m[(c.x >= x0 + i*px) & (c.x < x0 + (i+1)*px) & (c.y >= y0 + j*px) & (c.y < y0 + (j+1)*px)] = 1
    return m


GLYPHS = {
    'N': ["#...#", "##..#", "#.#.#", "#..##", "#...#"],
    'E': ["####", "#...", "###.", "#...", "####"],
    'S': [".###", "#...", ".##.", "...#", "###."],
    'W': ["#...#", "#...#", "#.#.#", "##.##", "#...#"],
}


def sd_poly(c, pts):
    """Signed distance to a closed polygon (list of native (x, y)), negative inside."""
    d = np.full(c.x.shape, 1e9, np.float32); inside = np.zeros(c.x.shape, bool)
    n = len(pts)
    for i in range(n):
        (ax, ay), (bx, by) = pts[i], pts[(i + 1) % n]
        ex, ey = bx - ax, by - ay
        px, py = c.x - ax, c.y - ay
        t = np.clip((px * ex + py * ey) / (ex * ex + ey * ey), 0, 1)
        d = np.minimum(d, np.hypot(px - t * ex, py - t * ey))
        cond = (ay > c.y) != (by > c.y)
        xi = ax + (c.y - ay) * ex / (ey if ey else 1e-9)
        inside ^= cond & (c.x < xi)
    return np.where(inside, -d, d)


def font(bold, size):
    for p in ('/usr/share/fonts/TTF/DejaVuSans%s.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans%s.ttf'):
        try:
            return ImageFont.truetype(p % ('-Bold' if bold else ''), size)
        except OSError:
            pass
    return ImageFont.load_default()


# Grit, to match the HUD sprites: a 3D render crushed to few colours has per-pixel grain and sharp glints.
def value_noise(c, cell, seed):
    """Smooth noise in -1..1 with features about `cell` native px wide."""
    rng = np.random.default_rng(seed)
    gh, gw = int(c.h / cell) + 3, int(c.w / cell) + 3
    g = rng.random((gh, gw)).astype(np.float32) * 2 - 1
    fy, fx = c.y / cell, c.x / cell
    y0, x0 = fy.astype(int), fx.astype(int)
    ty, tx = fy - y0, fx - x0
    ty, tx = ty * ty * (3 - 2 * ty), tx * tx * (3 - 2 * tx)
    top = g[y0, x0] * (1 - tx) + g[y0, x0 + 1] * tx
    bot = g[y0 + 1, x0] * (1 - tx) + g[y0 + 1, x0 + 1] * tx
    return top * (1 - ty) + bot * ty


def pixel_noise(c, seed):
    """One random value in -1..1 per native pixel."""
    g = np.random.default_rng(seed).random((c.h, c.w)).astype(np.float32) * 2 - 1
    return g[np.minimum(c.y.astype(int), c.h - 1), np.minimum(c.x.astype(int), c.w - 1)]


def scratch_mask(c, mask, n, seed, length=(2.0, 6.0), width=0.4):
    """Short straight scratches starting inside `mask`, mostly along the diagonals."""
    rng = np.random.default_rng(seed)
    ys, xs = np.nonzero(mask[::SS, ::SS])
    out = np.zeros(c.x.shape, bool)
    for _ in range(n if len(xs) else 0):
        i = rng.integers(len(xs))
        ax, ay = xs[i] + rng.random(), ys[i] + rng.random()
        a = rng.choice([0.75, 2.35, 0.2, 1.4]) + rng.normal(0, 0.25)
        ln = rng.uniform(*length)
        ex, ey = np.cos(a) * ln, np.sin(a) * ln
        px, py = c.x - ax, c.y - ay
        t = np.clip((px * ex + py * ey) / (ln * ln), 0, 1)
        out |= np.hypot(px - t * ex, py - t * ey) < width
    return out & mask


def grit(c, mask, seed, grain=0.09, bump=0.14, wear=0.0, wear_rgb=(200, 150, 96), scratches=0, mottle=0.08):
    """Grain, mottling and a fine bump (glints) on the painted region `mask`, optionally worn edges and scratches."""
    m = mask > 0
    convex = np.clip((c.h_ - blur(c.h_, int(0.5 * SS))) * 2.5, 0, 1)
    k = 1 + grain * pixel_noise(c, seed) + mottle * value_noise(c, 3.0, seed + 1)
    c.alb[m] *= np.clip(k, 0.3, 1.8)[m][:, None]
    c.h_ += np.where(m, bump * (0.6 * value_noise(c, 1.2, seed + 2) + 0.4 * pixel_noise(c, seed + 3)), 0)
    worn = m & (convex > 0.5) & (value_noise(c, 1.5, seed + 4) > 1 - 2 * wear) if wear else np.zeros_like(m)
    c.alb[worn] = c.alb[worn] * 0.4 + np.array(wear_rgb, np.float32) / 255 * 0.6
    c.spec[worn] = np.maximum(c.spec[worn], 1.1); c.shin[worn] = 45
    if scratches:
        s = scratch_mask(c, m, scratches, seed + 5, length=(1.5, 4.0), width=0.35)
        light = s & (pixel_noise(c, seed + 6) > 0.2)
        c.alb[light] = np.array(wear_rgb, np.float32) / 255 * 0.8
        c.spec[light] = 1.2
        c.alb[s & ~light] *= 0.45


def shade_hard(c, ambient=0.06, diffuse=1.3, ao_r=1.8, ao_k=0.9, light_rgb=(1.0, 0.96, 0.86), contrast=1.3):
    """shade() with a harder key light, deep crevices and a contrast curve, for the gritty frames."""
    rgb, ao = shade(c, ambient, diffuse, ao_r, ao_k, light_rgb)
    return np.clip((rgb - 0.3) * contrast + 0.3, 0, 1)
