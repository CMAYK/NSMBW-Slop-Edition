# NSMBW (USA Rev 2, SMNE01) -> native PC: static recompilation

## Layout
| Path | What |
|---|---|
| `orig/` | `main.dol` + decompressed `.rel` modules from your disc (not for redistribution) |
| `config.yml`, `config/` | decomp-toolkit project: 39,432 functions detected; `symbols.txt` per module |
| `config/*symbols.dtk-original.txt` | dtk's output before name porting (backup) |
| `tools/port_symbols.py` | Ports PAL v1 names (NSMBW-Decomp + Newer `kamek_pal.x`) to USA v2 via Newer's `versions-nsmbw.txt` address map |
| `tools/recomp.py` | PowerPC (Gekko) -> C static recompiler |
| `tools/link_rels.py` | Links the 4 REL modules at their real in-game addresses (USA v2), applying all relocations |
| `tools/validate_link.py` | Checks branch targets, data code-pointers and ported names land on function starts |
| `linked/` | Linked module images + `manifest.json` / `load.txt` |
| `runtime/ppc_context.h` | Guest CPU state, big-endian memory helpers, paired-single quantization |
| `runtime/test_harness.c` | Runs recompiled game functions on the host and checks them against host references |
| `build_and_test.sh` | Regenerate + compile + test in one go |
| `build/` | dtk split output (per-object assembly) for reading code |

## Status (milestone 1)
- main.dol: 16,291 functions / 737,860 instructions -> C. 99.998% translated; the only
  untranslated instruction is `rfi` (15x, exception-handler returns; belongs in the runtime).
- Generated C compiles warning-free with `gcc -Wall`.
- Correctness harness: 21/21 pass - memset/memcpy/strlen/strcmp/strcpy/atoi, 64-bit
  `__div2u`/`__mod2u` on 20,000 random inputs, `__shl2i`, fdlibm sin/cos (<1e-15 error),
  floor/ceil, fp->unsigned, paired-single PSMTXConcat/PSVECMag/Dot/Normalize, and
  `sprintf` (varargs, jump-table switch, int + float formatting).
- Names: 2,652 symbols ported from PAL v1; ~95% of functions are still `fn_XXXXXXXX`.

## Status (milestone 2: REL modules)
- All 4 modules linked at their real addresses, chained exactly like the in-game heap:
  `d_profileNP` 80768500, `d_basesNP` 8076D6C0 (text 8076D7B0), `d_enemiesNP` 809A2CE0,
  `d_en_bossNP` 80B1C960; .bss at buffer+fixSize. 198,455 relocations applied.
  These match the community (Kamek/Newer) address convention, so existing mod addresses stay valid.
- Link validation: 100% of REL direct calls resolve (the 4% that first looked off were
  mid-function entries of `__save_gpr`/`__restore_gpr`, now split into their own functions);
  98.5% of code pointers in REL data hit function starts; ported vtables valid.
- Whole game: 39,463 functions / 1,564,963 instructions -> C (99.999%; only `rfi` remains).
  The single unresolved direct call is `__OSDBJump` -> 0x60 (debugger vector; runtime HLE).
- Generated C for the whole game compiles warning-free (100 files).
- Harness 25/25: the 21 library checks plus each module's `_prolog` executed end to end
  (1 / 318 / 162 / 26 static constructors via relocated ctor tables, cross-module and into
  main.dol), with the stack balanced, r14-r31 preserved and zero runtime stubs hit.

## Next milestones
1. ~~RELs~~ done (milestone 2).
2. **Boot runtime** - HLE the Revolution OS: threads (OSThread -> host threads/fibers),
   alarms/interrupts, DVD reads -> `extracted/files`, NAND saves -> host files, `rfi`.
3. **Graphics** - GX command FIFO (write-gather pipe at 0xCC008000) -> a PC renderer.
   The single largest piece; Dolphin's video backend and Aurora are the references.
4. **Audio** (AX/DSP HLE mixer) and **input** (WPAD -> keyboard/gamepad, shake).
5. **Windows build** - MSVC/clang-cl, `VirtualAlloc` 4 GB guest reservation.

## Notes
- NSMBW-Decomp does not accept AI-generated decompilation and does not discuss PC ports;
  this project uses its published symbol names only.
- 31 direct branches target addresses that aren't detected function starts; they fall
  back to `ppc_call_indirect` and need splitting fixes in the dtk config.

## Status (milestone 3: boots natively, renders in a window)
- `out/nsmbw.exe` (Windows, double-click) / `out/nsmbw` (Linux): the recompiled game + native runtime.
  Paths default to `../` (this folder) and `../../extracted` (the unpacked disc).
- Runtime (`runtime/`): guest threads as host fibers, interrupts/timers, Wii hardware registers
  (VI/PI/DSP/AI/EXI/SI), IOS emulation (disc, NAND -> `nand/`, ES, STM), Bluetooth host controller,
  high-level DSP (AX task protocol), and a software GX renderer.
- Boots through the Revolution OS, links all 4 REL modules at the expected addresses, shows the
  strap screen, the "hold the Wii Remote sideways" card and creates real save data in `nand/`.
- Not yet: Wii Remote input (the game waits at "Press OK"), audio output, indirect textures/fog,
  lines/points. Rendering is CPU-only, so it's slower than full speed.
- Build: `./build_runtime.sh` (Linux, also regenerates C), `python tools/make_nand.py` (SYSCONF + setting.txt).

## Building the game (Windows exe)
Needs Python 3.8+ (python.org; tick "Add python.exe to PATH") and zig as a Python package
(`python -m pip install ziglang` - `build_windows.bat` installs it if it's missing).
- Windows: double-click `build_windows.bat` (or run it in a command prompt).
- Any OS: `python build.py` (`./build_windows.sh` does the same).

It regenerates the C code in `out/` when inputs changed, compiles what changed, links
`nsmbw.exe` and copies it (with what it needs) into `../game`. The first build compiles ~100
large generated files: several minutes, ~1.3 GB of memory per parallel compile (the job count is
picked from cores and memory; `--jobs N` to override). Later builds only redo what changed.
If the game is running, the exe is written as `game/nsmbw_new.exe`. Options: `--clean`, `--no-deploy`.

## Change log
Work between sessions (what changed, why, how it was tested, what is open) is recorded in `CHANGELOG.md` - read it first.
What's planned and who is on what: `TODO.md`. Third-party code and its license: `THIRD_PARTY_NOTICES.md`.
Progress of moving the 8-player code to native C: `NATIVE_PORT.md` (generated by `tools/native_port_status.py`).
