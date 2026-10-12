#!/usr/bin/env python3
"""Find function boundaries in the linked d_project module (the release build has no symbols)
and write config/d_project/symbols.txt in dtk format (section-relative, like the other modules)."""
import bisect, json, os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from recomp import load_symbols


def main():
    info = json.load(open(os.path.join(ROOT, 'linked', 'mod.json')))
    img = open(os.path.join(ROOT, 'linked', info['image']), 'rb').read(); B = info['load_addr']
    text = [s for s in info['sections'] if s['kind'] == 'text'][0]
    T0, T1 = text['addr'], text['addr'] + text['size']
    w = lambda a: struct.unpack_from('>I', img, a - B)[0]
    intext = lambda a: T0 <= a < T1
    p = json.load(open(os.path.join(ROOT, 'linked', 'mod_patches.json')))
    relocs = json.load(open(os.path.join(ROOT, 'linked', 'mod_relocs.json')))
    # names for replaced functions from the game's symbols
    gfuncs, _ = load_symbols(os.path.join(ROOT, 'config', 'symbols.txt'))
    man = json.load(open(os.path.join(ROOT, 'linked', 'manifest.json')))
    for m in man:
        f2, _ = load_symbols(os.path.join(ROOT, 'config', m['name'], 'symbols.txt'), {s['name']: s['addr'] for s in m['sections']})
        gfuncs += f2
    gname = {a: n for a, s, n, _ in gfuncs}
    names = {}
    starts = {info['prolog'], info['epilog'], info['unresolved']}
    names[info['prolog']] = '_prolog'; names[info['epilog']] = '_epilog'; names[info['unresolved']] = '_unresolved'
    for g, d in p['replace']:
        starts.add(d); names.setdefault(d, 'mod_' + gname.get(g, f'fn_{g:08X}'))
    thunks = {t: g for t, g in p['extern']}
    for t, g in thunks.items(): starts.add(t); names[t] = 'ext_' + gname.get(g, f'fn_{g:08X}')
    for k, v in p['pre'].items():
        if intext(v): starts.add(v); names[v] = k
    for c in p['ctors']: starts.add(c); names.setdefault(c, f'ctor_{c:08X}')
    for P, S, typ, mid in relocs:
        if intext(S) and typ in (1, 4, 6) and not intext(P) or (intext(S) and typ in (4, 6)):
            starts.add(S)
    # direct calls
    for a in range(T0, T1, 4):
        x = w(a)
        if x >> 26 == 18 and x & 1:
            li = x & 0x3FFFFFC; li -= 0x4000000 if li & 0x2000000 else 0
            t = ((0 if x & 2 else a) + li) & 0xFFFFFFFF
            if intext(t): starts.add(t)
    # tail branches leaving their function become starts too (iterate to a fixed point)
    while True:
        starts = {s for s in starts if intext(s)}; st = sorted(starts); new = set()
        for i, s in enumerate(st):
            e = st[i + 1] if i + 1 < len(st) else T1
            for a in range(s, e, 4):
                x = w(a)
                if x >> 26 == 18 and not x & 1:
                    li = x & 0x3FFFFFC; li -= 0x4000000 if li & 0x2000000 else 0
                    t = ((0 if x & 2 else a) + li) & 0xFFFFFFFF
                    if intext(t) and not s <= t < e and t not in starts: new.add(t)
        if not new: break
        starts |= new
    st = sorted(s for s in starts if intext(s))
    os.makedirs(os.path.join(ROOT, 'config', 'd_project'), exist_ok=True)
    with open(os.path.join(ROOT, 'config', 'd_project', 'symbols.txt'), 'w') as f:
        for i, s in enumerate(st):
            e = st[i + 1] if i + 1 < len(st) else T1
            # trim trailing zero padding
            while e - 4 > s and w(e - 4) == 0: e -= 4
            n = names.get(s, f'mfn_{s:08X}')
            f.write(f'{n} = .text:0x{s - T0:08X}; // type:function size:0x{e - s:X}\n')
    print('module functions:', len(st))


if __name__ == '__main__':
    main()
