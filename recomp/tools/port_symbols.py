"""Port PAL v1 (P1) symbol names onto the dtk config for USA rev 2 (E2).

Sources (all P1 addresses): Newer-Team kamek_pal.x, NSMBW-Decomp syms.txt and alias_db.txt.
Only renames dtk's auto-generated names (fn_*, lbl_*, etc.); never overwrites a real name.
"""
import json, re, sys, os
from collections import Counter
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from nsmbw_versions import VersionMap
vm = VersionMap(os.path.join(HERE, 'versions-nsmbw.txt'))
MODS = {1: 'd_profileNP', 2: 'd_basesNP', 3: 'd_enemiesNP', 4: 'd_en_bossNP'}
SECORDER = ['.text', '.ctors', '.dtors', '.rodata', '.data']
AUTO = re.compile(r'^(fn|lbl|gap|jumptable|switch|@|\$|FUN|DAT)_?')
LINE = re.compile(r'^(\S+) = (\.[\w.]+):0x([0-9A-Fa-f]+);(.*)$')

# P1 layout of each REL: sections laid out contiguously from baseAddr (text first)
def rel_layout(name):
    meta = json.load(open(os.path.join(HERE, 'decomp_slices', name + '.json')))['meta']
    addr = int(meta['baseAddr'], 16); out = []
    for s in SECORDER:
        info = meta['sections'][s]; al = info['align'] or 1
        addr = (addr + al - 1) // al * al; sz = int(info['size'], 16)
        out.append((s, addr, sz)); addr += sz
    return out

def load_sources():
    syms = {}  # P1 abs addr -> name (first source wins)
    for l in open(os.path.join(HERE, 'decomp_syms_P1.txt')):
        m = re.match(r'\s*(\S+?)\s*=\s*0x([0-9A-Fa-f]+)', l)
        if m: syms.setdefault(int(m.group(2), 16), m.group(1))
    for l in open(os.path.join(HERE, 'kamek_pal.x')):
        m = re.match(r'\s*(\S+)\s*=\s*0x([0-9A-Fa-f]+);', l)
        if m: syms.setdefault(int(m.group(2), 16), m.group(1))
    rel_sec = {}  # (mod, P1 section-relative) names from alias_db
    secname = {1: '.text', 2: '.ctors', 3: '.dtors', 4: '.rodata', 5: '.data', 6: '.bss'}
    for l in open(os.path.join(HERE, 'decomp_alias_db_P1.txt')):
        m = re.match(r'R_(\d+)_(\d+)_([0-9a-f]+)\s*=\s*(\S+)', l)
        if m and int(m.group(1)) in MODS and int(m.group(2)) in secname:
            rel_sec[(int(m.group(1)), secname[int(m.group(2))], int(m.group(3), 16))] = m.group(4)
    return syms, rel_sec

def main():
    syms, rel_sec = load_sources()
    layouts = {mid: rel_layout(n) for mid, n in MODS.items()}
    # Resolve every source symbol to (config file, section, E2 address-in-that-file's-convention)
    targets = {}  # (cfgfile, sec, addr) -> name
    stats = Counter()
    def place(cfg, sec, addr, name):
        targets.setdefault((cfg, sec, addr), name)
    for a, n in syms.items():
        for mid, lay in layouts.items():
            for sec, start, sz in lay:
                if start <= a < start + sz:
                    p, ps = vm.port(a, 'E2'), vm.port(start, 'E2')
                    if p is None or ps is None: stats['unportable'] += 1
                    else: place(f'config/{MODS[mid]}/symbols.txt', sec, p - ps, n)
                    break
            else: continue
            break
        else:
            p = vm.port(a, 'E2')
            if p is None: stats['unportable'] += 1
            else: place('config/symbols.txt', None, p, n)
    for (mid, sec, off), n in rel_sec.items():
        lay = {s: (st, sz) for s, st, sz in layouts[mid]}
        if sec not in lay: continue
        st = lay[sec][0]; p, ps = vm.port(st + off, 'E2'), vm.port(st, 'E2')
        if p is None or ps is None: stats['unportable'] += 1; continue
        place(f'config/{MODS[mid]}/symbols.txt', sec, p - ps, n)
    # Apply
    report = []
    for cfg in ['config/symbols.txt'] + [f'config/{n}/symbols.txt' for n in MODS.values()]:
        path = os.path.join(ROOT, cfg); lines = open(path).read().splitlines()
        want = {(s, a): n for (c, s, a), n in targets.items() if c == cfg}
        used = set(); existing = {l.split(' = ')[0] for l in lines}
        for i, l in enumerate(lines):
            m = LINE.match(l)
            if not m: continue
            old, sec, addr = m.group(1), m.group(2), int(m.group(3), 16)
            key = (None, addr) if (None, addr) in want else (sec, addr)
            if key not in want: continue
            used.add(key); new = want[key]
            if old == new: stats['already'] += 1; continue
            if not AUTO.match(old): stats['kept_real_name'] += 1; continue
            if new in existing: stats['dup_name'] += 1; continue
            lines[i] = f'{new} = {sec}:0x{m.group(3)};{m.group(4)}'; existing.add(new); stats['renamed'] += 1
        stats['no_symbol_at_addr'] += len(want) - len(used)
        open(path, 'w').write('\n'.join(lines) + '\n')
    print(dict(stats))

if __name__ == '__main__':
    main()
