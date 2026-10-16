#!/usr/bin/env python3
"""Build the game (Windows exe) and put it in ../game - works on Windows, Linux and macOS.

    python build.py            build what changed, then deploy to ../game
    python build.py --clean    recompile everything
    python build.py --jobs 4   limit parallel compiles (default: CPU cores, fewer if memory is short)

Needs Python 3.8+ and zig's compiler as a Python package:  python -m pip install ziglang
(On Windows you can also just double-click build_windows.bat.)

Steps:
 1. generate the C code in out/ from the game + the 8-player module (skipped when nothing changed)
 2. compile out/recomp_*.c (slow the first time: ~100 big files; later only what changed)
 3. compile runtime/*.c, link nsmbw.exe
 4. copy it to ../game/nsmbw.exe with what the game needs next to it
"""
import argparse, concurrent.futures, hashlib, os, shutil, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.abspath(__file__))
GAME = os.environ.get('NSMBW_GAME_DIR') or os.path.normpath(os.path.join(ROOT, '..', 'game'))
OBJ = os.path.join(ROOT, 'out', 'objwin')
RUNTIME = ['rt_core', 'rt_bootscreen', 'rt_hw', 'rt_ios', 'rt_disc', 'rt_gx', 'rt_gxrender', 'rt_bt', 'rt_wiimote', 'rt_remotes', 'rt_net', 'rt_det', 'rt_state', 'rt_profiles', 'rt_crypto', 'rt_update', 'rt_si', 'rt_threads',
           'rt_ax', 'rt_hdtex', 'rt_aspect', 'rt_menu', 'rt_gamesetup', 'rt_title', 'rt_lyt', 'rt_mod', 'smb1_physics', 'rt_retro', 'rt_sfx', 'rt_chars', 'rt_charmdl', 'rt_discord', 'rt_dvd', 'rt_plat_win']
ZCC = [sys.executable, '-m', 'ziglang', 'cc', '-target', 'x86_64-windows-gnu']
FLAGS = ['-std=gnu11', '-O2', '-fno-stack-protector', '-march=x86_64_v3', '-ffp-contract=off', '-w', '-Iruntime', '-Iout']


def say(msg): print(msg, flush=True)


def fail(msg):
    say('\nBUILD FAILED: ' + msg)
    sys.exit(1)


def check_zig():
    try:
        subprocess.run(ZCC[:3] + ['version'], check=True, capture_output=True, cwd=ROOT)
    except (subprocess.CalledProcessError, FileNotFoundError):
        fail('zig is not installed for this Python. Run:\n    "%s" -m pip install ziglang' % sys.executable)


def generate():
    """tools/gen.sh in Python: regenerate out/ when an input changed"""
    if not all(os.path.exists(os.path.join(ROOT, 'mod', f)) for f in ('d_project.rel', 'd_project_E2.imp', 'nsmbw-8p.arc')):
        subprocess.run([sys.executable, 'tools/fetch_mod.py'], check=True, cwd=ROOT)
    inputs = ['tools/recomp.py', 'tools/link_mod.py', 'tools/mod_patches.py', 'tools/mod_funcs.py', 'runtime/hle_funcs.txt',
              'orig/main.dol', 'linked/manifest.json', 'mod/d_project.rel', 'mod/d_project_E2.imp', 'config/symbols.txt']
    for d in sorted(os.listdir(os.path.join(ROOT, 'linked'))):
        if d.endswith('NP.bin'): inputs.append('linked/' + d)
    for d in sorted(os.listdir(os.path.join(ROOT, 'config'))):
        if d.startswith('d_') and d.endswith('NP'): inputs.append('config/%s/symbols.txt' % d)
    h = hashlib.md5()
    for f in inputs:
        p = os.path.join(ROOT, f)
        if os.path.exists(p):
            with open(p, 'rb') as fh: h.update(fh.read())
    stamp = h.hexdigest()
    sp = os.path.join(ROOT, 'out', '.gen_stamp_py')
    if os.path.exists(sp) and open(sp).read().strip() == stamp and os.path.exists(os.path.join(ROOT, 'out', 'recomp_table.c')):
        say('[1/4] generated code is up to date')
        return
    say('[1/4] generating C code (about a minute)...')
    os.makedirs(os.path.join(ROOT, 'out'), exist_ok=True)
    for tool in ['link_mod.py', 'mod_patches.py', 'mod_funcs.py', 'recomp.py']:
        r = subprocess.run([sys.executable, 'tools/' + tool], cwd=ROOT, capture_output=True, text=True, encoding='utf-8', errors='replace')
        if r.returncode: fail('tools/%s:\n%s%s' % (tool, r.stdout[-2000:], r.stderr[-2000:]))
    open(sp, 'w').write(stamp)


def compile_one(src, obj):
    r = subprocess.run(ZCC + FLAGS + ['-c', src, '-o', obj], cwd=ROOT, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode:
        if os.path.exists(obj): os.remove(obj)
        return src, r.stderr[-3000:]
    return src, None


def compile_all(srcs, jobs, label):
    todo = []
    for src in srcs:
        obj = os.path.join(OBJ, os.path.basename(src)[:-2] + ('.c.o' if 'recomp_' in src else '.o'))
        if os.path.exists(obj) and os.path.getmtime(obj) >= os.path.getmtime(src) and \
           (src.startswith('out') or os.path.getmtime(obj) >= newest_header): continue
        todo.append((src, obj))
    if not todo:
        say('%s: up to date' % label)
        return
    say('%s: compiling %d file(s) on %d core(s)%s' % (label, len(todo), jobs, ' - the first build takes a while' if len(todo) > 20 else ''))
    t0, done = time.time(), 0
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        for src, err in ex.map(lambda t: compile_one(*t), todo):
            done += 1
            if err: fail('%s:\n%s' % (src, err))
            say('    [%d/%d] %s (%.0f s)' % (done, len(todo), os.path.basename(src), time.time() - t0))


def write_over(src, dst):
    """copy into place without renaming (renames over a shared folder leave .fuse_hidden files)"""
    with open(src, 'rb') as a, open(dst, 'wb') as b: shutil.copyfileobj(a, b)


def same(a, b):
    return os.path.exists(b) and os.path.getsize(a) == os.path.getsize(b) and open(a, 'rb').read() == open(b, 'rb').read()


def deploy(exe):
    say('[4/4] deploying to %s' % GAME)
    for d in ['', 'orig', 'nand', 'mod', 'ui']: os.makedirs(os.path.join(GAME, d), exist_ok=True)
    try:
        write_over(exe, os.path.join(GAME, 'nsmbw.exe'))
        say('    game\\nsmbw.exe updated')
    except PermissionError:
        alt = os.path.join(GAME, 'nsmbw_new.exe')
        write_over(exe, alt)
        say('    game\\nsmbw.exe is in use (game running?) - wrote game\\nsmbw_new.exe instead; close the game and rename it')
    for src, dst in [('orig/main.dol', 'orig/main.dol'), ('linked/d_project.bin', 'orig/d_project.bin'),
                     ('mod/nsmbw-8p.arc', 'mod/nsmbw-8p.arc')]:
        s, d = os.path.join(ROOT, src), os.path.join(GAME, dst)
        if os.path.exists(s) and not same(s, d): write_over(s, d)
    # character models / portraits from the mods in game\mod (zips): extracted when a zip is newer
    md = os.path.join(GAME, 'mod')
    zips = [os.path.join(md, f) for f in os.listdir(md) if f.lower().endswith('.zip')]
    stamp = os.path.join(md, 'characters', '.imported')
    if zips and (not os.path.exists(stamp) or max(os.path.getmtime(z) for z in zips) > os.path.getmtime(stamp)):
        if subprocess.run([sys.executable, 'tools/import_char_mods.py', '--game', GAME], cwd=ROOT).returncode:
            say('    (character mods: import failed - see above)')
        os.makedirs(os.path.dirname(stamp), exist_ok=True); open(stamp, 'w').close()
    for dp, _, fs in os.walk(os.path.join(ROOT, 'ui')):          # ui/ with subfolders (retro sprite sheets...)
        if os.sep + 'guide' in dp: continue
        for f in fs:
            if not f.endswith(('.png', '.json', '.wav')): continue
            s = os.path.join(dp, f); d = os.path.join(GAME, os.path.relpath(s, ROOT))
            os.makedirs(os.path.dirname(d), exist_ok=True)
            if same(s, d): continue
            if os.path.exists(d) and os.path.getmtime(d) > os.path.getmtime(s):   # edited in game\ui: keep it
                say('    kept game\\%s (newer than recomp\\%s - copy it there to keep it for good)' % (os.path.relpath(d, GAME), os.path.relpath(s, ROOT)))
                continue
            write_over(s, d)
    if not os.path.exists(os.path.join(GAME, 'nand', 'shared2', 'sys', 'SYSCONF')):     # console settings, once
        subprocess.run([sys.executable, 'tools/make_nand.py', '--out', os.path.join(GAME, 'nand')], check=True, cwd=ROOT)
    disc = os.path.join(GAME, 'disc')
    ext = os.path.normpath(os.path.join(ROOT, '..', 'extracted'))
    if not os.path.exists(os.path.join(disc, 'sys', 'main.dol')) and os.path.isdir(os.path.join(ext, 'sys')):
        say('    copying the extracted disc into game\\disc (one time)')
        for d in ['sys', 'files']: shutil.copytree(os.path.join(ext, d), os.path.join(disc, d), dirs_exist_ok=True)


newest_header = 0


def ram_gb():
    try:
        if os.name == 'nt':
            import ctypes
            class MS(ctypes.Structure):
                _fields_ = [('len', ctypes.c_ulong), ('load', ctypes.c_ulong), ('total', ctypes.c_ulonglong), ('avail', ctypes.c_ulonglong),
                            ('tp', ctypes.c_ulonglong), ('ap', ctypes.c_ulonglong), ('tv', ctypes.c_ulonglong), ('av', ctypes.c_ulonglong), ('x', ctypes.c_ulonglong)]
            m = MS(); m.len = ctypes.sizeof(MS)
            ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
            return m.total / 2**30
        return os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES') / 2**30
    except Exception:
        return 8.0


def default_jobs():
    """all cores, but the big generated files take up to ~1.3 GB each to compile"""
    return max(1, min(os.cpu_count() or 4, int(ram_gb() / 1.6)))


def version_ok(v):
    parts = v.split('.')
    return 1 <= len(parts) <= 4 and all(p.isdigit() and len(p) <= 4 for p in parts)


def set_version(v):
    if not version_ok(v): fail('"%s" is not a version number (like 0.2.0)' % v)
    with open(os.path.join(ROOT, 'VERSION'), 'w') as f: f.write(v + '\n')
    say('version set to %s' % v)


def ask_version():
    """build_windows.bat: the version number for this build (Enter keeps it, + the next one)"""
    if not sys.stdin.isatty(): return
    p = os.path.join(ROOT, 'VERSION')
    cur = open(p).read().strip() if os.path.exists(p) else '0.0.0'
    server = ''
    try:                                                    # (what players have now, if updates are set up)
        sys.path.insert(0, os.path.join(ROOT, 'tools'))
        import publish_update, urllib.request
        base = publish_update.server_folder()
        if base:
            req = urllib.request.Request(base + 'latest.txt', headers={'User-Agent': 'nsmbw-build/1'})
            with urllib.request.urlopen(req, timeout=4) as r:
                for line in r.read().decode('utf-8', 'replace').splitlines():
                    if line.startswith('version '): server = line[8:].strip()
    except Exception:
        pass
    nums = [int(x) for x in cur.split('.')] if version_ok(cur) else [0, 0, 0]
    while len(nums) < 3: nums.append(0)
    nxt = '.'.join(map(str, nums[:-1] + [nums[-1] + 1]))
    say('\nVersion of this build: %s%s' % (cur, ('   (players have %s)' % server) if server else ''))
    while True:
        v = input('  Enter = keep %s,  + = %s,  or type a version: ' % (cur, nxt)).strip()
        if not v: return
        if v == '+': v = nxt
        if version_ok(v): break
        say('  "%s" is not a version number (like 0.2.0)' % v)
    if v != cur: set_version(v)


def main():
    global newest_header
    ap = argparse.ArgumentParser(description='Build nsmbw.exe and deploy it to ../game')
    ap.add_argument('--jobs', '-j', type=int, default=0, help='parallel compiles (default: cores, limited by memory)')
    ap.add_argument('--clean', action='store_true', help='recompile everything')
    ap.add_argument('--no-deploy', action='store_true', help='only build (exe left in out/nsmbw.exe)')
    ap.add_argument('--publish', action='store_true', help='then publish it as an update (tools/publish_update.py; raise VERSION first)')
    ap.add_argument('--notes', default='', help='with --publish: what changed (one line)')
    ap.add_argument('--ask-publish', action='store_true', help='after building, offer to publish when VERSION is higher than the server\'s (build_windows.bat)')
    ap.add_argument('--ask-version', action='store_true', help='before building, ask for the version number (build_windows.bat)')
    ap.add_argument('--version', default='', help='set the version number (written to VERSION) before building')
    a = ap.parse_args()
    os.chdir(ROOT)
    if a.version: set_version(a.version)
    elif a.ask_version: ask_version()
    if a.jobs <= 0: a.jobs = default_jobs()
    t0 = time.time()
    check_zig()
    generate()
    if a.clean and os.path.isdir(OBJ): shutil.rmtree(OBJ)
    os.makedirs(OBJ, exist_ok=True)
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import version_header                                         # out/rt_version.h: VERSION, update server + key
    ver = version_header.write()
    url = version_header.first_line('update_url.txt'); key = version_header.first_line('update_key.pub')
    say('    version %s - updates %s' % (ver, 'from ' + url.split('?')[0] if url and key else
        'OFF in this build (%s)' % ' and '.join(x for x, ok in (('no recomp\\update_url.txt', url), ('no update key: python tools/publish_update.py keygen', key)) if not ok)))
    rt = os.path.join(ROOT, 'runtime')
    newest_header = max(os.path.getmtime(os.path.join(rt, f)) for f in os.listdir(rt) if f.endswith(('.h', '.inc')))
    newest_header = max(newest_header, os.path.getmtime(os.path.join(ROOT, 'out', 'recomp_funcs.h')),
                        os.path.getmtime(os.path.join(ROOT, 'out', 'rt_version.h')))
    recomp = sorted('out/' + f for f in os.listdir('out') if f.startswith('recomp_') and f.endswith('.c'))
    compile_all(recomp, a.jobs, '[2/4] game code')
    compile_all(['runtime/%s.c' % f for f in RUNTIME], a.jobs, '[3/4] runtime')
    say('    linking...')
    tmp = tempfile.mkdtemp(prefix='nsmbw_build_')
    exe = os.path.join(tmp, 'nsmbw.exe')
    objs = sorted(os.path.join(OBJ, f) for f in os.listdir(OBJ) if f.endswith('.o'))
    r = subprocess.run(ZCC + objs + ['runtime/nsmbw.rc', '-o', exe, '-luser32', '-lgdi32', '-lwinmm', '-lws2_32', '-lwinhttp', '-Wl,--stack,67108864',
                        # fixed load address, no ASLR: save states hold code addresses (rt_state.c)
                        '-Wl,--image-base,0x140000000', '-Wl,--no-dynamicbase'],
                       cwd=ROOT, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode: fail('link:\n' + r.stderr[-3000:])
    if a.no_deploy:
        write_over(exe, os.path.join(ROOT, 'out', 'nsmbw.exe'))
        say('built out\\nsmbw.exe')
    else:
        deploy(exe)
    shutil.rmtree(tmp, ignore_errors=True)
    if (a.publish or a.ask_publish) and not a.no_deploy:
        if a.publish: say('\npublishing version %s...' % ver)
        cmd = [sys.executable, 'tools/publish_update.py', 'publish', '--game', GAME] + ([] if a.publish else ['--ask'])
        if a.notes: cmd += ['--notes', a.notes]
        if subprocess.run(cmd, cwd=ROOT).returncode: fail('publishing failed (see above) - the build itself is fine')
    elif a.publish: fail('--publish publishes the game folder: leave out --no-deploy')
    say('\ndone in %.0f s' % (time.time() - t0))


if __name__ == '__main__':
    main()
