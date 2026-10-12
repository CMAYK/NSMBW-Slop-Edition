#!/usr/bin/env python3
"""Find code that builds a pointer to a string (lis/addi pairs, incl. base+offset forms)."""
import sys, struct, re, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from recomp import Image, load_symbols
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
img = Image(); img.load_dol(os.path.join(ROOT, 'orig', 'main.dol'))
funcs, _ = load_symbols(os.path.join(ROOT, 'config', 'symbols.txt'))
needle = sys.argv[1].encode()
d = open(os.path.join(ROOT, 'orig', 'main.dol'), 'rb').read()
off = struct.unpack('>18I', d[0:72]); addr = struct.unpack('>18I', d[72:144]); sz = struct.unpack('>18I', d[144:216])
i = d.find(needle); s = d.rfind(b'\0', 0, i) + 1
target = next(addr[k] + s - off[k] for k in range(18) if sz[k] and off[k] <= s < off[k] + sz[k])
print('string at %08X' % target)
# scan all code: track lis values per register, report any addi/ori whose result is within 0x800 of target
for a0, size, name, sec in funcs:
    if sec not in ('.init', '.text'): continue
    hi = {}
    for a in range(a0, a0 + size, 4):
        w = img.word(a); op = w >> 26; D = (w >> 21) & 31; A = (w >> 16) & 31; imm = w & 0xFFFF
        if op == 15 and A == 0: hi[D] = imm << 16
        elif op == 14 and A in hi:
            v = (hi[A] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
            if 0 <= target - v < 0x1000: print('%08X in %s: base %08X (+0x%X)' % (a, name, v, target - v))
