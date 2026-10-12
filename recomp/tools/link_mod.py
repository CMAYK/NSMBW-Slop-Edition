#!/usr/bin/env python3
"""Statically link mkwcat's nsmbw-project module (d_project.rel + the USA rev 2 import table)
at a fixed address, the way its loader + OSLinkFixed would at runtime.

Output: linked/d_project.bin (image from MOD_BUF) and an entry in linked/mod.json.
"""
import json, os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
MOD_BUF = 0x81800000   # just past the end of MEM1: free in the runtime's 1 GB guest map

R_ADDR32, R_ADDR24, R_ADDR16, R_LO, R_HI, R_HA, R_ADDR14 = 1, 2, 3, 4, 5, 6, 7
R_REL24, R_REL14 = 10, 11
R_NOP, R_SECTION, R_END = 201, 202, 203


def load(rel_path, imp_path):
    d = bytearray(open(rel_path, 'rb').read())
    h = struct.unpack('>IIIIIIIIIIIIBBBBIII', d[:0x40])
    mid, ns, so, bss_size, imp_off, imp_size = h[0], h[3], h[4], h[8], h[10], h[11]
    pro_s, epi_s, unr_s, pro, epi, unr = h[12], h[13], h[14], h[16], h[17], h[18]
    align, bss_align, fix = struct.unpack('>III', d[0x40:0x4C])
    imps = [struct.unpack_from('>II', d, imp_off + 8 * k) for k in range(imp_size // 8)]
    first_ext = next(off for m, off in imps if m < 0x100)
    d = d[:first_ext] + open(imp_path, 'rb').read()
    secs = []
    for i in range(ns):
        off, sz = struct.unpack_from('>II', d, so + 8 * i)
        secs.append((off & ~1, sz, bool(off & 1)))
    fix32 = (fix + 31) & ~31
    bss_addr = MOD_BUF + fix32
    def sec_addr(i):
        off, sz, _ = secs[i]
        return MOD_BUF + off if off else (bss_addr if sz else 0)
    return dict(d=d, id=mid, secs=secs, imps=imps, sec_addr=sec_addr, bss_addr=bss_addr, bss_size=bss_size,
                fix=fix, entry=lambda s, o: sec_addr(s) + o, pro=(pro_s, pro), epi=(epi_s, epi), unr=(unr_s, unr))


def main():
    man = json.load(open(os.path.join(ROOT, 'linked', 'manifest.json')))
    order = ['.text', '.ctors', '.dtors', '.rodata', '.data', '.bss']
    rels = {}
    for m in man:
        a = {s['name']: s['addr'] for s in m['sections']}
        rels[m['id']] = [None] + [a.get(n) for n in order]
    M = load(os.path.join(ROOT, 'mod', 'd_project.rel'), os.path.join(ROOT, 'mod', 'd_project_E2.imp'))
    d, secs = M['d'], M['secs']
    stats = {}
    self_relocs = []          # (patched address, target) for analysis
    for mod_id, off in M['imps']:
        p = off; cur_sec = None; cur = 0; n = 0
        while True:
            o, typ, sec, add = struct.unpack_from('>HBBI', d, p); p += 8
            cur += o
            if typ == R_END: break
            if typ == R_SECTION: cur_sec = sec; cur = 0; continue
            if typ == R_NOP: continue
            if mod_id == M['id']: S = M['sec_addr'](sec) + add
            elif mod_id == 0: S = add
            else:
                base = rels[mod_id][sec]
                if base is None: raise SystemExit(f'import into module {mod_id} section {sec} unknown')
                S = base + add
            S &= 0xFFFFFFFF
            fo = secs[cur_sec][0] + cur; P = MOD_BUF + fo
            if typ == R_ADDR32: struct.pack_into('>I', d, fo, S)
            elif typ == R_ADDR16 or typ == R_LO: struct.pack_into('>H', d, fo, S & 0xFFFF)
            elif typ == R_HI: struct.pack_into('>H', d, fo, S >> 16)
            elif typ == R_HA: struct.pack_into('>H', d, fo, ((S + 0x8000) >> 16) & 0xFFFF)
            elif typ == R_REL24:
                w = struct.unpack_from('>I', d, fo)[0]; struct.pack_into('>I', d, fo, (w & ~0x03FFFFFC) | ((S - P) & 0x03FFFFFC))
            elif typ == R_REL14:
                w = struct.unpack_from('>I', d, fo)[0]; struct.pack_into('>I', d, fo, (w & ~0xFFFC) | ((S - P) & 0xFFFC))
            elif typ == R_ADDR24:
                w = struct.unpack_from('>I', d, fo)[0]; struct.pack_into('>I', d, fo, (w & ~0x03FFFFFC) | (S & 0x03FFFFFC))
            else: raise SystemExit(f'unhandled relocation type {typ}')
            self_relocs.append((P, S, typ, mod_id)); n += 1
        stats[mod_id] = n
    out = os.path.join(ROOT, 'linked'); os.makedirs(out, exist_ok=True)
    img = bytes(d[:M['fix']])
    open(os.path.join(out, 'd_project.bin'), 'wb').write(img)
    names = {1: '.text', 2: '.data'}
    sections = []
    for i, (off, sz, ex) in enumerate(secs):
        if not sz: continue
        sections.append({'name': '.text' if ex else ('.bss' if not off else '.data'), 'addr': M['sec_addr'](i), 'size': sz,
                         'kind': 'text' if ex else ('bss' if not off else 'data'), 'file_off': off})
    info = {'name': 'd_project', 'id': M['id'], 'image': 'd_project.bin', 'load_addr': MOD_BUF, 'image_size': len(img),
            'sections': sections, 'prolog': M['entry'](*M['pro']), 'epilog': M['entry'](*M['epi']),
            'unresolved': M['entry'](*M['unr'])}
    json.dump(info, open(os.path.join(out, 'mod.json'), 'w'), indent=1)
    json.dump([[p, s, t, m] for p, s, t, m in self_relocs], open(os.path.join(out, 'mod_relocs.json'), 'w'))
    print('d_project linked at', hex(MOD_BUF), 'relocs per import:', {hex(k) if k > 4 else k: v for k, v in stats.items()})
    for s in sections: print(f"  {s['name']:6s} {s['addr']:08X} +{s['size']:#x}")
    print('  prolog', hex(info['prolog']))


if __name__ == '__main__':
    main()
