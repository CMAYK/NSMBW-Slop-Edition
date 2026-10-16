#!/usr/bin/env python3
"""Placeholder character-select portraits for the characters the game has no picture for.

    python3 tools/make_char_portraits.py

Writes ui/characters/<id>/portrait.png (150x150, like the game's own Im_plofile*.tpl portraits:
the head, transparent around it) for every character after the first four (Mario, Luigi and
the two Toads have pictures in the game's layout). A placeholder is a white plate with the
character's name; paint over it (any size works, 150x150 matches the game's). A file that is
no longer a placeholder is never overwritten. Putting a portrait.png in the folder of one of the
first four characters replaces the game's picture for it too.

The list must match ROSTER in runtime/rt_chars.c.
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROSTER = [  # (id, name) in roster order
    ('mario', 'Mario'), ('luigi', 'Luigi'), ('ala_gold', 'Ala Gold'), ('buckenberry', 'Buckenberry'),
    ('toadette', 'Toadette'), ('toadsworth', 'Toadsworth'), ('daisy', 'Daisy'), ('rosalina', 'Rosalina'),
    ('goomba', 'Goomba'), ('retro_smb1', 'Retro Mario (SMB1)'), ('retro_smw', 'Retro Mario (SMW)'),
    ('retro_sm64', 'Retro Mario (SM64)'),
]
LAYOUT_HAS_PICTURE = {'mario', 'luigi', 'ala_gold', 'buckenberry'}
SIZE = 150
MARK = (250, 250, 250, 254)       # the plate colour: alpha 254 marks a placeholder


def font(size):
    for f in ('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 'C:/Windows/Fonts/arialbd.ttf',
              '/System/Library/Fonts/Supplemental/Arial Bold.ttf'):
        if os.path.exists(f): return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def is_placeholder(path):
    try: im = Image.open(path).convert('RGBA')
    except OSError: return True
    return im.size == (SIZE, SIZE) and im.getpixel((SIZE // 2, 12)) == MARK


def wrap(draw, text, f, width):
    lines, cur = [], ''
    for word in text.split():
        t = (cur + ' ' + word).strip()
        if draw.textlength(t, font=f) <= width or not cur: cur = t
        else: lines.append(cur); cur = word
    return lines + [cur]


def make(name):
    im = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((8, 8, SIZE - 9, SIZE - 9), radius=28, fill=MARK, outline=(60, 60, 70, 255), width=3)
    size = 22
    while size > 12 and max(d.textlength(w, font=font(size)) for w in name.split()) > SIZE - 30: size -= 1
    f = font(size)
    lines = wrap(d, name, f, SIZE - 30)
    h = len(lines) * 26
    for i, line in enumerate(lines):
        w = d.textlength(line, font=f)
        d.text(((SIZE - w) / 2, (SIZE - h) / 2 + i * 26 - 6), line, font=f, fill=(40, 40, 50, 255))
    small = font(11)
    t = 'portrait'
    d.text(((SIZE - d.textlength(t, font=small)) / 2, SIZE - 30), t, font=small, fill=(120, 120, 130, 255))
    return im


def main():
    made = 0
    for cid, name in ROSTER:
        if cid in LAYOUT_HAS_PICTURE: continue
        p = os.path.join(ROOT, 'ui', 'characters', cid, 'portrait.png')
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if os.path.exists(p) and not is_placeholder(p): continue
        make(name).save(p); made += 1
    print('ui/characters: %d placeholder portrait(s)' % made)


if __name__ == '__main__':
    main()
