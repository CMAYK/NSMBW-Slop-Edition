#!/usr/bin/env python3
"""Names for the game functions the 8-player code replaces, taken from its source.

The 8-player module's source marks every game function it replaces or calls with [[nsmbw(0xADDR)]]
(PAL v1 addresses) right before the C++ definition/declaration. This ports those addresses to
USA rev 2 (tools/versions-nsmbw.txt) and writes linked/mod_source_names.json:
    {"80918790": {"name": "dScGameSetup_c::executeState_FileSelect", "file": "wiimj2d/.../d_s_GameSetup.cpp"}}
Usage: tools/mod_source_names.py <path to a checkout of its source at tag v0.6.5>
(where to get it: THIRD_PARTY_NOTICES.md, "Source code:")
"""
import json, os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from nsmbw_versions import VersionMap

ANN = re.compile(r'^\s*\[\[nsmbw\(0x([0-9A-Fa-f]+)\)\]\]\s*$')
NAME = re.compile(r'([A-Za-z_~][\w:~<>]*(?:operator\s*\S+)?)\s*\(')


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, '..', '..', 'nsmbw-project')
    src = os.path.join(src, 'source')
    vm = VersionMap(os.path.join(HERE, 'versions-nsmbw.txt'))
    out = {}
    for dp, _, fs in os.walk(src):
        for f in fs:
            if not f.endswith(('.cpp', '.h', '.c')): continue
            path = os.path.join(dp, f); lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
            for i, l in enumerate(lines):
                m = ANN.match(l)
                if not m: continue
                text = ' '.join(lines[i + 1:i + 6])
                n = NAME.search(text)
                if not n: continue
                name = n.group(1)
                if name in ('if', 'for', 'while', 'switch', 'return', 'sizeof'): continue
                # a member declared inside its class body has no "Class::" - find the class
                if '::' not in name:
                    for j in range(i, -1, -1):
                        c = re.match(r'\s*(?:class|struct)\s+(\w+)\b[^;]*$', lines[j])
                        if c: name = c.group(1) + '::' + name; break
                        if re.match(r'^(namespace|\}\s*;?\s*$)', lines[j]) and j < i - 1 and lines[j].startswith('}'): break
                e2 = vm.port(int(m.group(1), 16), 'E2')
                if e2 is None: continue
                key = f'{e2:08X}'
                if key not in out or path.endswith('.cpp'):
                    out[key] = {'name': name, 'file': os.path.relpath(path, src)}
    dst = os.path.join(ROOT, 'linked', 'mod_source_names.json')
    json.dump(out, open(dst, 'w'), indent=0, sort_keys=True)
    print(f'{len(out)} names -> {dst}')


if __name__ == '__main__':
    main()
