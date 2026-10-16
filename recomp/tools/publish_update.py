#!/usr/bin/env python3
"""Publish a game update for players to download (docs/UPDATES.md).

    python tools/publish_update.py keygen
        Once: makes your signing key. The secret half goes in your user folder
        (~/.nsmbw_update_key - back it up, never share it); the public half goes in
        recomp/update_key.pub and is built into the game.

    python tools/publish_update.py publish --notes "what changed"
        Builds the update in recomp/out/update_site/ from the game folder (../game):
            nsmbw-<version>.zip the zipped build (also a complete download for new players:
                                unzip into a game folder, add the disc)
            latest.txt          the update list: version, the zip, each file's SHA-256, size and
                                place in the zip, and your signature
        The game downloads only the files that changed, straight out of the zip (HTTP ranges).
        Then it uploads both to the folder in recomp/update_url.txt (or --upload URL; --no-upload
        to skip) logged in as you: the zip first, latest.txt last, so players never see a list
        whose zip isn't there. build_windows.bat offers this after each build when VERSION is
        higher than the server's; build.py --publish does it without asking.

    python tools/publish_update.py login / logout
        Save (or forget) your copyparty login: ~/.nsmbw_upload_login, the password encrypted
        for your Windows account. The first publish also asks and offers to save it.

Before publishing: raise the number in recomp/VERSION and build (python build.py) - the game
only takes an update whose version is higher than its own.

Which files: recomp/update_files.txt (made with the defaults the first time), one pattern per
line relative to the game folder; "!pattern" leaves files out. Player data (nand/, settings.ini,
profiles.json, ...) and the disc are never published, whatever the list says.
"""
import argparse, fnmatch, hashlib, os, shutil, struct, sys, urllib.error, urllib.parse, urllib.request, zipfile, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.environ.get('NSMBW_GAME_DIR') or os.path.normpath(os.path.join(ROOT, '..', 'game'))
KEY_FILE = os.path.join(os.path.expanduser('~'), '.nsmbw_update_key')
PUB_FILE = os.path.join(ROOT, 'update_key.pub')
LIST_FILE = os.path.join(ROOT, 'update_files.txt')
DEFAULT_LIST = """# Files players get, relative to the game folder (patterns: * ? and folder/ for everything in it).
# Lines starting with ! leave files out. Player data and the disc are never published.
nsmbw.exe
orig/
mod/
ui/
!mod/*.zip
"""
# never published (the game never writes these from an update either: rt_update.c)
PROTECTED_DIRS = ('disc/', 'nand/', 'online/', 'update/', 'frames/')
PROTECTED_FILES = ('settings.ini', 'wiimotes.ini', 'online.ini', 'profiles.json', 'update.ini', 'shadercache.bin',
                   'nsmbw_log.txt', 'nsmbw_profile.txt', 'profile_samples.txt', 'nsmbw_new.exe')

# ------------------------------------------------------------------ Ed25519 (RFC 8032 section 6)
P = 2**255 - 19
L = 2**252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, P - 2, P) % P
SQRT_M1 = pow(2, (P - 1) // 4, P)


def _add(a, b):
    A, B = (a[1] - a[0]) * (b[1] - b[0]) % P, (a[1] + a[0]) * (b[1] + b[0]) % P
    C, E = 2 * a[3] * b[3] * D % P, 2 * a[2] * b[2] % P
    e, f, g, h = B - A, E - C, E + C, B + A
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def _mul(s, pt):
    q = (0, 1, 1, 0)
    while s > 0:
        if s & 1: q = _add(q, pt)
        pt = _add(pt, pt); s >>= 1
    return q


def _recover_x(y, sign):
    x2 = (y * y - 1) * pow(D * y * y + 1, P - 2, P) % P
    if x2 == 0: return None if sign else 0
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P: x = x * SQRT_M1 % P
    if (x * x - x2) % P: return None
    if (x & 1) != sign: x = P - x
    return x


_GY = 4 * pow(5, P - 2, P) % P
_GX = _recover_x(_GY, 0)
G = (_GX, _GY, 1, _GX * _GY % P)


def _compress(pt):
    zi = pow(pt[2], P - 2, P)
    x, y = pt[0] * zi % P, pt[1] * zi % P
    return int.to_bytes(y | ((x & 1) << 255), 32, 'little')


def _decompress(s):
    y = int.from_bytes(s, 'little'); sign = y >> 255; y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    return None if x is None else (x, y, 1, x * y % P)


def _expand(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], 'little'); a &= (1 << 254) - 8; a |= 1 << 254
    return a, h[32:]


def public_key(seed):
    return _compress(_mul(_expand(seed)[0], G))


def sign(seed, msg):
    a, prefix = _expand(seed)
    pub = _compress(_mul(a, G))
    r = int.from_bytes(hashlib.sha512(prefix + msg).digest(), 'little') % L
    R = _compress(_mul(r, G))
    h = int.from_bytes(hashlib.sha512(R + pub + msg).digest(), 'little') % L
    return R + int.to_bytes((r + h * a) % L, 32, 'little')


def verify(pub, msg, sig):
    A = _decompress(pub); R = _decompress(sig[:32]); s = int.from_bytes(sig[32:], 'little')
    if A is None or R is None or s >= L: return False
    h = int.from_bytes(hashlib.sha512(sig[:32] + pub + msg).digest(), 'little') % L
    sB, hA = _mul(s, G), _mul(h, A)
    rhs = _add(R, hA)
    return (sB[0] * rhs[2] - rhs[0] * sB[2]) % P == 0 and (sB[1] * rhs[2] - rhs[1] * sB[2]) % P == 0


# ------------------------------------------------------------------ helpers
def say(m): print(m, flush=True)


def fail(m):
    say('\nERROR: ' + m); sys.exit(1)


def load_seed():
    if not os.path.exists(KEY_FILE): fail('no signing key yet - run:  python tools/publish_update.py keygen')
    seed = bytes.fromhex(open(KEY_FILE).read().strip())
    if len(seed) != 32: fail('%s is not a key made by keygen' % KEY_FILE)
    return seed


def version_tuple(v):
    out = []
    for part in v.split('.'):
        n = ''.join(c for c in part if c.isdigit())
        out.append(int(n) if n else 0)
    return tuple(out)


def protected(rel):
    r = rel.lower()
    return r.startswith(PROTECTED_DIRS) or r in PROTECTED_FILES or r.endswith(('.old', '.part'))


def collect(game):
    if not os.path.exists(LIST_FILE):
        with open(LIST_FILE, 'w') as f: f.write(DEFAULT_LIST)
        say('made %s (which files players get) - edit it to change the list' % os.path.relpath(LIST_FILE, ROOT))
    inc, exc = [], []
    for line in open(LIST_FILE, encoding='utf-8'):
        line = line.strip().replace('\\', '/')
        if not line or line.startswith('#'): continue
        (exc if line.startswith('!') else inc).append(line.lstrip('!'))
    def match(rel, pats):
        for p in pats:
            if p.endswith('/') and rel.startswith(p): return True
            if fnmatch.fnmatchcase(rel, p): return True
        return False
    files = []
    for dp, dns, fns in os.walk(game):
        dns.sort()
        for fn in sorted(fns):
            rel = os.path.relpath(os.path.join(dp, fn), game).replace('\\', '/')
            if protected(rel) or not match(rel, inc) or match(rel, exc): continue
            if any(c in rel for c in '\n\r') or rel.startswith('/') or '..' in rel.split('/'): continue
            files.append(rel)
    return files


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''): h.update(chunk)
    return h.hexdigest()


# ------------------------------------------------------------------ commands
def cmd_keygen(a):
    if os.path.exists(KEY_FILE) and not a.force:
        seed = load_seed()
        say('you already have a signing key (%s)' % KEY_FILE)
    else:
        seed = os.urandom(32)
        with open(KEY_FILE, 'w') as f: f.write(seed.hex() + '\n')
        try: os.chmod(KEY_FILE, 0o600)
        except OSError: pass
        say('new signing key: %s\n  BACK THIS FILE UP and never share it. Without it you cannot publish updates\n'
            '  that existing copies of the game accept; anyone who has it can.' % KEY_FILE)
    pub = public_key(seed).hex()
    with open(PUB_FILE, 'w') as f: f.write(pub + '\n')
    say('public key -> %s (built into the game: rebuild with python build.py)' % os.path.relpath(PUB_FILE, ROOT))


def server_version(base, auth=None):
    """the version in latest.txt on the server ('' = none there yet)"""
    st, body = http('GET', base + 'latest.txt', auth=auth)
    if st == 200:
        for line in body.decode('utf-8', 'replace').splitlines():
            if line.startswith('version '): return line[8:].strip()
    elif st not in (404,): fail('the server answered HTTP %d for %slatest.txt' % (st, base))
    return ''


def cmd_publish(a):
    ver = open(os.path.join(ROOT, 'VERSION')).read().strip()
    target = '' if a.no_upload else (a.upload or server_folder())
    if target and not target.endswith('/'): target += '/'
    if a.ask:                                   # (build_windows.bat: offered after each build)
        why = ('no recomp\\update_url.txt' if not target else
               'no signing key (python tools/publish_update.py keygen)' if not os.path.exists(KEY_FILE) else '')
        if why: say('updates: not set up (%s) - nothing to publish' % why); return
        try: have = server_version(target)
        except Exception as e: say('updates: can\'t reach %s (%s) - not publishing' % (target, e)); return
        if have and version_tuple(have) >= version_tuple(ver):
            say('updates: the server already has version %s - raise recomp\\VERSION to publish this build' % have); return
        if not sys.stdin.isatty(): return
        if input('\nPublish version %s as an update for players%s? [y/N] ' % (ver, ' (the server has %s)' % have if have else '')).strip().lower() not in ('y', 'yes'):
            say('not published'); return
    seed = load_seed()
    pub = public_key(seed)
    if not os.path.exists(PUB_FILE) or open(PUB_FILE).read().strip().lower() != pub.hex():
        fail('update_key.pub does not match your signing key - run keygen again (it rewrites update_key.pub), then rebuild')
    auth = None
    if target:
        auth = get_login(a, target)
        have = server_version(target, auth)
        if have and version_tuple(have) >= version_tuple(ver):
            fail('the server already has version %s - raise recomp/VERSION (and rebuild) first' % have)
    if not a.notes and sys.stdin.isatty():
        a.notes = input('What changed in this version (one line, Enter to skip): ').strip()
    game = os.path.abspath(a.game)
    files = collect(game)
    if 'nsmbw.exe' not in files: say('warning: no nsmbw.exe in %s' % game)
    if not files: fail('no files to publish in %s' % game)
    out = os.path.abspath(a.out)
    if os.path.isdir(out): shutil.rmtree(out)
    os.makedirs(out)
    zname = 'nsmbw-%s.zip' % ''.join(c if c.isalnum() or c in '.-_' else '_' for c in ver)
    zpath = os.path.join(out, zname)
    say('packing %d files into %s ...' % (len(files), zname))
    hashes, total = {}, 0
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for rel in files:
            p = os.path.join(game, rel)
            hashes[rel] = sha256_file(p); total += os.path.getsize(p)
            z.write(p, rel)
    # where each file's data is in the zip: the game downloads just the files it needs (HTTP ranges)
    lines = ['nsmbw-update 2', 'version ' + ver]
    if a.notes: lines.append('notes ' + ' '.join(a.notes.split()))
    lines.append('zip %d %s' % (os.path.getsize(zpath), zname))
    with zipfile.ZipFile(zpath) as z, open(zpath, 'rb') as raw:
        for info in z.infolist():
            raw.seek(info.header_offset)
            hdr = raw.read(30)
            if hdr[:4] != b'PK\x03\x04': fail('zip writing went wrong (%s)' % info.filename)
            nlen, xlen = struct.unpack('<HH', hdr[26:30])
            off = info.header_offset + 30 + nlen + xlen
            if info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED): fail('unexpected compression for %s' % info.filename)
            raw.seek(off)
            data = raw.read(info.compress_size)
            plain = data if info.compress_type == zipfile.ZIP_STORED else zlib.decompress(data, -15)
            if hashlib.sha256(plain).hexdigest() != hashes[info.filename]: fail('zip check failed for %s' % info.filename)
            lines.append('file %s %d %d %d %d %s' % (hashes[info.filename], info.file_size, off, info.compress_size, info.compress_type, info.filename))
    body = ('\n'.join(lines) + '\n').encode('utf-8')
    sig = sign(seed, body)
    assert verify(pub, body, sig)
    with open(os.path.join(out, 'latest.txt'), 'wb') as f: f.write(body + b'sig ' + sig.hex().encode() + b'\n')
    say('version %s: %d files, %.1f MB -> %s (%.1f MB) + latest.txt in %s' % (ver, len(files), total / 1e6, zname, os.path.getsize(zpath) / 1e6, out))
    if target: upload(out, zname, target, auth, ver)
    else: say('put both on your server, in the folder the game looks in (latest.txt last) - or set recomp/update_url.txt and it uploads them')


def server_folder():
    """the folder in recomp/update_url.txt (the address built into the game), without ?query"""
    p = os.path.join(ROOT, 'update_url.txt')
    if not os.path.exists(p): return ''
    for line in open(p, encoding='utf-8'):
        line = line.strip()
        if line and not line.startswith('#'):
            line = line.split('?')[0]
            return line if line.endswith('/') else line.rsplit('/', 1)[0] + '/'
    return ''


# ------------------------------------------------------------------ your copyparty login
# Kept in your user folder (never in recomp/ or the game): ~/.nsmbw_upload_login. On Windows the
# password is encrypted for your Windows account (DPAPI: only you, on this PC, can read it).
LOGIN_FILE = os.path.join(os.path.expanduser('~'), '.nsmbw_upload_login')


def _dpapi(data, protect):
    import ctypes
    from ctypes import wintypes
    class BLOB(ctypes.Structure): _fields_ = [('cbData', wintypes.DWORD), ('pbData', ctypes.POINTER(ctypes.c_char))]
    buf = ctypes.create_string_buffer(data, len(data))
    inb, out = BLOB(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char))), BLOB()
    c32, k32 = ctypes.windll.crypt32, ctypes.windll.kernel32
    fn = c32.CryptProtectData if protect else c32.CryptUnprotectData
    fn.argtypes = [ctypes.POINTER(BLOB), ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(BLOB)]
    fn.restype = wintypes.BOOL
    if not fn(ctypes.byref(inb), None, None, None, None, 0, ctypes.byref(out)): raise OSError('DPAPI failed')
    res = ctypes.string_at(out.pbData, out.cbData)
    k32.LocalFree.argtypes = [ctypes.c_void_p]; k32.LocalFree(ctypes.cast(out.pbData, ctypes.c_void_p))
    return res


def load_login():
    try:
        user, kind, blob = open(LOGIN_FILE, encoding='utf-8').read().strip().split('\n')[:3]
        raw = bytes.fromhex(blob)
        return user, (_dpapi(raw, False) if kind == 'dpapi' else raw).decode('utf-8')
    except Exception:
        return None


def save_login(user, pw):
    kind, raw = 'plain', pw.encode('utf-8')
    if os.name == 'nt':
        try: kind, raw = 'dpapi', _dpapi(raw, True)
        except Exception as e: say('(couldn\'t encrypt it with Windows: %s - not saved)' % e); return
    with open(LOGIN_FILE, 'w', encoding='utf-8') as f: f.write('%s\n%s\n%s\n' % (user, kind, raw.hex()))
    try: os.chmod(LOGIN_FILE, 0o600)
    except OSError: pass
    say('login saved in %s' % LOGIN_FILE)


def forget_login():
    if os.path.exists(LOGIN_FILE): os.remove(LOGIN_FILE); say('forgot the saved copyparty login')


def access(base, auth):
    """(ok, problem): may this login write + delete in the folder? copyparty's ?ls tells who it
    thinks you are ("acct") and what you may do there ("perms"). A folder that doesn't exist yet
    answers 404: then its nearest existing parent counts (the upload makes the folder)."""
    import json
    who = auth[0] if auth else 'nobody'
    path, tried = base, []
    while True:
        try: st, body = http('GET', path + '?ls', auth=auth)
        except Exception as e: return False, 'can\'t reach %s (%s)' % (path, e)
        d = None
        if st == 200:
            try: d = json.loads(body.decode('utf-8'))
            except Exception: d = None
        if isinstance(d, dict) and 'perms' in d:
            acct, perms = d.get('acct', '?'), d.get('perms', [])
            where = path if path == base else '%s (%s doesn\'t exist yet, or this login can\'t see it)' % (path, base)
            if acct in ('*', '', None):
                return False, ('copyparty doesn\'t accept that login - it treats you as not logged in. Check the user name and '
                               'password (copyparty\'s [accounts] / -a user:password; restart copyparty after changing its config)')
            if 'write' in perms and 'delete' in perms: return True, ''
            return False, ('copyparty accepts the login (you are "%s"), but in %s you may only: %s. The account needs rwmd there '
                           '(in its volume\'s accs: "rwmd: %s"; restart copyparty after changing its config)' % (acct, where, ', '.join(perms) or 'nothing', acct))
        if st == 200 and d is None:
            return False, '%s doesn\'t answer like copyparty (no ?ls listing) - is the address right?' % path
        snippet = ' '.join(body[:120].decode('utf-8', 'replace').split()) if body else ''
        tried.append('%s -> HTTP %d%s' % (path + '?ls', st, (': ' + snippet) if snippet else ''))
        parts = urllib.parse.urlsplit(path)
        if parts.path in ('', '/'):
            return False, 'nothing on the way up answered as copyparty:\n  ' + '\n  '.join(tried)
        parent = parts.path.rstrip('/').rsplit('/', 1)[0] + '/'
        path = urllib.parse.urlunsplit((parts.scheme, parts.netloc, parent, '', ''))


def ask_login(base, user_default=''):
    import getpass
    say('Log in to copyparty (%s) as you:' % base)
    user = input('  user name%s: ' % (' [%s]' % user_default if user_default else '')).strip() or user_default
    pw = getpass.getpass('  password: ')
    return user, pw


def get_login(a, base):
    """your copyparty login, checked: it must be able to write and delete (replace latest.txt) there"""
    user = a.user or os.environ.get('NSMBW_UPLOAD_USER', '')
    pw = a.password or os.environ.get('NSMBW_UPLOAD_PW', '')
    saved = None
    if not pw:
        saved = load_login()
        if saved: user, pw = saved
    for attempt in range(3):
        if not pw:
            if not sys.stdin.isatty(): fail('no copyparty login (run: python tools/publish_update.py login)')
            user, pw = ask_login(base, user)
        ok, problem = access(base, (user, pw))
        if ok:
            if not saved and sys.stdin.isatty() and not (a.password or os.environ.get('NSMBW_UPLOAD_PW')):
                if input('Save this login for next time? [Y/n] ').strip().lower() in ('', 'y', 'yes'): save_login(user, pw)
            return (user, pw)
        say(problem)
        if 'doesn\'t accept that login' not in problem: fail('can\'t publish with that login')
        if saved: forget_login(); saved = None
        pw = ''
    fail('no working copyparty login')


def cmd_login(a):
    base = a.upload or server_folder()
    if not base: fail('no recomp/update_url.txt (or --upload URL)')
    if not base.endswith('/'): base += '/'
    forget_login()
    a.user = a.password = ''
    user, pw = ask_login(base)
    ok, problem = access(base, (user, pw))
    if not ok: fail(problem)
    save_login(user, pw)


def http(method, url, data=None, auth=None, replace=False):
    req = urllib.request.Request(url, data=data, method=method, headers={'User-Agent': 'nsmbw-publish/1'})   # (not "Python-urllib": proxies like Cloudflare block that)
    if auth:                                   # (copyparty takes "user:password" as HTTP basic auth)
        import base64
        req.add_header('Authorization', 'Basic ' + base64.b64encode(('%s:%s' % auth).encode('utf-8')).decode('ascii'))
    if replace: req.add_header('Replace', '1')       # copyparty: overwrite (PUT reads this header; needs delete rights)
    try:
        with urllib.request.urlopen(req, timeout=900) as r: return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def upload(out, zname, base, auth, ver):
    with open(os.path.join(out, zname), 'rb') as f: data = f.read()
    say('uploading %s (%.1f MB) as %s...' % (zname, len(data) / 1e6, auth[0] or 'you'))
    st, resp = http('PUT', base + zname, data, auth, replace=True)
    if st not in (200, 201): fail('upload of %s failed: HTTP %d %s' % (zname, st, resp[:200]))
    st, _ = http('HEAD', base + zname, auth=auth)
    if st != 200: fail('%s is not on the server after uploading it (HTTP %d)' % (zname, st))
    with open(os.path.join(out, 'latest.txt'), 'rb') as f: data = f.read()
    st, resp = http('PUT', base + 'latest.txt', data, auth, replace=True)
    if st not in (200, 201): fail('upload of latest.txt failed: HTTP %d %s' % (st, resp[:200]))
    st, body = http('GET', base + 'latest.txt')                  # (as a player sees it: no login)
    if st != 200 or body != data: fail('latest.txt on the server is not the one just uploaded (HTTP %d without a login) - can everyone read %s?' % (st, base))
    say('published version %s. Older nsmbw-*.zip files on the server can be deleted once nobody is mid-download.' % ver)


def main():
    ap = argparse.ArgumentParser(description='Publish a game update (docs/UPDATES.md)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    k = sub.add_parser('keygen', help='make your signing key (once)')
    k.add_argument('--force', action='store_true', help='make a new key even if you have one (old copies of the game will refuse updates signed with it)')
    p = sub.add_parser('publish', help='build (and upload) the update')
    p.add_argument('--game', default=GAME, help='the game folder to publish from (default ../game)')
    p.add_argument('--notes', default='', help='a line about what changed (shown while downloading)')
    p.add_argument('--out', default=os.path.join(ROOT, 'out', 'update_site'), help='where to build it')
    p.add_argument('--upload', default='', help='copyparty folder to upload to (default: the address in recomp/update_url.txt)')
    p.add_argument('--no-upload', action='store_true', help='only build it in --out')
    p.add_argument('--ask', action='store_true', help='only if the server has an older version, and after asking (build_windows.bat)')
    p.add_argument('--user', default='', help='copyparty user name (default: the saved login, NSMBW_UPLOAD_USER, or asked)')
    p.add_argument('--password', default='', help='copyparty password (default: the saved login, NSMBW_UPLOAD_PW, or asked)')
    l = sub.add_parser('login', help='save your copyparty login (asks; checks it can write + delete there)')
    l.add_argument('--upload', default='', help='the folder (default: recomp/update_url.txt)')
    sub.add_parser('logout', help='forget the saved copyparty login')
    a = ap.parse_args()
    {'keygen': cmd_keygen, 'publish': cmd_publish, 'login': cmd_login, 'logout': lambda a: forget_login()}[a.cmd](a)


if __name__ == '__main__':
    main()
