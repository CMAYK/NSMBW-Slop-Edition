#!/usr/bin/env python3
"""Convert PPM frame dumps to PNG (no dependencies)."""
import sys, zlib, struct
def ppm2png(src, dst):
    d = open(src, 'rb').read()
    parts = d.split(b'\n', 3); w, h = map(int, parts[1].split()); px = parts[3]
    raw = b''.join(b'\0' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xFFFFFFFF)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(dst, 'wb').write(png)
for s in sys.argv[1:]: ppm2png(s, s[:-4] + '.png')
