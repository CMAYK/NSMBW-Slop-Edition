#!/usr/bin/env python3
"""Download mkwcat's nsmbw-project release (MPL-2.0, https://github.com/mkwcat/nsmbw-project)
and unpack what the build needs into mod/:
  mod/mkwcat-nsmbw.arc     the asset archive (served to the game as /mkwcat.arc)
  mod/d_project.rel        the code module
  mod/d_project_E2.imp     its import table for USA rev 2
The build's patch tables (mod_patches.py) are tied to this exact release, so it is pinned.
"""
import hashlib, io, os, struct, sys, urllib.request, zipfile
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
TAG = 'v0.6.5'
URL = f'https://github.com/mkwcat/nsmbw-project/releases/download/{TAG}/mkwcat-nsmbw.zip'
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


def u8_files(d):
    """{path: bytes} of a U8 archive"""
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
        else: out[p] = d[a:a + b]
    return out


def main():
    md = os.path.join(ROOT, 'mod'); os.makedirs(md, exist_ok=True)
    zp = os.path.join(md, 'mkwcat-nsmbw.zip')
    if not os.path.exists(zp):
        print('downloading', URL)
        data = urllib.request.urlopen(URL).read()
        open(zp, 'wb').write(data)
    data = open(zp, 'rb').read()
    if hashlib.sha256(data).hexdigest() != SHA256: raise SystemExit(f'{zp}: unexpected file (want release {TAG})')
    arc = zipfile.ZipFile(io.BytesIO(data)).read('riivolution/mkwcat-nsmbw.arc')
    open(os.path.join(md, 'mkwcat-nsmbw.arc'), 'wb').write(arc)
    files = u8_files(arc)
    open(os.path.join(md, 'd_project.rel'), 'wb').write(yaz0(files['rels/d_project.rel.szs']))
    open(os.path.join(md, 'd_project_E2.imp'), 'wb').write(yaz0(files['rels/d_project_E2.imp.szs']))
    print(f'nsmbw-project {TAG}: mod/mkwcat-nsmbw.arc ({len(arc)} bytes), d_project.rel, d_project_E2.imp')


if __name__ == '__main__':
    main()
