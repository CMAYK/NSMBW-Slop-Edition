#!/bin/sh
# Linux build using the same compiler as the Windows build (zig/clang -O2) to reproduce
# compiler-specific issues locally.
set -e
cd "$(dirname "$0")"
ZCC="python3 -m ziglang cc -target x86_64-linux-gnu"
FLAGS="-std=gnu11 -O2 -g -w -Iruntime -Iout -DPPC_TRACE"
sh tools/gen.sh
mkdir -p out/objclang
for f in out/recomp_*.c; do o=out/objclang/$(basename $f).o; [ -f "$o" ] && [ "$o" -nt "$f" ] && continue; echo "$f"; done | \
  xargs -r -P "$(nproc)" -I{} sh -c "$ZCC $FLAGS -c {} -o out/objclang/\$(basename {}).o || { rm -f out/objclang/\$(basename {}).o; exit 1; }"
for f in rt_core rt_hw rt_ios rt_disc rt_gx rt_gxrender rt_bt rt_wiimote rt_threads rt_ax rt_hdtex rt_aspect rt_menu rt_lyt rt_mod rt_plat_linux; do $ZCC $FLAGS -c runtime/$f.c -o out/objclang/$f.o; done
BUILD_TMP=${BUILD_TMP:-/tmp/nsmbw_build}; mkdir -p "$BUILD_TMP"
$ZCC out/objclang/*.o -lm -lpthread -ldl -o "$BUILD_TMP/nsmbw_clang"
echo built $BUILD_TMP/nsmbw_clang   "# test build: run from recomp/ as  \$BUILD_TMP/nsmbw_clang . ../extracted"
find . \( -name '.fuse_hidden*' -o -name '*.tmp*' \) -type f -exec rm -f {} + 2>/dev/null || true
