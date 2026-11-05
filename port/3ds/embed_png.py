#!/usr/bin/env python3
# embed_png.py <out.h> <name>=<file.png> ...: 8-bit PNGs (RGBA, RGB, grey) as C arrays of RGBA texels
# (r | g << 8 | b << 16 | a << 24), with the standard library only.
import struct
import sys
import zlib


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def read_png(path):
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        sys.exit('%s: not a PNG' % path)
    pos, idat = 8, b''
    while pos < len(data):
        length, kind = struct.unpack_from('>I4s', data, pos)
        body = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            width, height, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
        pos += 12 + length
    channels = {0: 1, 2: 3, 6: 4}.get(colour)
    if depth != 8 or interlace or not channels:
        sys.exit('%s: only 8-bit, non-interlaced grey, RGB or RGBA' % path)
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, prev = [], bytearray(stride)
    for y in range(height):
        filt = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = row[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            row[i] = (row[i] + (0, a, b, (a + b) // 2, paeth(a, b, c))[filt]) & 0xFF
        rows.append(row)
        prev = row
    texels = []
    for row in rows:
        for x in range(width):
            p = row[x * channels:(x + 1) * channels]
            r, g, b, a = (p[0], p[0], p[0], 255) if channels == 1 else (p[0], p[1], p[2], 255 if channels == 3 else p[3])
            texels.append(r | g << 8 | b << 16 | a << 24)
    return width, height, texels


out = ['// Made by embed_png.py from the PNGs below.', '#pragma once', '']
for arg in sys.argv[2:]:
    name, path = arg.split('=', 1)
    w, h, texels = read_png(path)
    out.append('// %s' % path.replace('\\', '/').split('/port/')[-1])
    out.append('const int k%sW = %d, k%sH = %d;' % (name, w, name, h))
    out.append('const unsigned int k%s[%d] = {' % (name, w * h))
    for i in range(0, len(texels), 8):
        out.append('    ' + ', '.join('0x%08X' % t for t in texels[i:i + 8]) + ',')
    out.append('};')
    out.append('')
open(sys.argv[1], 'w').write('\n'.join(out))
