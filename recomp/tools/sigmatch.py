#!/usr/bin/env python3
"""Name SDK/library functions by matching Dolphin's signature database (totaldb.dsy).

Checksum = Dolphin's HashSignatureDB::ComputeCodeChecksum: opcode bits only (immediates /
registers / branch targets masked), so it survives relocation and register allocation.
Only plain C identifiers are applied, and only to dtk's auto-named functions.
    python tools/sigmatch.py [--apply]
"""
import os, re, struct, sys
from collections import defaultdict, Counter
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from recomp import Image, load_symbols

def checksum(img, start, size):
    s = 0
    for a in range(start, start + size, 4):
        w = img.word(a)
        if w is None: return None
        op = w & 0xFC000000; aux = op >> 26; op2 = op3 = 0
        if aux == 4:
            op2 = w & 0x3F
            if op2 in (0, 8, 16, 21, 22): op3 = w & 0x7C0
        elif aux in (7, 8, 10, 11, 12, 13, 14, 15): op2 = w & 0x03FF0000
        elif aux in (19, 31, 63): op2 = w & 0x7FF
        elif aux == 59:
            op2 = w & 0x3F
            if op2 < 16: op3 = w & 0x7C0
        elif 32 <= aux < 56: op2 = w & 0x03FF0000
        s = ((s << 17) & 0xFFFE0000) | ((s >> 15) & 0x1FFFF)
        s ^= (op | op2 | op3)
    return s

def load_db(path):
    d = open(path, 'rb').read(); n = struct.unpack_from('<I', d)[0]; db = defaultdict(set)
    for i in range(n):
        c, sz = struct.unpack_from('<II', d, 4 + i * 136)
        name = d[4 + i * 136 + 8: 4 + i * 136 + 136].split(b'\0')[0].decode('latin-1').split('\t')[0].strip()
        db[(c, sz)].add(name)
    return db

def main():
    apply = '--apply' in sys.argv
    db = load_db(os.path.join(HERE, 'dolphin_totaldb.dsy'))
    img = Image(); img.load_dol(os.path.join(ROOT, 'orig', 'main.dol'))
    path = os.path.join(ROOT, 'config', 'symbols.txt')
    funcs, _ = load_symbols(path)
    found = {}; st = Counter()
    for a, sz, name, sec in funcs:
        if sec not in ('.init', '.text'): continue
        names = db.get((checksum(img, a, sz), sz))
        if not names: continue
        st['matched'] += 1
        if len(names) > 1: st['ambiguous'] += 1; continue
        n = next(iter(names))
        if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', n): st['non_c_name'] += 1; continue
        found[a] = n
    # apply: only rename auto-named functions, avoid duplicate names
    lines = open(path).read().splitlines(); existing = {l.split(' = ')[0] for l in lines}
    pat = re.compile(r'^(\S+) = (\.[\w.]+):0x([0-9A-Fa-f]+);(.*)$')
    for i, l in enumerate(lines):
        m = pat.match(l)
        if not m or 'type:function' not in m.group(4): continue
        a = int(m.group(3), 16)
        if a not in found: continue
        if m.group(1) == found[a]: st['already'] += 1; continue
        if not re.match(r'^(fn|FUN)_', m.group(1)): st['kept_existing'] += 1; continue
        if found[a] in existing: st['dup'] += 1; continue
        st['renamed'] += 1; existing.add(found[a])
        lines[i] = f'{found[a]} = {m.group(2)}:0x{m.group(3)};{m.group(4)}'
    print(dict(st))
    if apply: open(path, 'w').write('\n'.join(lines) + '\n')

if __name__ == '__main__':
    main()
