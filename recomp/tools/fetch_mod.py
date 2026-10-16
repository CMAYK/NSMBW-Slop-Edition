#!/usr/bin/env python3
"""Get the 8-player module's release (MPL-2.0; where from: THIRD_PARTY_NOTICES.md, "Release:")
and unpack what the build needs into mod/:
  mod/upstream-release.zip  the release as downloaded (checked against SHA256)
  mod/nsmbw-8p.arc          the asset archive (served to the game as /nsmbw-8p.arc); its
                            Info/Version.json names the project "nsmbw-pc"
  mod/d_project.rel         the code module
  mod/d_project_E2.imp      its import table for USA rev 2
The build's patch tables (mod_patches.py) are tied to this exact release, so it is pinned.
A release zip already in mod/ under another name is used if it has the right checksum.
"""
import glob, hashlib, io, json, os, re, struct, sys, urllib.request, zipfile
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
TAG = 'v0.6.5'
ARC = 'nsmbw-8p.arc'
PROJECT_NAME = 'nsmbw-pc'
SHA256 = '9f731f1012878b32ada6b47d0e99ad32f25e93672be315875d4405263cceeea3'


def yaz0(d):
    assert d[:4] == b'Yaz0'
    n = struct.unpack('>I', d[4:8])[0]; out = bytearray(); s = 16
    while len(out) < n:
        c = d[s]; s += 1
        for b in range(8):
            if len(out) >= n: break
            if c & (0x80 >> b): out.append(d[s]); s += 1
            else:
                x = d[s] << 8 | d[s + 1]; s += 2
                dist = (x & 0xFFF) + 1; ln = x >> 12
                if ln == 0: ln = d[s] + 0x12; s += 1
                else: ln += 2
                for _ in range(ln): out.append(out[-dist])
    return bytes(out)


def release_url():
    notes = open(os.path.join(ROOT, 'THIRD_PARTY_NOTICES.md'), encoding='utf-8').read()
    m = re.search(r'^- Release: (\S+)$', notes, re.M)
    if not m: raise SystemExit('THIRD_PARTY_NOTICES.md: no "- Release:" line')
    return m.group(1)


def u8_files(d, offsets=False):
    """{path: bytes} of a U8 archive ({path: (offset, size)} with offsets=True)"""
    assert d[:4] == b'\x55\xAA\x38\x2D'
    root = struct.unpack('>I', d[4:8])[0]
    n = struct.unpack_from('>I', d, root + 8)[0]; st = root + n * 12
    name = lambda o: d[st + o:d.index(b'\0', st + o)].decode()
    out, ends = {}, [(n, '')]
    for i in range(1, n):
        while ends and i >= ends[-1][0]: ends.pop()
        cur = ends[-1][1] if ends else ''
        t, a, b = struct.unpack_from('>III', d, root + i * 12)
        p = (cur + '/' if cur else '') + name(t & 0xFFFFFF)
        if t >> 24: ends.append((b, p))
        else: out[p] = (a, b) if offsets else d[a:a + b]
    return out


def rename_project(arc):
    """Info/Version.json is {"<project>": "<version>"}: the project gets this port's name (the
    file keeps its size, padded with spaces, so the archive needn't be rebuilt)"""
    arc = bytearray(arc)
    off, size = u8_files(bytes(arc), offsets=True)['Info/Version.json']
    v = json.loads(bytes(arc[off:off + size]).decode('utf-8'))
    new = json.dumps({PROJECT_NAME: list(v.values())[0]}).encode()
    if len(new) <= size: arc[off:off + size] = new + b' ' * (size - len(new))
    return bytes(arc)


def main():
    md = os.path.join(ROOT, 'mod'); os.makedirs(md, exist_ok=True)
    zp = os.path.join(md, 'upstream-release.zip')
    if not os.path.exists(zp):                       # (a copy under an older name?)
        for z in glob.glob(os.path.join(md, '*.zip')):
            if hashlib.sha256(open(z, 'rb').read()).hexdigest() == SHA256:
                open(zp, 'wb').write(open(z, 'rb').read()); break
    if not os.path.exists(zp):
        url = release_url()
        print('downloading', url)
        open(zp, 'wb').write(urllib.request.urlopen(url).read())
    data = open(zp, 'rb').read()
    if hashlib.sha256(data).hexdigest() != SHA256: raise SystemExit(f'{zp}: unexpected file (want release {TAG})')
    zf = zipfile.ZipFile(io.BytesIO(data))
    arc = zf.read([n for n in zf.namelist() if n.endswith('.arc')][0])
    files = u8_files(arc)
    open(os.path.join(md, ARC), 'wb').write(rename_project(arc))
    open(os.path.join(md, 'd_project.rel'), 'wb').write(yaz0(files['rels/d_project.rel.szs']))
    open(os.path.join(md, 'd_project_E2.imp'), 'wb').write(yaz0(files['rels/d_project_E2.imp.szs']))
    print(f'8-player module {TAG}: mod/{ARC} ({len(arc)} bytes), d_project.rel, d_project_E2.imp')


if __name__ == '__main__':
    main()
