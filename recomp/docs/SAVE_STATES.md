# Save states (runtime/rt_state.c)

A save state is everything the running game is, so that loading it - in the same copy of the
game or in another one, on another PC - continues exactly where it was taken. Used by online
play (the host's game is sent to everyone at the start; everyone goes back to a state of their
own game at the end) and the base for rollback.

## What is in a state

- guest memory: MEM1 + the PC mod above it (0x80000000-0x82000000: the mod's code and data,
  our retro / character textures and models live at 0x81800000-0x82000000), MEM2 (64 MB), the
  locked cache (16 KB at 0xE0000000), ARAM (16 MB);
- the runtime's emulation state: every variable of the files marked `RT_STATE_FILE` (after
  their #includes; `#pragma clang section` puts their data / bss in sections `rts_d` / `rts_b`
  on Linux, `rtd` / `rtb` on Windows, found by `__start_` / `__stop_` symbols or the PE
  section table), plus single variables of other files marked `RT_STATE`
  (`g_frames_presented`, `kb_latched`, rt_det's EFB page set, rt_remotes' GC port timing and
  menu buttons);
- the guest threads' native stacks: a guest thread is a fiber running recompiled code, and
  where it is lives on its stack (each from its saved stack pointer up).

Variables of state files that belong to this process only are marked `RT_LOCAL` (section
`rtl`): host pointers and handles (disc files, FILE*s, the FST copy, guest memory's address),
paths, the PC's clock (`t0_ns`, TSC calibration, det pacing), renderer handles (the font
atlas, the Controls background), caches (indirect-call cache), logs and stats, settings (re-read
per game). The IOS file descriptors are part of the state but their FILE*s aren't: each open
gets an id and the handle is reopened when the id it was opened for isn't the descriptor's.
Files in the state set: rt_core, rt_hw, rt_ios, rt_disc, rt_si, rt_bt, rt_wiimote, rt_ax,
rt_gx, rt_menu, rt_gamesetup, rt_title, rt_retro, rt_chars, rt_charmdl, rt_aspect, rt_mod,
rt_lyt. Not: the platform files, the renderer (rt_gxrender, rt_hdtex), rt_net, rt_sfx,
rt_threads, rt_remotes, rt_det (except the variables above). After a load each of those gets a
hook: `ios_state_loaded` (reopen files), `gx_render_state_loaded` (texture cache: check
everything again), `remotes_state_loaded`, `det_pace_reset`.

## Same addresses in every copy

Everything in a state that points somewhere must mean the same in another process:
- guest memory at 0x500000000000, the arena (ARAM, then one stack slot per guest thread,
  80 MB apart: 64 MB for the main thread, 16 MB for others, committed when the thread is made)
  at 0x600000000000 (`rt_layout_init`; if those addresses are taken, save states and online
  play are off and the log says so);
- the exe at a fixed base without ASLR (build.py: `--image-base 0x140000000 --no-dynamicbase`;
  the Linux test build is non-PIE);
- guest thread records are a static pool (`thread_pool`), not malloc'd;
- our own fibers (`rt_fsw`, a few instructions per ABI: SysV, and Win64 with xmm6-15 and the
  TEB stack bounds kept on the running fiber) instead of the OS's, whose stacks and records are
  wherever the OS puts them. The guest's main thread runs on a fiber too; main()'s own stack is
  the host fiber, which no state touches.
- no stack protector (`-fno-stack-protector`; a random canary per process would fail every
  restored frame) and no CET shadow stacks (`-fcf-protection=none` on Linux).
A state records a build id (compile time + a code address) and only loads into the same build.

## Taking and loading

`rt_snap_take(s)` works like setjmp, on a guest thread (rt_det.c / rt_net.c call it at a
video field): it saves the registers and return point, then copies every region into one
buffer (header with the region table + the bytes, 117 MB, ~100-170 ms); it returns 0, and 1
when a load of it resumes there. `rt_snap_load(s)` switches to the host fiber, copies the
regions back (committing stacks of threads this process hasn't made yet), runs the hooks and
jumps to the saved return point. `rt_snap_data` / `rt_snap_set` give / take the buffer
(online play sends it).

## Testing (RT_DETERMINISTIC=1, Linux)

- `RT_SNAP_TEST=S,L`: state at field S, load it at field L; the fields after S must hash the
  same both times (`RT_DET_HASH`; compare with a script). Verified: 1000 fields identical.
- `RT_SNAP_SAVE=F:file` / `RT_SNAP_LOAD=F:file`: one copy writes a state, another (booted on
  its own) loads it at its field F and goes on; verified identical to the first for 1758
  fields.
- Debug aids: `RT_POLL_LOG=from,to` (every poll point: clock, registers hash, poll counter),
  `RT_DEC_LOG=1` (decrementer), `RT_DET_DUMP_AT` (+ `.pages2` for the second pass) and
  `RT_DET_PEEK`, `rt_snap_compare` (what differs from a state right after loading it).
  Found this way: the mod's memory above 24 MB wasn't in the state (a one-time flag there made
  the replay call one function fewer).
