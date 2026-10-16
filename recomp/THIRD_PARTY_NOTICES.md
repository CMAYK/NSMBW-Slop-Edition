# Third-party code

## 8-player module (nsmbw-project, v0.6.5)

The 8-player support and related features started as a separate open-source project for the
original game, which this port builds in (see `NATIVE_PORT.md`). Its PowerPC build is
downloaded by `tools/fetch_mod.py` into `mod/`, linked and recompiled with the game, and is being
rewritten as native C in `runtime/` (files that are ported from it are marked in their header).

- License: Mozilla Public License 2.0 (https://mozilla.org/MPL/2.0/)
- Source code: https://github.com/mkwcat/nsmbw-project (tag v0.6.5)
- Release: https://github.com/mkwcat/nsmbw-project/releases/download/v0.6.5/mkwcat-nsmbw.zip

Under the MPL, the code ported from it (and changes to it) stays under the MPL 2.0, and anyone
receiving the game as an executable must be told where its source code can be obtained - this
file is where that is said. `tools/fetch_mod.py` reads the release address from the
"Release:" line above.
