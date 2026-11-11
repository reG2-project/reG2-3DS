# Side by side bottom screens of Azahar captures (test_headless.sh shots, the bottom screen at BOTTOM), a column
# per frame and a row per capture, at 1x and 2x nearest. The sheets hold game graphics: keep them out of the repo.
# usage: python3 -I compare_frames.py <out prefix> <row label>=<shot path with {}> ... -- <column label>=<text for {}> ...
#   each cell crops the shot path with {} replaced by its column text; a row label splits into lines at '|'.
#   <out prefix>_zoom.png: per column, the respect meters and the minimap of the first row's shot at 3x.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image, ImageDraw
from mm_lib import font

BOTTOM = (106, 262, 426, 502)
# In bottom screen pixels: the respect meters' panel and the minimap's slot with a margin.
METERS = (0, 0, 104, 82)
MINIMAP = (204, 124, 320, 240)
GAP, HEAD, SIDE = 6, 22, 150


def sheet(rows, cols, z):
    f = font(True, 13 * z); fs = font(False, 11 * z)
    cw, ch = 320 * z, 240 * z
    out = Image.new('RGB', (SIDE * z + len(cols) * (cw + GAP * z), HEAD * z + len(rows) * (ch + GAP * z)), (24, 24, 26))
    d = ImageDraw.Draw(out)
    for i, (label, _) in enumerate(cols):
        d.text((SIDE * z + i * (cw + GAP * z) + 4 * z, 4 * z), label, font=f, fill=(235, 220, 160))
    for j, (label, shot) in enumerate(rows):
        y = HEAD * z + j * (ch + GAP * z)
        for k, line in enumerate(label.split('|')):
            d.text((6 * z, y + 4 * z + k * 15 * z), line, font=f if k == 0 else fs, fill=(200, 200, 200))
        for i, (_, prefix) in enumerate(cols):
            im = Image.open(shot.replace('{}', prefix)).convert('RGB').crop(BOTTOM)
            out.paste(im.resize((cw, ch), Image.NEAREST), (SIDE * z + i * (cw + GAP * z), y))
    return out


def zoom_sheet(shot, cols, z=3):
    f = font(True, 15)
    cells = []
    for label, prefix in cols:
        im = Image.open(shot.replace('{}', prefix)).convert('RGB').crop(BOTTOM)
        parts = [im.crop(b) for b in (METERS, MINIMAP)]
        w = sum(p.width * z for p in parts) + GAP * z
        cell = Image.new('RGB', (w, HEAD + max(p.height for p in parts) * z), (24, 24, 26))
        ImageDraw.Draw(cell).text((4, 3), label, font=f, fill=(235, 220, 160))
        x = 0
        for p in parts:
            cell.paste(p.resize((p.width * z, p.height * z), Image.NEAREST), (x, HEAD)); x += p.width * z + GAP * z
        cells.append(cell)
    cw, ch = max(c.width for c in cells), max(c.height for c in cells)
    out = Image.new('RGB', (2 * cw + GAP * z, ((len(cells) + 1) // 2) * (ch + GAP * z)), (24, 24, 26))
    for i, c in enumerate(cells):
        out.paste(c, ((i % 2) * (cw + GAP * z), (i // 2) * (ch + GAP * z)))
    return out


def main(args):
    prefix = args[0]
    sep = args.index('--')
    rows = [a.split('=', 1) for a in args[1:sep]]
    cols = [a.split('=', 1) for a in args[sep + 1:]]
    for z in (1, 2):
        sheet(rows, cols, z).save('%s_%dx.png' % (prefix, z))
    zoom_sheet(rows[0][1], cols).save('%s_zoom.png' % prefix)


if __name__ == '__main__':
    main(sys.argv[1:])
