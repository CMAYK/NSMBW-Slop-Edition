#!/usr/bin/env python3
"""Take character art and models from the mods in game/mod/ (zips, as downloaded) for the
character list (docs/CHARACTERS.md).

    python3 tools/import_char_mods.py [--game ../game] [--force]

Recognised mods (by their content, any file name):
  - "8 Player Mod Toad Addon": a replacement 8-player archive (mkwcat-nsmbw.arc); its Kinopio
    model turns the Black Toad textures into Toadsworth; its HUD icons (Toadette, Toadsworth...)
  - "Rosalina and Daisy for NSMBW" (Riivolution): Kinopio.arc = Daisy (blue Toad's frames) and
    Rosalina (yellow Toad's), its own P_rcha/K_rcha animations; portraits
  - "Playable Goombas for NSMBW" (Riivolution): Kinopio.arc = Goombas; portraits

Writes:
  ui/characters/<id>/portrait.png    - from the mods' character-select portraits (150x150);
                                       Toadette / Toadsworth have none: their HUD icon, scaled
                                       (a placeholder from make_char_portraits.py is replaced,
                                       a painted portrait never is - unless --force)
  <game>/mod/characters/<set>/*.brres - the models and animations the game loads for the
                                       characters that use them (runtime/rt_charmdl.c)
"""
import argparse, io, os, struct, sys, zipfile
try:
    from PIL import Image
except ImportError:          # portraits need Pillow (python -m pip install pillow); models don't
    Image = None

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZE = 150


# ---- formats ----
def u8_files(d):
    if d[:4] == b'Yaz0': d = yaz0(d)
    assert d[:4] == b'\x55\xaa\x38\x2d', 'not a U8 archive'
    root = struct.unpack('>I', d[4:8])[0]
    n = struct.unpack('>I', d[root + 8:root + 12])[0]; st = root + n * 12
    def name(o): e = d.index(b'\0', st + o); return d[st + o:e].decode('latin1')
    out, path, ends = {}, [], []
    for i in range(n):
        t = d[root + i * 12]; no = struct.unpack('>I', b'\0' + d[root + i * 12 + 1:root + i * 12 + 4])[0]
        a, b = struct.unpack('>II', d[root + i * 12 + 4:root + i * 12 + 12])
        while ends and i >= ends[-1]: ends.pop(); path.pop()
        nm = name(no)
        if t == 1:
            if i: path.append(nm); ends.append(b)
        else: out['/'.join(path + [nm])] = d[a:a + b]
    return out


def yaz0(d):
    size = struct.unpack('>I', d[4:8])[0]; out = bytearray(); p = 16
    while len(out) < size:
        code = d[p]; p += 1
        for bit in range(8):
            if len(out) >= size: break
            if code & (0x80 >> bit): out.append(d[p]); p += 1
            else:
                b1, b2 = d[p], d[p + 1]; p += 2
                dist = ((b1 & 15) << 8 | b2) + 1; n = b1 >> 4
                if n == 0: n = d[p] + 0x12; p += 1
                else: n += 2
                for _ in range(n): out.append(out[-dist])
    return bytes(out)


def tpl_image(tpl):
    tab = struct.unpack('>I', tpl[8:12])[0]; ih = struct.unpack('>I', tpl[tab:tab + 4])[0]
    h, w, fmt, off = struct.unpack('>HHII', tpl[ih:ih + 12])
    im = Image.new('RGBA', (w, h)); px = im.load(); p = off
    if fmt == 5:        # RGB5A3
        for ty in range(0, h, 4):
            for tx in range(0, w, 4):
                for i in range(16):
                    v = struct.unpack('>H', tpl[p:p + 2])[0]; p += 2
                    x, y = tx + (i & 3), ty + (i >> 2)
                    if x >= w or y >= h: continue
                    if v & 0x8000: px[x, y] = ((v >> 10 & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v & 31) * 255 // 31, 255)
                    else: px[x, y] = ((v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17, (v >> 12 & 7) * 255 // 7)
    elif fmt == 6:      # RGBA8
        for ty in range(0, h, 4):
            for tx in range(0, w, 4):
                ar, gb = tpl[p:p + 32], tpl[p + 32:p + 64]; p += 64
                for i in range(16):
                    x, y = tx + (i & 3), ty + (i >> 2)
                    if x < w and y < h: px[x, y] = (ar[i * 2 + 1], gb[i * 2], gb[i * 2 + 1], ar[i * 2])
    else: raise ValueError('TPL format %d' % fmt)
    return im


# ---- the mods ----
def read_zip(path):
    z = zipfile.ZipFile(path)
    return {n.replace('\\', '/'): z for n in z.namelist() if not n.endswith('/')}


def find(files, suffix):
    for n, z in files.items():
        if n.lower().endswith(suffix.lower()): return z.read(n)
    return None


def identify(files):
    names = ' '.join(files).lower()
    if 'mkwcat-nsmbw.arc' in names: return 'toad_addon'
    if 'rosalina' in names or 'daisy' in names: return 'rosalina_daisy'
    if 'goomba' in names: return 'goomba'
    return None


def is_placeholder(path):
    try: im = Image.open(path).convert('RGBA')
    except OSError: return True
    return im.size == (SIZE, SIZE) and im.getpixel((SIZE // 2, 12)) == (250, 250, 250, 254)


def save_portrait(cid, im, force, how):
    p = os.path.join(ROOT, 'ui', 'characters', cid, 'portrait.png')
    os.makedirs(os.path.dirname(p), exist_ok=True)
    if os.path.exists(p) and not is_placeholder(p) and not force:
        print('  %-12s kept (painted)' % cid); return
    im.save(p); print('  %-12s portrait from %s' % (cid, how))


def icon_portrait(icon):
    """a 48x48 HUD icon as a 150x150 portrait: scaled 3x, bottom-aligned like the game's"""
    big = icon.resize((icon.width * 3, icon.height * 3), Image.LANCZOS)
    out = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    out.alpha_composite(big, ((SIZE - big.width) // 2, SIZE - big.height))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=os.path.join(ROOT, '..', 'game'))
    ap.add_argument('--force', action='store_true', help='also replace portraits that were painted')
    a = ap.parse_args()
    moddir = os.path.join(a.game, 'mod')
    mods = {}
    for f in sorted(os.listdir(moddir)) if os.path.isdir(moddir) else []:
        if f.lower().endswith('.zip'):
            files = read_zip(os.path.join(moddir, f)); kind = identify(files)
            if kind: mods[kind] = files; print('%s: %s' % (f, kind))
    if not mods: print('no character mods in', moddir); return

    # portraits
    if Image is None: print('  (portraits skipped: python -m pip install pillow)')
    if Image and 'rosalina_daisy' in mods:
        arc = u8_files(find(mods['rosalina_daisy'], 'charaChangeSelectContents/charaChangeSelectContents.arc'))
        save_portrait('daisy', tpl_image(arc['arc/timg/Im_plofileKinoB_00.tpl']), a.force, 'Rosalina and Daisy mod')
        save_portrait('rosalina', tpl_image(arc['arc/timg/Im_plofileKinoY_00.tpl']), a.force, 'Rosalina and Daisy mod')
    if Image and 'goomba' in mods:
        arc = u8_files(find(mods['goomba'], 'charaChangeSelectContents/charaChangeSelectContents.arc'))
        save_portrait('goomba', tpl_image(arc['arc/timg/Im_plofileKinoY_00.tpl']), a.force, 'Goomba mod')
    icons = None
    if 'toad_addon' in mods: icons = u8_files(find(mods['toad_addon'], 'mkwcat-nsmbw.arc')); how = 'Toad addon icon'
    elif os.path.exists(os.path.join(ROOT, 'mod', 'nsmbw-8p.arc')):
        icons = u8_files(open(os.path.join(ROOT, 'mod', 'nsmbw-8p.arc'), 'rb').read()); how = '8-player icon'
    if icons and Image:
        g = 'Layout/gameScene/gameScene.arc/arc/timg/'
        save_portrait('toadette', icon_portrait(tpl_image(icons[g + 'im_kinopicoIcon_01.tpl'])), a.force, how)
        if 'toad_addon' in mods:
            save_portrait('toadsworth', icon_portrait(tpl_image(icons[g + 'im_kinopioIcon_Black_01.tpl'])), a.force, how)

    # models (runtime/rt_charmdl.c loads these)
    out = os.path.join(a.game, 'mod', 'characters')
    def put(sub, name, data):
        d = os.path.join(out, sub); os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, name), 'wb') as f: f.write(data)
        print('  mod/characters/%s/%s (%d KB)' % (sub, name, len(data) // 1024))
    if 'toad_addon' in mods:
        arc = u8_files(find(mods['toad_addon'], 'mkwcat-nsmbw.arc'))
        put('toad', 'model.brres', arc['Object/Kinopio.arc/g3d/model.brres'])
    if 'rosalina_daisy' in mods:
        m = mods['rosalina_daisy']
        put('rosalina_daisy', 'model.brres', u8_files(find(m, 'Object/Kinopio.arc'))['g3d/model.brres'])
        put('rosalina_daisy', 'P_rcha.brres', u8_files(find(m, 'Object/P_rcha.arc'))['g3d/model.brres'])
        put('rosalina_daisy', 'K_rcha.brres', u8_files(find(m, 'Object/K_rcha.arc'))['g3d/model.brres'])
    if 'goomba' in mods:
        put('goomba', 'model.brres', u8_files(find(mods['goomba'], 'Object/Kinopio.arc'))['g3d/model.brres'])


if __name__ == '__main__':
    main()
