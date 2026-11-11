# Side by side bottom screens of Azahar captures (test_headless.sh shots, the bottom screen at BOTTOM), a column
# per frame and a row per capture, at 1x and 2x nearest. The sheets hold game graphics: keep them out of the repo.
# usage: python3 -I compare_frames.py <out prefix> <row label>=<shot path with {}> ... -- <column label>=<text for {}> ...
#   each cell crops the shot path with {} replaced by its column text; a row label splits into lines at '|'.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image, ImageDraw
from mm_lib import font

BOTTOM = (106, 262, 426, 502)
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


def main(args):
    prefix = args[0]
    sep = args.index('--')
    rows = [a.split('=', 1) for a in args[1:sep]]
    cols = [a.split('=', 1) for a in args[sep + 1:]]
    for z in (1, 2):
        sheet(rows, cols, z).save('%s_%dx.png' % (prefix, z))


if __name__ == '__main__':
    main(sys.argv[1:])
