#!/bin/sh
# Link RELs, regenerate C from the game binaries, compile, and run the correctness harness.
# Usage: ./build_and_test.sh   (Linux/WSL; needs python3 and gcc)
set -e
cd "$(dirname "$0")"
python3 tools/link_rels.py
python3 tools/validate_link.py
python3 tools/recomp.py --out out
mkdir -p out/obj
ls out/recomp_*.c | xargs -P "$(nproc)" -I{} sh -c 'gcc -std=c11 -O1 -Wall -Wno-unused-label -Iruntime -c {} -o out/obj/$(basename {}).o'
gcc -std=c11 -O1 -Wall -Iruntime -Iout -c runtime/test_harness.c -o out/obj/harness.o
gcc out/obj/*.o -lm -o out/harness
./out/harness orig/main.dol linked
