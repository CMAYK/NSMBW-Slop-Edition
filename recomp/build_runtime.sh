#!/bin/sh
# Build the native runtime (Linux, headless) against freshly recompiled game code.
# Usage: ./build_runtime.sh [--regen]   (regen re-runs the recompiler; slow compile)
set -e
cd "$(dirname "$0")"
CFLAGS="-std=gnu11 -O1 -g -w -Iruntime -Iout -DPPC_TRACE"
if [ "$1" = "--regen" ] || [ ! -f out/recomp_table.c ]; then
  python3 tools/recomp.py --out out
  rm -f out/obj/*.o 2>/dev/null || true
fi
mkdir -p out/obj frames nand
for f in out/recomp_*.c; do
  o=out/obj/$(basename $f).o
  [ -f "$o" ] && [ "$o" -nt "$f" ] && continue
  echo "$f"
done | xargs -r -P "$(nproc)" -I{} sh -c 'gcc -std=gnu11 -O1 -w -Iruntime -Iout -DPPC_TRACE -c {} -o out/obj/$(basename {}).tmp && mv out/obj/$(basename {}).tmp out/obj/$(basename {}).o'
for f in rt_core rt_hw rt_ios rt_disc rt_gx rt_gxrender rt_bt rt_wiimote rt_plat_linux; do
  gcc $CFLAGS -c runtime/$f.c -o out/obj/$f.o
done
gcc -no-pie out/obj/recomp_*.o out/obj/rt_*.o -lm -o out/nsmbw
echo built out/nsmbw
