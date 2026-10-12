#!/usr/bin/env python3
"""Aggregate profile_samples.txt (raw RIPs) by function using nm on the binary."""
import sys, subprocess, bisect, collections
binary = sys.argv[1] if len(sys.argv) > 1 else 'out/nsmbw_clang'
syms = []
for l in subprocess.run(['nm', '-n', binary], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3 and p[1] in 'tTwW': syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]
c = collections.Counter()
samples = [int(x, 16) for x in open('profile_samples.txt')]
for s in samples:
    i = bisect.bisect_right(addrs, s) - 1
    c[syms[i][1] if i >= 0 else '?'] += 1
for n, k in c.most_common(30): print(f'{100 * k / len(samples):6.2f}%  {n}')
print(len(samples), 'samples')
