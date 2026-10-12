#!/bin/sh
# Generate out/ (the recompiled C) from orig/ + linked/ + the 8-player module. Skipped when
# none of the inputs changed since the last run.
set -e
cd "$(dirname "$0")/.."
[ -f mod/d_project.rel ] || python3 tools/fetch_mod.py
mkdir -p out
stamp=$(cat tools/recomp.py tools/link_mod.py tools/mod_patches.py tools/mod_funcs.py runtime/hle_funcs.txt \
        orig/main.dol linked/manifest.json linked/*NP.bin mod/d_project.rel mod/d_project_E2.imp config/symbols.txt config/d_*NP/symbols.txt 2>/dev/null | md5sum | cut -c1-32)
if [ -f out/.gen_stamp ] && [ "$(cat out/.gen_stamp)" = "$stamp" ]; then exit 0; fi
python3 tools/link_mod.py > /dev/null
python3 tools/mod_patches.py > /dev/null
python3 tools/mod_funcs.py > /dev/null
python3 tools/recomp.py | grep -E "module|functions:"
echo "$stamp" > out/.gen_stamp
