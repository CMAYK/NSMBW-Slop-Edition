"""Parser for Newer-Team's versions-nsmbw.txt address maps (P1 -> other versions)."""
import re
class VersionMap:
    def __init__(self, path):
        self.v = {}; cur = None
        for line in open(path, encoding='utf-8'):
            line = line.split('#')[0].strip()
            if not line: continue
            m = re.match(r'\[(\w+)\]', line)
            if m: cur = m.group(1); self.v[cur] = {'parent': None, 'rules': []}; continue
            if line.startswith('extend'): self.v[cur]['parent'] = line.split()[1]; continue
            m = re.match(r'([0-9a-fA-F]+)-([0-9a-fA-F]+|\*):\s*([+-])0x([0-9a-fA-F]+)', line)
            if m:
                lo = int(m.group(1), 16); hi = 0xFFFFFFFF if m.group(2) == '*' else int(m.group(2), 16)
                d = int(m.group(4), 16) * (1 if m.group(3) == '+' else -1)
                self.v[cur]['rules'].append((lo, hi, d))
    def chain(self, ver):
        c = []
        while ver: c.append(ver); ver = self.v[ver]['parent']
        return c[::-1]  # P1 first
    def port(self, addr, ver):
        """Port a P1 address to `ver`. Returns None when not portable."""
        for v in self.chain(ver)[1:]:
            for lo, hi, d in self.v[v]['rules']:
                if lo <= addr <= hi: addr += d; break
            else: return None
        return addr
