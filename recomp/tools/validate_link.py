#!/usr/bin/env python3
"""Sanity checks that the REL modules were linked at the right addresses.

1. Every direct branch (b/bl) in REL code should land on a known function start.
2. Every code pointer stored in REL data (vtables, callback tables) should too.
3. Names ported from PAL v1 (Newer kamek_pal.x) for REL addresses should land on
   function starts when they name functions, and __vt__ vtables should hold code pointers.
If the load addresses were wrong, all three numbers collapse.
"""
import os, re, sys, struct, bisect
from collections import Counter
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from recomp import Image, load_symbols
from nsmbw_versions import VersionMap

img = Image(); img.load_dol(os.path.join(ROOT, 'orig', 'main.dol'))
funcs, _ = load_symbols(os.path.join(ROOT, 'config', 'symbols.txt'))
man = img.load_linked(os.path.join(ROOT, 'linked'))
for m in man:
    f2, _ = load_symbols(os.path.join(ROOT, 'config', m['name'], 'symbols.txt'), {s['name']: s['addr'] for s in m['sections']})
    funcs += f2
starts = {f[0] for f in funcs if f[3] in ('.init', '.text')}
text_ranges = sorted((a, a + len(b)) for (a, b), k in zip(img.segs, img.kinds) if k == 'text')
tstarts = [r[0] for r in text_ranges]
def in_text(v):
    i = bisect.bisect_right(tstarts, v) - 1
    return i >= 0 and v < text_ranges[i][1]

rel_text = [(s['addr'], s['size']) for m in man for s in m['sections'] if s['kind'] == 'text']
rel_data = [(s['addr'], s['size']) for m in man for s in m['sections'] if s['kind'] == 'data']

# 1. direct branches
c = Counter()
for a0, sz in rel_text:
    for a in range(a0, a0 + sz, 4):
        w = img.word(a)
        if w >> 26 != 18: continue
        li = w & 0x3FFFFFC; li -= 0x4000000 if li & 0x2000000 else 0
        t = (0 if w & 2 else a) + li
        if w & 1: c['bl_total'] += 1; c['bl_hit'] += t in starts
        elif not in_text(t): c['b_outside_text'] += 1
print(f"REL bl calls: {c['bl_hit']}/{c['bl_total']} land on function starts "
      f"({100 * c['bl_hit'] / c['bl_total']:.2f}%); b to non-code: {c['b_outside_text']}")

# 2. code pointers in REL data
p = Counter()
for a0, sz in rel_data:
    for a in range(a0, a0 + sz - 3, 4):
        v = img.word(a)
        if v is not None and in_text(v): p['ptr'] += 1; p['hit'] += v in starts
print(f"REL data words pointing into code: {p['ptr']}, at function starts: {p['hit']} ({100 * p['hit'] / max(1, p['ptr']):.2f}%)")

# 3. ported PAL v1 names
vm = VersionMap(os.path.join(HERE, 'versions-nsmbw.txt'))
lo, hi = man[0]['load_addr'], max(s['addr'] + s['size'] for m in man for s in m['sections'])
k = Counter()
for l in open(os.path.join(HERE, 'kamek_pal.x')):
    mm = re.match(r'\s*(\S+)\s*=\s*0x([0-9A-Fa-f]+);', l)
    if not mm: continue
    e2 = vm.port(int(mm.group(2), 16), 'E2')
    if e2 is None or not lo <= e2 < hi: continue
    name = mm.group(1)
    if name.startswith('__vt__'):
        ws = [img.word(e2 + 8 + 4 * i) for i in range(4)]
        k['vt'] += 1; k['vt_ok'] += all(w is not None and (w == 0 or w in starts) for w in ws) and any(ws)
    elif in_text(e2):
        k['fn'] += 1; k['fn_ok'] += e2 in starts
print(f"Ported REL function names on function starts: {k['fn_ok']}/{k['fn']}; vtables with valid code pointers: {k['vt_ok']}/{k['vt']}")
