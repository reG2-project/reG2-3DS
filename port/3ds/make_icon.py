#!/usr/bin/env python3
# make_icon.py <out.png>: the 48x48 Homebrew Launcher icon, drawn from plain shapes (no game data).
import struct
import sys
import zlib

W = H = 48
px = [[(32, 38, 46)] * W for _ in range(H)]


def rect(x0, y0, x1, y1, c):
    for y in range(max(y0, 0), min(y1, H)):
        for x in range(max(x0, 0), min(x1, W)):
            px[y][x] = c


# A road seen from above, with a car on it.
rect(0, 3, W, 27, (70, 72, 76))
rect(0, 3, W, 5, (150, 150, 150))
rect(0, 25, W, 27, (150, 150, 150))
for x in range(1, W, 8):
    rect(x, 14, x + 4, 16, (230, 200, 40))
rect(14, 9, 34, 21, (20, 20, 20))
rect(15, 10, 33, 20, (200, 30, 30))
rect(27, 11, 31, 19, (90, 140, 170))
rect(17, 11, 19, 19, (90, 140, 170))
rect(32, 10, 33, 12, (255, 240, 160))
rect(32, 18, 33, 20, (255, 240, 160))

# "GTA2" in a 3x5 pixel font at 3x, outlined.
FONT = {
    "G": ["111", "100", "101", "101", "111"],
    "T": ["111", "010", "010", "010", "010"],
    "A": ["010", "101", "111", "101", "101"],
    "2": ["111", "001", "111", "100", "111"],
}
S = 3
text = "GTA2"
x0 = (W - (len(text) * 3 * S + (len(text) - 1) * 2)) // 2
y0 = 30
for pass_, colour in ((0, (0, 0, 0)), (1, None)):
    for i, ch in enumerate(text):
        cx = x0 + i * (3 * S + 2)
        fill = (230, 60, 50) if ch == "2" else (245, 205, 40)
        for gy, row in enumerate(FONT[ch]):
            for gx, bit in enumerate(row):
                if bit != "1":
                    continue
                x, y = cx + gx * S, y0 + gy * S
                if pass_ == 0:
                    rect(x - 1, y - 1, x + S + 1, y + S + 1, colour)
                else:
                    rect(x, y, x + S, y + S, fill)

raw = b"".join(b"\0" + bytes(c for p in row for c in p) for row in px)


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


with open(sys.argv[1], "wb") as f:
    f.write(b"\x89PNG\r\n\x1a\n")
    f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)))
    f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
    f.write(chunk(b"IEND", b""))
