#!/bin/sh
# Cross-compile the native Windows build with zig's bundled clang + mingw, then deploy it to
# ../game, the self-contained folder you run the game from:
#   game/nsmbw.exe        the game
#   game/orig/main.dol    original game code (data sections are loaded from it)
#   game/disc/            the extracted disc (copied once from ../extracted)
#   game/nand/            console settings and your save data (never overwritten by builds)
#   game/nsmbw_log.txt    written by the game
# Objects stay in out/objwin; the exe is linked in a scratch folder (BUILD_TMP) and only the
# finished file is copied into game/ - nothing else is left in out/.
set -e
cd "$(dirname "$0")"
ZCC="python3 -m ziglang cc -target x86_64-windows-gnu"
FLAGS="-std=gnu11 -O2 -march=x86_64_v3 -ffp-contract=off -w -Iruntime -Iout"
sh tools/gen.sh
mkdir -p out/objwin
# objects are written in place (no temp+rename: renames over a shared folder leave .fuse_hidden files)
for f in out/recomp_*.c; do
  o=out/objwin/$(basename $f).o
  [ -f "$o" ] && [ "$o" -nt "$f" ] && continue
  echo "$f"
done | xargs -r -P "$(nproc)" -I{} sh -c "$ZCC $FLAGS -c {} -o out/objwin/\$(basename {}).o || { rm -f out/objwin/\$(basename {}).o; exit 1; }"
for f in rt_core rt_hw rt_ios rt_disc rt_gx rt_gxrender rt_bt rt_wiimote rt_threads rt_ax rt_hdtex rt_aspect rt_menu rt_lyt rt_mod rt_plat_win; do
  $ZCC $FLAGS -c runtime/$f.c -o out/objwin/$f.o
done
BUILD_TMP=${BUILD_TMP:-/tmp/nsmbw_build}; mkdir -p "$BUILD_TMP"
$ZCC out/objwin/*.o runtime/nsmbw.rc -o "$BUILD_TMP/nsmbw.exe" -luser32 -lgdi32 -lwinmm -Wl,--stack,67108864
echo built nsmbw.exe

# deploy
GAME=../game
mkdir -p $GAME/orig $GAME/nand
if cat "$BUILD_TMP/nsmbw.exe" > $GAME/nsmbw.exe 2>/dev/null; then echo "deployed $GAME/nsmbw.exe"
else echo "NOTE: $GAME/nsmbw.exe is in use (game running?) - close it and run this script again"; fi
cmp -s orig/main.dol $GAME/orig/main.dol || cat orig/main.dol > $GAME/orig/main.dol
cmp -s linked/d_project.bin $GAME/orig/d_project.bin || cat linked/d_project.bin > $GAME/orig/d_project.bin
mkdir -p $GAME/mod; cmp -s mod/mkwcat-nsmbw.arc $GAME/mod/mkwcat-nsmbw.arc || cat mod/mkwcat-nsmbw.arc > $GAME/mod/mkwcat-nsmbw.arc
mkdir -p $GAME/ui
for f in ui/*.png; do [ -f "$f" ] && { cmp -s "$f" "$GAME/$f" || cat "$f" > "$GAME/$f"; }; done
# console settings are created once; your save data in game/nand is never overwritten
[ -f $GAME/nand/shared2/sys/SYSCONF ] || python3 tools/make_nand.py --out $GAME/nand
if [ ! -f $GAME/disc/sys/main.dol ] && [ -d ../extracted/sys ]; then echo "copying disc into $GAME/disc (one time)"; mkdir -p $GAME/disc; cp -r ../extracted/sys ../extracted/files $GAME/disc/; fi
find . $GAME \( -name '.fuse_hidden*' -o -name '*.tmp*' \) -type f -exec rm -f {} + 2>/dev/null || true
