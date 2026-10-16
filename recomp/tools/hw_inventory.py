#!/usr/bin/env python3
"""Which recompiled functions touch Wii hardware registers directly (track A inventory,
docs/NATIVE_ROADMAP.md). Finds `lis rX, 0xCC0x/0xCD0x/0xCD8x` and the loads/stores/addi
that use rX within the next instructions, and maps the address to a hardware block.

    python3 tools/hw_inventory.py [--by-block] [--json out.json]
"""
import argparse, bisect, json, os, sys, struct
from collections import defaultdict
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from recomp import Image, load_symbols

BLOCKS = [  # physical base, size, name
    (0x0C000000, 0x1000, 'CP'), (0x0C001000, 0x1000, 'PE'), (0x0C002000, 0x1000, 'VI'),
    (0x0C003000, 0x1000, 'PI'), (0x0C004000, 0x1000, 'MI'), (0x0C005000, 0x1000, 'DSP'),
    (0x0C006000, 0x400, 'DI(GC)'), (0x0C006400, 0x400, 'SI'), (0x0C006800, 0x400, 'EXI'),
    (0x0C006C00, 0x400, 'AI'), (0x0C008000, 0x1000, 'WGPIPE'),
    (0x0D000000, 0x800, 'IPC/HW'), (0x0D006000, 0x400, 'DI'), (0x0D006400, 0x400, 'SI'),
    (0x0D006800, 0x400, 'EXI'), (0x0D006C00, 0x400, 'AI'), (0x0D800000, 0x800, 'IPC/HW'),
]
def block(a):
    p = a & 0x3FFFFFFF
    for b, s, n in BLOCKS:
        if b <= p < b + s: return n
    return None

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--json'); a = ap.parse_args()
    img = Image(); img.load_dol(os.path.join(ROOT, 'orig', 'main.dol'))
    funcs, _ = load_symbols(os.path.join(ROOT, 'config', 'symbols.txt'))
    funcs = [f for f in funcs if f[3] in ('.init', '.text')]
    man = img.load_linked(os.path.join(ROOT, 'linked'))
    for m in man:
        f2, _ = load_symbols(os.path.join(ROOT, 'config', m['name'], 'symbols.txt'), {s['name']: s['addr'] for s in m['sections']})
        funcs += [f for f in f2 if f[3] == '.text']
    funcs.sort()
    found = defaultdict(set)
    for addr, size, name, sec in funcs:
        for pc in range(addr, addr + size, 4):
            w = img.word(pc)
            if w is None or w >> 26 != 15 or (w >> 16) & 31: continue      # lis = addis rD, 0, imm
            imm = w & 0xFFFF
            if imm >> 8 not in (0xCC, 0xCD): continue
            rd = (w >> 21) & 31; hi = imm << 16
            hit = False
            for q in range(pc + 4, min(pc + 40, addr + size), 4):
                x = img.word(q); op = x >> 26
                if op in (14, 24, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 50, 52, 54) and (x >> 16) & 31 == rd:
                    d = x & 0xFFFF; d -= 0x10000 if d & 0x8000 and op != 24 else 0
                    t = (hi | d) if op == 24 else (hi + d) & 0xFFFFFFFF
                    b = block(t)
                    if b: found[(addr, name)].add(b); hit = True
                if op in (14, 15, 24) and (x >> 21) & 31 == rd: break       # rd overwritten
            if not hit and block(hi): found[(addr, name)].add(block(hi))
    by_block = defaultdict(list)
    for (addr, name), bl in sorted(found.items()):
        for b in bl: by_block[b].append(name)
    for b in sorted(by_block): print(f'{b:8s} {len(by_block[b]):4d}  ' + ', '.join(sorted(set(by_block[b]))[:400]))
    print('functions touching hardware:', len(found))
    if a.json: json.dump({f'{k[0]:08X}': [k[1], sorted(v)] for k, v in found.items()}, open(a.json, 'w'), indent=0)

if __name__ == '__main__': main()
