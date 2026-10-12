#!/usr/bin/env python3
"""Statically link NSMBW's REL modules at their real in-game addresses (USA rev 2).

Load addresses: each module's text address is the PAL v1 address from NSMBW-Decomp
ported through Newer's versions-nsmbw.txt map. Every other section sits at
buffer + file offset (the game loads the REL file as one blob), and .bss sits at
buffer + fixSize (OSLinkFixed reuses the relocation area), which reproduces the
exact back-to-back heap layout the real game ends up with.

Outputs linked/<module>.bin (the patched file image starting at its buffer address)
and linked/manifest.json describing every section.
"""
import json, os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from nsmbw_versions import VersionMap

MODS = ['d_profileNP', 'd_basesNP', 'd_enemiesNP', 'd_en_bossNP']
SECNAMES = {1: '.text', 2: '.ctors', 3: '.dtors', 4: '.rodata', 5: '.data', 6: '.bss'}

R_PPC_ADDR32, R_PPC_ADDR24, R_PPC_ADDR16, R_PPC_ADDR16_LO, R_PPC_ADDR16_HI, R_PPC_ADDR16_HA = 1, 2, 3, 4, 5, 6
R_PPC_ADDR14, R_PPC_ADDR14_BRTAKEN, R_PPC_ADDR14_BRNTAKEN = 7, 8, 9
R_PPC_REL24, R_PPC_REL14, R_PPC_REL14_BRTAKEN, R_PPC_REL14_BRNTAKEN = 10, 11, 12, 13
R_DOLPHIN_NOP, R_DOLPHIN_SECTION, R_DOLPHIN_END = 201, 202, 203


class Rel:
    def __init__(self, name, path, text_addr):
        self.name = name
        self.data = bytearray(open(path, 'rb').read())
        d = self.data
        (self.id, _, _, ns, so, _, _, self.ver, self.bss_size, self.rel_off, self.imp_off, self.imp_size,
         self.prolog_sec, self.epilog_sec, self.unres_sec, self.bss_sec, self.prolog, self.epilog, self.unres) = \
            struct.unpack('>IIIIIIIIIIIIBBBBIII', d[:0x40])
        self.align, self.bss_align, self.fix_size = struct.unpack('>III', d[0x40:0x4C])
        self.secs = []  # (index, file_off, size, exec)
        for i in range(ns):
            off, sz = struct.unpack_from('>II', d, so + 8 * i)
            self.secs.append((i, off & ~1, sz, bool(off & 1)))
        text_off = self.secs[1][1]
        self.buf = text_addr - text_off
        self.bss_addr = (self.buf + self.fix_size + self.bss_align - 1) // self.bss_align * self.bss_align

    def sec_addr(self, i):
        _, off, sz, _ = self.secs[i]
        if off: return self.buf + off
        if sz: return self.bss_addr  # the one zero-offset section with a size is .bss
        return None


def link(mods, dol_bss_end):
    by_id = {m.id: m for m in mods}
    stats = {}
    for m in mods:
        st = {'relocs': 0, 'by_type': {}, 'unknown_module': 0}
        d = m.data
        for k in range(m.imp_size // 8):
            imp_mod, rel_off = struct.unpack_from('>II', d, m.imp_off + 8 * k)
            if imp_mod != 0 and imp_mod not in by_id: st['unknown_module'] += 1; continue
            p = rel_off; cur_sec = None; cur = 0
            while True:
                off, typ, sec, add = struct.unpack_from('>HBBI', d, p); p += 8
                cur += off
                if typ == R_DOLPHIN_END: break
                if typ == R_DOLPHIN_SECTION: cur_sec = sec; cur = 0; continue
                if typ == R_DOLPHIN_NOP: continue
                S = add if imp_mod == 0 else by_id[imp_mod].sec_addr(sec) + add
                S &= 0xFFFFFFFF
                fo = m.secs[cur_sec][1] + cur          # where to patch, in the file image
                P = m.buf + fo                         # its guest address
                st['relocs'] += 1; st['by_type'][typ] = st['by_type'].get(typ, 0) + 1
                if typ == R_PPC_ADDR32: struct.pack_into('>I', d, fo, S)
                elif typ == R_PPC_ADDR24:
                    w = struct.unpack_from('>I', d, fo)[0]; struct.pack_into('>I', d, fo, (w & ~0x03FFFFFC) | (S & 0x03FFFFFC))
                elif typ in (R_PPC_ADDR16, R_PPC_ADDR16_LO): struct.pack_into('>H', d, fo, S & 0xFFFF)
                elif typ == R_PPC_ADDR16_HI: struct.pack_into('>H', d, fo, S >> 16)
                elif typ == R_PPC_ADDR16_HA: struct.pack_into('>H', d, fo, ((S + 0x8000) >> 16) & 0xFFFF)
                elif typ == R_PPC_REL24:
                    w = struct.unpack_from('>I', d, fo)[0]; struct.pack_into('>I', d, fo, (w & ~0x03FFFFFC) | ((S - P) & 0x03FFFFFC))
                elif typ in (R_PPC_REL14, R_PPC_REL14_BRTAKEN, R_PPC_REL14_BRNTAKEN):
                    w = struct.unpack_from('>I', d, fo)[0]; struct.pack_into('>I', d, fo, (w & ~0xFFFC) | ((S - P) & 0xFFFC))
                elif typ in (R_PPC_ADDR14, R_PPC_ADDR14_BRTAKEN, R_PPC_ADDR14_BRNTAKEN):
                    w = struct.unpack_from('>I', d, fo)[0]; struct.pack_into('>I', d, fo, (w & ~0xFFFC) | (S & 0xFFFC))
                else: raise SystemExit(f'{m.name}: unhandled relocation type {typ}')
        stats[m.name] = st
    # overlap checks
    spans = sorted([(m.buf, m.buf + m.fix_size, m.name + ' image') for m in mods] +
                   [(m.bss_addr, m.bss_addr + m.bss_size, m.name + ' bss') for m in mods])
    if spans[0][0] < dol_bss_end: raise SystemExit('module overlaps main.dol')
    for a, b in zip(spans, spans[1:]):
        if a[1] > b[0]: raise SystemExit(f'overlap: {a} {b}')
    return stats


def main():
    vm = VersionMap(os.path.join(HERE, 'versions-nsmbw.txt'))
    mods = []
    for n in MODS:
        p1 = int(json.load(open(os.path.join(HERE, 'decomp_slices', n + '.json')))['meta']['baseAddr'], 16)
        mods.append(Rel(n, os.path.join(ROOT, 'orig', n + '.rel'), vm.port(p1, 'E2')))
    d = open(os.path.join(ROOT, 'orig', 'main.dol'), 'rb').read()
    bss, bss_sz = struct.unpack('>II', d[0xD8:0xE0])
    stats = link(mods, bss + bss_sz)
    out = os.path.join(ROOT, 'linked'); os.makedirs(out, exist_ok=True)
    manifest = []
    for m in mods:
        img = bytes(m.data[:m.fix_size])
        open(os.path.join(out, m.name + '.bin'), 'wb').write(img)
        secs = []
        for i, off, sz, ex in m.secs:
            if i in SECNAMES and sz:
                secs.append({'name': SECNAMES[i], 'addr': m.sec_addr(i), 'size': sz,
                             'kind': 'bss' if off == 0 else ('text' if ex else 'data'),
                             'file_off': off})
        entry = lambda s, o: m.sec_addr(s) + o if s else 0
        manifest.append({'name': m.name, 'id': m.id, 'image': m.name + '.bin', 'load_addr': m.buf,
                         'image_size': len(img), 'sections': secs,
                         'prolog': entry(m.prolog_sec, m.prolog), 'epilog': entry(m.epilog_sec, m.epilog),
                         'unresolved': entry(m.unres_sec, m.unres)})
        print(f"{m.name:12s} id={m.id} load={m.buf:08X} text={m.sec_addr(1):08X} bss={m.bss_addr:08X}+{m.bss_size:#x} "
              f"relocs={stats[m.name]['relocs']} types={dict(sorted(stats[m.name]['by_type'].items()))}")
    json.dump(manifest, open(os.path.join(out, 'manifest.json'), 'w'), indent=1)
    with open(os.path.join(out, 'load.txt'), 'w') as f:  # simple form for the C runtime
        for e in manifest:
            f.write(f"{e['name']} {e['image']} {e['load_addr']:08X} {e['prolog']:08X} {e['epilog']:08X}\n")


if __name__ == '__main__':
    main()
