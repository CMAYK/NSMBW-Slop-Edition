#!/usr/bin/env python3
"""Contact sheet of the most recent frame dumps: sheet.py OUT.ppm [N] [cols]
Reads frame_*.ppm from $RT_DUMP_DIR (default: recomp/frames)."""
import os, sys, glob
out = sys.argv[1]; N = int(sys.argv[2]) if len(sys.argv) > 2 else 60; cols = int(sys.argv[3]) if len(sys.argv) > 3 else 10
d = os.environ.get('RT_DUMP_DIR', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'frames'))
fs = sorted(glob.glob(os.path.join(d, 'frame_*.ppm')))[-N:]
if not fs: sys.exit('no frames in ' + d)
W, H = 160, 114; rows = (len(fs) + cols - 1) // cols
img = bytearray(W * cols * H * rows * 3)
for k, f in enumerate(fs):
    data = open(f, 'rb').read(); hdr = data.split(b'\n', 3); w, h = map(int, hdr[1].split()); px = hdr[3]
    ox = (k % cols) * W; oy = (k // cols) * H
    for y in range(H):
        sy = y * h // H; o = ((oy + y) * W * cols + ox) * 3
        for x in range(W):
            i = (sy * w + x * w // W) * 3; img[o:o + 3] = px[i:i + 3]; o += 3
open(out, 'wb').write(b'P6\n%d %d\n255\n' % (W * cols, H * rows) + img)
print(len(fs), 'frames:', os.path.basename(fs[0]), '..', os.path.basename(fs[-1]))
