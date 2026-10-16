#!/usr/bin/env python3
"""Sprite sheet templates for the retro characters (white boxes, one per animation frame).

    python tools/make_retro_templates.py [character]      (default: mario)

Writes ui/retro/<character>/:
  <form>.png          the sheet to paint over - one white box per frame, transparent around
                      them. Once anything is painted on it, it is never overwritten (your art
                      is safe); a sheet that is still only white boxes is rewritten when the
                      layout changes.
  frames.json         where each frame is in each sheet (x, y, w, h) - the game reads this,
                      so the layout can change without code changes. A painted sheet keeps its
                      entry, so frames can be moved on the art and their positions written here
                      by hand (16x16 / 16x32 box per frame).
  guide/<form>.png    the same layout 8x bigger with each frame's name (always regenerated)

Conventions (same as the 8-bit original): 1 pixel = 1 game unit (a block is 16x16), everything
faces right (left is mirrored), feet on the bottom edge of the box, centred horizontally.
Small forms use 16x16 boxes, big forms 16x32. Frames marked "optional" in the guide can be
left as white boxes / empty: the game falls back to the frame named in FALLBACK.
"""
import json, os, sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAP = 2                      # transparent pixels between boxes
COLS = 8

# frames every form has (name, description)
COMMON = [
    ('idle', 'standing still'),
    ('walk1', 'walk/run cycle 1'), ('walk2', 'walk/run cycle 2'), ('walk3', 'walk/run cycle 3'),
    ('skid', 'turning around at speed'),
    ('jump', 'in the air (going up)'),
    ('fall', 'in the air (going down) - optional, uses jump'),
    ('swim1', 'swim frame 1'), ('swim2', 'swim frame 1, feet kicking'),
    ('swim3', 'swim frame 2'), ('swim4', 'swim frame 2, feet kicking'),
    ('swim5', 'swim frame 3'), ('swim6', 'swim frame 3, feet kicking'),
    ('swim7', 'swim 7 - optional, not used yet'), ('swim8', 'swim 8 - optional, not used yet'),
    ('climb1', 'climbing vine/fence 1'), ('climb2', 'climbing vine/fence 2'),
    ('hang1', 'hanging from rope/bar 1 - optional, uses climb1'), ('hang2', 'hanging 2 - optional, uses climb2'),
    ('carry_idle', 'holding a shell/block - optional, uses idle'),
    ('carry_walk1', 'carrying, walk 1 - optional, uses walk1'), ('carry_walk2', 'carrying, walk 2 - optional, uses walk2'),
    ('carry_walk3', 'carrying, walk 3 - optional, uses walk3'),
    ('throw', 'throwing what was carried - optional, uses walk2'),
    ('kick', 'kicking a shell - optional, uses walk2'),
    ('flagpole1', 'sliding down the flagpole 1'), ('flagpole2', 'sliding down the flagpole 2'),
    ('back', 'facing away (going through a door) - optional, uses idle'),
    ('front', 'facing the screen (pipe going down) - optional, uses idle'),
    ('victory', 'goal pose - optional, uses idle'),
]
BIG_EXTRA = [('crouch', 'crouching (big forms only)')]
FORMS = [   # (sheet, box size, extra frames)
    ('small', (16, 16), [('death', 'losing a life (only the small sheet)')]),
    ('super', (16, 32), BIG_EXTRA + [('grow', 'between small and big while powering up/down (feet at the bottom)')]),
    ('fire', (16, 32), BIG_EXTRA + [('shoot', 'throwing a fireball')]),
    ('ice', (16, 32), BIG_EXTRA + [('shoot', 'throwing an ice ball')]),
    ('propeller', (16, 32), BIG_EXTRA + [('spin1', 'propeller spin, going up 1'), ('spin2', 'propeller spin 2'),
                                         ('spin3', 'propeller spin 3'), ('spin4', 'propeller spin 4'),
                                         ('float1', 'slow fall after the spin 1'), ('float2', 'slow fall 2'),
                                         ('drill', 'spinning dive down')]),
    ('penguin', (16, 32), BIG_EXTRA + [('slide', 'belly slide'), ('shoot', 'throwing an ice ball'),
                                       ('pswim1', 'penguin swim 1 - optional, uses swim1'), ('pswim2', 'penguin swim 2 - optional, uses swim2')]),
    ('mini', (16, 16), []),
]
FALLBACK = {'fall': 'jump', 'swim2': 'swim1', 'swim4': 'swim3', 'swim6': 'swim5', 'swim7': 'swim1', 'swim8': 'swim3', 'hang1': 'climb1', 'hang2': 'climb2', 'carry_idle': 'idle', 'carry_walk1': 'walk1',
            'carry_walk2': 'walk2', 'carry_walk3': 'walk3', 'throw': 'walk2', 'kick': 'walk2', 'back': 'idle',
            'front': 'idle', 'victory': 'idle', 'pswim1': 'swim1', 'pswim2': 'swim2'}


def font(size):
    for f in ['/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 'C:/Windows/Fonts/arial.ttf', 'DejaVuSans.ttf']:
        try: return ImageFont.truetype(f, size)
        except OSError: pass
    return ImageFont.load_default()


def is_template(path):
    """True while a sheet is still only white boxes on transparency (nothing painted yet)."""
    b = Image.open(path).convert('RGBA').tobytes()
    return all(b[i + 3] == 0 or b[i:i + 4] == b'\xff\xff\xff\xff' for i in range(0, len(b), 4))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    who = args[0] if args else 'mario'
    out = os.path.join(ROOT, 'ui', 'retro', who)
    os.makedirs(os.path.join(out, 'guide'), exist_ok=True)
    manifest = {'character': who, 'pixel_scale': 1, 'facing': 'right', 'fallback': FALLBACK, 'sheets': {}}
    old = {}
    try: old = json.load(open(os.path.join(out, 'frames.json')))['sheets']
    except (OSError, ValueError, KeyError): pass
    for form, (w, h), extra in FORMS:
        frames = COMMON + extra
        rows = (len(frames) + COLS - 1) // COLS
        W, H = GAP + COLS * (w + GAP), GAP + rows * (h + GAP)
        sheet = Image.new('RGBA', (W, H), (0, 0, 0, 0))
        d = ImageDraw.Draw(sheet)
        rects = {}
        for i, (name, _) in enumerate(frames):
            x, y = GAP + (i % COLS) * (w + GAP), GAP + (i // COLS) * (h + GAP)
            d.rectangle([x, y, x + w - 1, y + h - 1], fill=(255, 255, 255, 255))
            rects[name] = [x, y, w, h]
        p = os.path.join(out, form + '.png')
        painted = os.path.exists(p) and not is_template(p)
        if painted and form in old:
            # painted: keep the frame positions frames.json has for it (they may have been
            # measured from the art, see docs/RETRO_MARIO.md)
            manifest['sheets'][form] = old[form]
            continue
        manifest['sheets'][form] = {'file': form + '.png', 'size': [W, H], 'box': [w, h], 'frames': rects}
        if not painted: sheet.save(p)          # new, or still an untouched template: (re)write it
        # guide: 8x, with names
        S, LH = 8, 26
        gw, gh = W * S, H * S + rows * LH + 40
        g = Image.new('RGB', (gw, gh), (40, 44, 52))
        gd = ImageDraw.Draw(g)
        gd.text((8, 8), f'{who} / {form}.png  ({w}x{h} boxes, {len(frames)} frames, sheet {W}x{H})', fill=(255, 220, 120), font=font(20))
        for i, (name, desc) in enumerate(frames):
            x, y, _, _ = rects[name]
            r = i // COLS
            gx, gy = x * S, 40 + y * S + r * LH
            gd.rectangle([gx, gy, gx + w * S - 1, gy + h * S - 1], fill=(255, 255, 255), outline=(150, 150, 150))
            gd.line([gx, gy + h * S - 1, gx + w * S - 1, gy + h * S - 1], fill=(220, 60, 60), width=3)   # feet line
            opt = 'optional' in desc
            gd.text((gx + 4, gy + 4), str(i + 1), fill=(120, 120, 120), font=font(16))
            gd.text((gx + 2, gy + h * S + 3), name, fill=(170, 170, 170) if opt else (255, 255, 255), font=font(15))
        g.save(os.path.join(out, 'guide', form + '.png'))
    json.dump(manifest, open(os.path.join(out, 'frames.json'), 'w'), indent=1)
    # a text list of all frames
    with open(os.path.join(out, 'guide', 'FRAMES.txt'), 'w') as f:
        f.write(__doc__ + '\n')
        for form, (w, h), extra in FORMS:
            f.write(f'\n{form}.png ({w}x{h})\n')
            for i, (name, desc) in enumerate(COMMON + extra): f.write(f'  {i + 1:2d}. {name:12s} {desc}\n')
    print(f'ui/retro/{who}: {len(FORMS)} sheets, frames.json, guide/')


if __name__ == '__main__':
    main()
