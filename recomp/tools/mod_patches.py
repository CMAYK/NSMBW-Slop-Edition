#!/usr/bin/env python3
"""Extract what the d_project module's _prolog would patch at runtime, so the patches can be
applied statically before recompiling (the recompiled game can't rewrite its own code).

Arrays (located by disassembling _prolog of the release build):
  replace    {game addr, module function}      -> game function redirected to module code
  extern     {game addr, module thunk}         -> thunk = original game function (unpatched)
  references {dest, count, {type, addend, addr}[]} -> lis/addi immediates in game code
  four       {addr, size, offset}              -> player-count immediates 4 -> 8
"""
import json, os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from recomp import Image

ARR = dict(replace=(0x81845460, 0x81846C98), refs=(0x81846C98, 0x81848370),
           extern=(0x81848370, 0x818483D8), four=(0x8185133C, 288), ctors=(0x8185E810, 0x8185E84C))
PRE = dict(preinit=0x81840F24, main=0x81840F48, find_code_region=0x818145D4, code_region_var=0x8185ED00,
           stack_chk_init=0x81844FC0)
PLAYER_COUNT = 8


def main():
    info = json.load(open(os.path.join(ROOT, 'linked', 'mod.json')))
    mimg = open(os.path.join(ROOT, 'linked', info['image']), 'rb').read(); B = info['load_addr']
    w = lambda a: struct.unpack_from('>I', mimg, a - B)[0]
    img = Image(); img.load_dol(os.path.join(ROOT, 'orig', 'main.dol')); img.load_linked(os.path.join(ROOT, 'linked'))
    out = {'pre': PRE}
    # replace
    rep = []
    for a in range(*ARR['replace'], 8): rep.append((w(a), w(a + 4)))
    out['replace'] = [r for r in rep if r[0]]
    out['replace_skipped'] = sum(1 for r in rep if not r[0])
    # extern thunks
    ext = []
    for a in range(*ARR['extern'], 8):
        g, thunk = w(a), w(a + 4)
        ext.append((thunk, g))
    out['extern'] = ext
    # reference patches
    refs = []; a, end = ARR['refs']
    while a < end:
        dest, cnt = w(a), w(a + 4); p = a + 8
        for i in range(cnt):
            typ = mimg[p - B]; add = struct.unpack_from('>h', mimg, p + 2 - B)[0]; addr = w(p + 4); p += 8
            if not addr: continue
            v = (dest + add) & 0xFFFFFFFF
            if typ == 4: half = v & 0xFFFF
            elif typ == 5: half = v >> 16
            elif typ == 6: half = ((v + 0x8000) >> 16) & 0xFFFF
            else: raise SystemExit(f'bad ref type {typ}')
            refs.append((addr, half, typ, dest))
        a = p
    out['refs'] = refs
    # four patches
    four = []; bad = 0
    a0, n = ARR['four']
    for i in range(n):
        a = a0 + 8 * i
        addr, size, off = w(a), mimg[a + 4 - B], struct.unpack('b', mimg[a + 5 - B:a + 6 - B])[0]
        if not addr: continue
        neg = off <= -64
        if neg: off -= -64
        old, new = 4 + off, PLAYER_COUNT + off
        if neg: old, new = -old, -new
        word = img.word(addr & ~3)
        sh = (4 - (addr & 3) - size) * 8
        cur = (word >> sh) & ((1 << 8 * size) - 1)
        if cur != old & ((1 << 8 * size) - 1): bad += 1; print(f'four: {addr:08X} expected {old} found {cur}')
        four.append((addr, size, new & ((1 << 8 * size) - 1)))
    out['four'] = four
    # Riivolution memory patches from mkwcat-nsmbw.xml (US addresses), except the loader hook
    out['memory'] = [(0x8015BDF4, 0x60000000, 0x981F0102),   # wait for the module loader
                     (0x800B6490, 0x3C800012, 0x3C800010),   # expand layout heap
                     (0x800E4D70, 0x38600000, 0x3863300C)]   # disable exception handler input sequence
    for a, new, old in out['memory']:
        if img.word(a) != old: raise SystemExit(f'memory patch {a:08X}: found {img.word(a):08X}, expected {old:08X}')
    out['ctors'] = [w(a) for a in range(*ARR['ctors'], 4) if w(a)]
    json.dump(out, open(os.path.join(ROOT, 'linked', 'mod_patches.json'), 'w'), indent=0)
    print(f"replace {len(out['replace'])} (+{out['replace_skipped']} unported), extern {len(ext)}, refs {len(refs)}, "
          f"four {len(four)} ({bad} mismatched), ctors {len(out['ctors'])}")
    # sanity: replace targets in game code / module text
    t0 = [s for s in info['sections'] if s['kind'] == 'text'][0]
    badr = [r for r in out['replace'] if not (t0['addr'] <= r[1] < t0['addr'] + t0['size'])]
    print('replace dests outside module text:', len(badr), [tuple(map(hex, r)) for r in badr[:5]])
    print('extern sample:', [(hex(t), hex(g), hex(struct.unpack_from('>I', mimg, t - B)[0]), hex(struct.unpack_from('>I', mimg, t + 4 - B)[0])) for t, g in ext[:3]])


if __name__ == '__main__':
    main()
