#!/usr/bin/env python3
# eyes_png.py <eyes_N.bin> [out prefix]: a GTA2_3DS_EYES dump as <prefix>_left.png, _right.png (400x240), _sbs.png (side
# by side) and _anaglyph.png (red left, cyan right). Needs Pillow.
import sys
from PIL import Image

data = open(sys.argv[1], 'rb').read()
prefix = sys.argv[2] if len(sys.argv) > 2 else sys.argv[1].rsplit('.', 1)[0]
size = 240 * 400 * 3
eyes = []
for i in range(2):
    # The screen's columns left to right, each 240 BGR8 pixels from its bottom.
    portrait = Image.frombytes('RGB', (240, 400), data[i * size:(i + 1) * size], 'raw', 'BGR')
    eyes.append(portrait.transpose(Image.Transpose.ROTATE_90))
eyes[0].save(prefix + '_left.png')
eyes[1].save(prefix + '_right.png')
sbs = Image.new('RGB', (808, 240), (255, 255, 255))
sbs.paste(eyes[0], (0, 0))
sbs.paste(eyes[1], (408, 0))
sbs.save(prefix + '_sbs.png')
Image.merge('RGB', (eyes[0].getchannel('R'), eyes[1].getchannel('G'), eyes[1].getchannel('B'))).save(prefix + '_anaglyph.png')
