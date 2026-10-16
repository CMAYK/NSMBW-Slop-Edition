/* test_smb1_physics.c - checks runtime/smb1_physics.c against known Super Mario Bros. numbers.
 *
 *   cc -O2 -Iruntime tools/test_smb1_physics.c runtime/smb1_physics.c -o test_smb1 && ./test_smb1
 *   ./test_smb1 trace "R:60 RB:120 RBA:40"    print a frame-by-frame trace for a button script
 *
 * The level is a flat floor (y = 0) with no walls; the host side is the same as the game's:
 * move, then report the ground. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "smb1_physics.h"

typedef struct { smb1_player p; int x, y, peak; } sim;     /* x, y in pixels (y down, floor at 0) */

static void sim_init(sim *s, int small) { smb1_init(&s->p, small, SMB1_FACE_RIGHT); s->x = s->y = 0; s->peak = 0; s->p.Player_State = SMB1_ON_GROUND; }
static void step(sim *s, uint8_t buttons) {
    smb1_motion m;
    smb1_frame(&s->p, buttons, &m);
    s->x += m.dx; s->y += m.dy;
    int ground = 0;
    if (s->y >= 0) { s->y = 0; ground = 1; }
    if (-s->y > s->peak) s->peak = -s->y;
    smb1_ground(&s->p, ground);
}
static double speed(const sim *s) { return (int8_t)s->p.Player_X_Speed / 16.0; }

static int fails;
static void check(const char *what, double got, double lo, double hi) {
    int ok = got >= lo && got <= hi;
    if (!ok) fails++;
    printf("%-58s %8.3f  (expected %g..%g)  %s\n", what, got, lo, hi, ok ? "ok" : "FAIL");
}

/* a full jump (A held until the peak) after reaching the given ground speed; returns height */
static int jump_height(int small, uint8_t move, int frames_before) {
    sim s; sim_init(&s, small);
    for (int i = 0; i < frames_before; i++) step(&s, move);
    s.peak = 0;
    step(&s, move | SMB1_A);
    for (int i = 0; i < 200 && s.p.Player_State != SMB1_ON_GROUND; i++) step(&s, move | SMB1_A);
    return s.peak;
}

static void trace(const char *script) {
    sim s; sim_init(&s, 0);
    char buf[512]; strncpy(buf, script, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    int f = 0;
    for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
        char *c = strchr(tok, ':'); int n = c ? atoi(c + 1) : 1; if (c) *c = 0;
        uint8_t b = 0;
        for (char *k = tok; *k; k++) b |= *k == 'R' ? SMB1_RIGHT : *k == 'L' ? SMB1_LEFT : *k == 'A' ? SMB1_A : *k == 'B' ? SMB1_B : *k == 'D' ? SMB1_DOWN : *k == 'U' ? SMB1_UP : 0;
        for (int i = 0; i < n; i++, f++) {
            step(&s, b);
            printf("%4d %-4s x %5d y %4d  xs %02X(%6.3f) xmf %02X  ys %02X ymf %02X  st %d face %d mov %d abs %02X\n", f, tok, s.x, s.y,
                   s.p.Player_X_Speed, speed(&s), s.p.Player_X_MoveForce, s.p.Player_Y_Speed, s.p.Player_Y_MoveForce,
                   s.p.Player_State, s.p.PlayerFacingDir, s.p.Player_MovingDir, s.p.Player_XSpeedAbsolute);
        }
    }
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "trace")) { trace(argv[2]); return 0; }
    sim s;
    /* horizontal */
    sim_init(&s, 0); for (int i = 0; i < 120; i++) step(&s, SMB1_RIGHT);
    check("walk: top speed (px/frame)", speed(&s), 1.5, 1.5);
    sim_init(&s, 0); for (int i = 0; i < 120; i++) step(&s, SMB1_RIGHT | SMB1_B);
    check("run (B held): top speed (px/frame)", speed(&s), 2.5, 2.5);
    sim_init(&s, 0); int n = 0; while (speed(&s) < 1.5 && n < 200) { step(&s, SMB1_RIGHT); n++; }
    check("walk: frames from standing to top speed", n, 35, 45);
    sim_init(&s, 0); n = 0; while (speed(&s) < 2.5 && n < 200) { step(&s, SMB1_RIGHT | SMB1_B); n++; }
    check("run: frames from standing to top speed", n, 40, 70);
    sim_init(&s, 0); for (int i = 0; i < 120; i++) step(&s, SMB1_RIGHT | SMB1_B);
    n = 0; while (speed(&s) > 0 && n < 200) { step(&s, 0); n++; }
    check("run, let go: frames to stop", n, 20, 60);
    /* vertical */
    check("standing jump, A held: height (px)", jump_height(0, 0, 0), 64, 68);
    check("walking jump (top walk speed): height (px)", jump_height(0, SMB1_RIGHT, 120), 66, 72);
    check("running jump (top run speed): height (px)", jump_height(0, SMB1_RIGHT | SMB1_B, 120), 78, 86);
    {   /* tap jump: A for one frame only */
        sim_init(&s, 0); s.peak = 0; step(&s, SMB1_A);
        for (int i = 0; i < 200 && s.p.Player_State != SMB1_ON_GROUND; i++) step(&s, 0);
        check("standing jump, A tapped: height (px)", s.peak, 14, 24);
    }
    {   /* falling speed */
        sim_init(&s, 0); s.p.Player_State = SMB1_FALLING; s.y = -1000;
        for (int i = 0; i < 100; i++) { smb1_motion m; smb1_frame(&s.p, 0, &m); s.y += m.dy; smb1_ground(&s.p, 0); }
        check("falling: terminal speed (px/frame)", (int8_t)s.p.Player_Y_Speed, 4, 4);
    }
    {   /* no air control without input: speed kept */
        sim_init(&s, 0); for (int i = 0; i < 120; i++) step(&s, SMB1_RIGHT);
        step(&s, SMB1_A); for (int i = 0; i < 10; i++) step(&s, SMB1_A);
        check("in the air, no direction held: speed kept (px/frame)", speed(&s), 1.5, 1.5);
    }
    {   /* skid: running right, press left */
        sim_init(&s, 0); for (int i = 0; i < 120; i++) step(&s, SMB1_RIGHT | SMB1_B);
        n = 0; while (speed(&s) > 0 && n < 200) { step(&s, SMB1_LEFT | SMB1_B); n++; }
        check("skid from full run to a stop (frames)", n, 10, 30);
    }
    {   /* pictures (PlayerGfxHandler) */
        smb1_gfx g;
        sim_init(&s, 1); step(&s, 0); smb1_graphics(&s.p, &g);
        check("standing: stand picture", g.gfx == SMB1_GFX_STAND, 1, 1);
        int seen[3] = {0}, changes = 0, last = -1;
        for (int i = 0; i < 60; i++) { step(&s, SMB1_RIGHT); smb1_graphics(&s.p, &g); if (g.gfx == SMB1_GFX_WALK) { seen[g.anim]++; if (g.anim != last) changes++; last = g.anim; } }
        check("walking: all 3 walk frames shown", (seen[0] > 0) + (seen[1] > 0) + (seen[2] > 0), 3, 3);
        check("walking at top speed: frame changes in 1 s (timer 4: 15/s - 7: 8/s)", changes, 8, 16);
        for (int i = 0; i < 60; i++) step(&s, SMB1_RIGHT | SMB1_B);
        step(&s, SMB1_LEFT | SMB1_B); smb1_graphics(&s.p, &g);
        check("running, pressing back: skid picture", g.gfx == SMB1_GFX_SKID, 1, 1);
        step(&s, SMB1_A); step(&s, SMB1_A); smb1_graphics(&s.p, &g);
        check("jumping: jump picture", g.gfx == SMB1_GFX_JUMP, 1, 1);
        sim_init(&s, 1); s.p.SwimmingFlag = 1; s.p.Player_State = SMB1_JUMP_SWIM; s.y = -500;
        int kicks = 0, swim3_kick = 0;
        for (int i = 0; i < 64; i++) { smb1_motion m; smb1_frame(&s.p, (i % 16) < 2 ? SMB1_A : 0, &m); smb1_graphics(&s.p, &g); kicks += g.kick; if (g.anim == 2 && g.kick) swim3_kick++; }
        check("swimming: feet kick half the frames (small: not on swim frame 3)", kicks, 10, 32);
        check("small swim frame 3 never gets the kick feet", swim3_kick, 0, 0);
    }
    printf(fails ? "\n%d check(s) failed\n" : "\nall checks passed\n", fails);
    return fails != 0;
}
