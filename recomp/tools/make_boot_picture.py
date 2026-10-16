"""make the boot screen's keyboard picture: ui/graphics/boot/keyboard_sideways.png

    python tools/make_boot_picture.py [--size 0.7225] [--sign 1.2] [--sign-line 0.05] [--lines 5] [--keys 3d|flat]

From the drawing tools/art/keyboard_sideways_source.png (two hands holding a keyboard sideways, on
black), in the look of the console's own "Hold the Wii Remote sideways" picture
(docs/BOOT_SCREEN.md):
  1. the keys are drawn again, all alike (a 75% keyboard: 6 rows of 15 units, turned so the space
     bar is on the right), onto the drawing's keyboard face; the hands stay on top. --keys 3d
     (default): mechanical keycaps - darker sides, a lighter top set in; --keys flat: one outline
  2. the console's colours: skin (255, 231, 189), lines (56, 47, 40); every line grown (--lines)
  3. the black background becomes white
  4. a window of it fills the box's inside (292 x 172 layout units): the hands as big as the
     console's (palm with thumb 59 units wide, x --size), their fingertips 12% down the box, the
     arms off its bottom; the top of the keyboard is cut off
  5. the red "no" sign, centred: radius 0.455 x 175 x --sign units, line --sign-line x 175. The
     picture is 200 units tall (14 above and below the box, transparent but for the sign), so
     the sign goes over the box's frame.
2 pixels per layout unit: 584 x 400. Needs: pip install pillow numpy opencv-python
"""
import argparse, math, os
import cv2, numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap = argparse.ArgumentParser()
ap.add_argument('--size', type=float, default=0.7225, help='the drawing, against hands as big as the console\'s')
ap.add_argument('--sign', type=float, default=1.2, help='the sign\'s size')
ap.add_argument('--sign-line', type=float, default=0.05, help='its line, against the box height')
ap.add_argument('--lines', type=int, default=5, help='how much every line grows (pixels of the source)')
ap.add_argument('--keys', choices=['3d', 'flat'], default='3d', help='keycaps: 3d (mechanical: sides and a set-in top) or flat')
ap.add_argument('--out', default=os.path.join(ROOT, 'ui', 'graphics', 'boot', 'keyboard_sideways.png'))
a = ap.parse_args()

src = cv2.imread(os.path.join(ROOT, 'tools', 'art', 'keyboard_sideways_source.png'))
H, W = src.shape[:2]

# 1. the keys
U, S, MX, MY = 64, 4, 0.72, 0.36                       # units of a key; margins (long side, short side)
FW, FH = int((15 + 2 * MX) * U), int((6 + 2 * MY) * U)
face = Image.new('RGBA', (FW * S, FH * S), (0, 0, 0, 0)); d = ImageDraw.Draw(face)
d.rounded_rectangle((0, 0, FW * S - 1, FH * S - 1), radius=int(0.3 * U * S), fill=(250, 250, 250, 255))
rows = [[1] * 15, [1] * 13 + [2], [1.5] + [1] * 12 + [1.5], [1.75] + [1] * 11 + [2.25],
        [2.25] + [1] * 10 + [1.75, 1], [1.25, 1.25, 1.25, 6.25, 1, 1, 1, 1, 1]]
gap = 0.2
for r, keys in enumerate(rows):
    x = MX
    for w in keys:
        x0, y0, x1, y1 = (x + gap / 2) * U, (MY + r + gap / 2) * U, (x + w - gap / 2) * U, (MY + r + 1 - gap / 2) * U
        if a.keys == 'flat':
            d.rounded_rectangle((x0 * S, y0 * S, x1 * S, y1 * S), radius=8 * S, fill=(230, 230, 230, 255), outline=(24, 24, 24, 255), width=4 * S)
        else:
            # a mechanical keycap seen from above: its skirt (the sides, darker), and its top
            # face set in - more on the side that faces the viewer once the keyboard is turned
            # (upright left = the picture's bottom), a little on the others. The top's outline is
            # grey, so it stays thinner than the key's own when the lines are grown.
            d.rounded_rectangle((x0 * S, y0 * S, x1 * S, y1 * S), radius=9 * S, fill=(196, 198, 204, 255), outline=(24, 24, 24, 255), width=4 * S)
            front, back, side = 0.24 * U, 0.10 * U, 0.12 * U
            d.rounded_rectangle(((x0 + front) * S, (y0 + side) * S, (x1 - back) * S, (y1 - side) * S), radius=7 * S,
                                fill=(240, 240, 242, 255), outline=(96, 96, 100, 255), width=3 * S)
            # a soft light on the top face's far edge
            d.line(((x1 - back - 5) * S, (y0 + side + 6) * S, (x1 - back - 5) * S, (y1 - side - 6) * S), fill=(255, 255, 255, 255), width=3 * S)
        x += w
face = face.resize((FW, FH), Image.LANCZOS).rotate(90, expand=True)
fw, fh = face.size
quad = np.float32([[751, 39], [1133, 53], [1051, 1033], [678, 992]])   # the drawing's keyboard face (measured)
M = cv2.getPerspectiveTransform(np.float32([[0, 0], [fw, 0], [fw, fh], [0, fh]]), quad)
warped = cv2.warpPerspective(np.asarray(face).astype(np.float32), M, (W, H), flags=cv2.INTER_LINEAR, borderValue=(0, 0, 0, 0))
px = src.astype(int); b, g, r_ = px[..., 0], px[..., 1], px[..., 2]
skin = ((r_ > 170) & (r_ - b > 35) & (g > 110)).astype(np.uint8)
skin = cv2.morphologyEx(skin, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8))
near = cv2.dilate(skin, np.ones((7, 7), np.uint8)).astype(bool)
hand = cv2.GaussianBlur((skin.astype(bool) | (near & (px.max(axis=2) < 110))).astype(np.float32), (3, 3), 0)[..., None]
keep = np.clip(warped[..., 3:4] / 255.0 * (1 - hand), 0, 1)
img = src.astype(np.float32) * (1 - keep) + warped[..., [2, 1, 0]] * keep
img = img[..., ::-1]                                    # (RGB from here on)

# 2-3. the console's look, white background
SKIN = np.float32([255, 231, 189]); LINE = np.float32([56, 47, 40])
lum = img.max(axis=2)
grown = cv2.dilate((lum > 90).astype(np.uint8), cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (9, 9)))
bg = cv2.GaussianBlur((1 - grown).astype(np.float32), (3, 3), 0)[..., None]
img = img * (1 - bg) + 255 * bg
r_, g, b = img[..., 0], img[..., 1], img[..., 2]
sk = (r_ > 170) & (r_ - b > 35) & (g > 110)
ref = np.median(lum[sk]) if sk.any() else 255
img = np.where(sk[..., None], np.clip(SKIN * np.clip(lum / ref, 0.85, 1.08)[..., None], 0, 255), img)
dark = np.clip((170 - img.max(axis=2)) / 120.0, 0, 1)
dark = cv2.GaussianBlur(cv2.dilate(dark, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (a.lines, a.lines))), (3, 3), 0)[..., None]
img = Image.fromarray((img * (1 - dark) + LINE * dark).clip(0, 255).astype(np.uint8))

# 4. the window
PX, IW, IH, CH = 2, 292, 172, 200
s = 59.0 / 178.0 * a.size                             # layout units per source pixel
ww, wh = round(IW / s), round(IH / s)
hand_top, hands_cx = 646, 882                          # (the drawing's fingertips, its hands' middle)
y0 = round(hand_top - 0.12 * wh)
if y0 + wh > H: y0 = H - wh
x0 = round(hands_cx - ww / 2)
drawing = img.crop((x0, y0, x0 + ww, y0 + wh)).resize((IW * PX, IH * PX), Image.LANCZOS)
can = Image.new('RGBA', (IW * PX, CH * PX), (255, 255, 255, 0))
can.paste(drawing, (0, (CH - IH) * PX // 2))

# 5. the sign
Q = 4; ov = Image.new('RGBA', (IW * PX * Q, CH * PX * Q), (0, 0, 0, 0)); d = ImageDraw.Draw(ov)
cx, cy = IW * PX * Q / 2, CH * PX * Q / 2
rad, sw = 175 * 0.455 * a.sign * PX * Q, 175 * a.sign_line * PX * Q
d.ellipse((cx - rad, cy - rad, cx + rad, cy + rad), outline=(227, 0, 43, 245), width=round(sw))
k = (rad - sw / 2) / math.sqrt(2)
d.line((cx - k, cy - k, cx + k, cy + k), fill=(227, 0, 43, 245), width=round(sw))
can = Image.alpha_composite(can, ov.resize(can.size, Image.LANCZOS))
os.makedirs(os.path.dirname(a.out), exist_ok=True)
can.save(a.out)
print('wrote %s (%dx%d)' % (a.out, can.width, can.height))
