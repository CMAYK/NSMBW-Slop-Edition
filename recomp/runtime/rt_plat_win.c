/* rt_plat_win.c - Windows platform: Win32 window (GDI blit), fibers, keyboard. */
#include "rt.h"
#include <windows.h>

/* Laptops with two GPUs (Intel/AMD integrated + NVIDIA/AMD dedicated) run programs on the weak
   integrated GPU unless the exe asks for the fast one. The drivers look for these two exported
   variables in the .exe (NVIDIA Optimus / AMD PowerXpress) - the same thing Dolphin and most
   games do. Without them the game "doesn't use the GPU" on those machines. */
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

static HWND hwnd;
static BITMAPINFO bmi;
static const uint32_t *frame_px; static int frame_w = 640, frame_h = 456;
static volatile uint32_t buttons;
static int quit_requested;
/* typed characters, for text fields (the Online screen's address) */
static int text_q[64], text_h, text_t;
static void text_push(int c) { if (text_h - text_t < 64) text_q[text_h++ % 64] = c; }
int plat_text_char(void) { return text_t != text_h ? text_q[text_t++ % 64] : 0; }
static HDC gl_dc; static int gl_window;      /* GL renders straight into the game window */

/* (guest thread fibers: rt_state.c) */

/* Wii Remote buttons (core button bits as reported in HID reports) */
enum { WIIB_LEFT = 0x0001, WIIB_RIGHT = 0x0002, WIIB_DOWN = 0x0004, WIIB_UP = 0x0008, WIIB_PLUS = 0x0010,
       WIIB_2 = 0x0100, WIIB_1 = 0x0200, WIIB_B = 0x0400, WIIB_A = 0x0800, WIIB_MINUS = 0x1000, WIIB_HOME = 0x8000 };
static uint32_t key_to_button(WPARAM k) {
    /* NSMBW holds the remote sideways: d-pad up = left, 2 = jump, 1 = run */
    switch (k) {
    case VK_LEFT: return WIIB_UP;  case VK_RIGHT: return WIIB_DOWN; case VK_UP: return WIIB_RIGHT; case VK_DOWN: return WIIB_LEFT;
    case 'X': return WIIB_2; case 'Z': return WIIB_1; case VK_RETURN: return WIIB_PLUS; case VK_SHIFT: return WIIB_MINUS;
    case 'C': return 0x10000; /* shake */
    case 'A': return WIIB_A; case 'S': return WIIB_B; case VK_HOME: case 'H': return WIIB_HOME;
    }
    return 0;
}

/* ---- window modes: fixed-size window (settings window_width/height, fixed_window_size) and
   borderless fullscreen (settings fullscreen; F11 or Alt+Enter toggles, Esc leaves it) ---- */
static int fullscreen; static RECT saved_rect; static LONG saved_style;
static DWORD window_style(void) { return rt_setting("fixed_window_size", 1) ? (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)) : WS_OVERLAPPEDWINDOW; }
static void set_fullscreen(int on) {
    if (on == fullscreen || !hwnd) return;
    if (on) {
        GetWindowRect(hwnd, &saved_rect); saved_style = GetWindowLongA(hwnd, GWL_STYLE);
        MONITORINFO mi; mi.cbSize = sizeof mi; GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongA(hwnd, GWL_STYLE, (saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        ShowCursor(FALSE);
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, saved_style);
        SetWindowPos(hwnd, NULL, saved_rect.left, saved_rect.top, saved_rect.right - saved_rect.left, saved_rect.bottom - saved_rect.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
        ShowCursor(TRUE);
    }
    fullscreen = on;
}
/* live changes from the in-game options menu */
void plat_set_fullscreen(int on) { set_fullscreen(on); }
int plat_get_fullscreen(void) { return fullscreen; }
void plat_set_window(int w, int h, int fixed) {
    if (!hwnd) return;
    LONG st = (GetWindowLongA(hwnd, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW) | (fixed ? (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)) : WS_OVERLAPPEDWINDOW);
    if (fullscreen) { saved_style = st; RECT r = {0, 0, w, h}; AdjustWindowRect(&r, st, FALSE);
                      saved_rect.right = saved_rect.left + (r.right - r.left); saved_rect.bottom = saved_rect.top + (r.bottom - r.top); return; }
    SetWindowLongA(hwnd, GWL_STYLE, st);
    RECT r = {0, 0, w, h}; AdjustWindowRect(&r, st, FALSE);
    SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}
/* where the 4:3 (or 16:9) picture goes inside the client area: the largest that fits, black bars
   around it if the shapes differ */
static void picture_rect(int cw, int ch, int *x, int *y, int *w, int *h) {
    int ax = rt_widescreen() ? 16 : 4, ay = rt_widescreen() ? 9 : 3;
    int pw = cw, ph = cw * ay / ax;
    if (ph > ch) { ph = ch; pw = ch * ax / ay; }
    *x = (cw - pw) / 2; *y = (ch - ph) / 2; *w = pw; *h = ph;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE: quit_requested = 1; DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_SYSKEYDOWN: if (wp == VK_RETURN) { set_fullscreen(!fullscreen); return 0; } break;   /* Alt+Enter */
    case WM_SYSCHAR: if (wp == '\r') return 0; break;                                            /* (no beep) */
    case WM_CHAR: text_push((int)wp); return 0;                                                   /* (typed text: plat_text_char) */
    case WM_KEYDOWN:
        if (wp == VK_F11) { set_fullscreen(!fullscreen); return 0; }
        if (wp == VK_ESCAPE) { if (fullscreen) set_fullscreen(0); return 0; }   /* (Esc no longer quits) */
        buttons |= key_to_button(wp); return 0;
    case WM_KEYUP: buttons &= ~key_to_button(wp); return 0;
    case WM_ERASEBKGND: if (gl_window) return 1; break;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        if (frame_px) {
            RECT rc; GetClientRect(h, &rc);
            int x, y, w, hh; picture_rect(rc.right, rc.bottom, &x, &y, &w, &hh);
            if (w != rc.right || hh != rc.bottom) FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            bmi.bmiHeader.biWidth = frame_w; bmi.bmiHeader.biHeight = -frame_h;
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, x, y, w, hh, 0, 0, frame_w, frame_h, frame_px, &bmi, DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcA(h, msg, wp, lp);
}


/* ------------------------------------------------------------------ sampling profiler
 * A helper thread samples the game thread ~1000x per second (suspend, read RIP, resume) and
 * counts where it is: the innermost frame inside nsmbw.exe (frames in Windows DLLs are unwound
 * and counted separately) plus its caller. Written to nsmbw_profile.txt every 20 s and at exit;
 * addresses are image offsets, resolved against the linker map. Only runs with NSMBW_PROFILE=1. */
#define PROF_N 65536
typedef struct { uint32_t leaf, caller; uint32_t ext, count; } prof_ent;
static prof_ent prof[PROF_N]; static volatile LONG prof_lock; static uint64_t prof_samples, prof_lost;
static HANDLE prof_target; static uintptr_t img_base, img_end;
static int in_image(DWORD64 a) { return a >= img_base && a < img_end; }
static void prof_add(uint32_t leaf, uint32_t caller, uint32_t ext) {
    uint32_t h = (leaf * 2654435761u) ^ (caller * 40503u) ^ ext;
    for (int k = 0; k < 64; k++) {
        prof_ent *e = &prof[(h + k) & (PROF_N - 1)];
        if (e->count && e->leaf == leaf && e->caller == caller && e->ext == ext) { e->count++; return; }
        if (!e->count) { e->leaf = leaf; e->caller = caller; e->ext = ext; e->count = 1; return; }
    }
    prof_lost++;
}
static void prof_write(void) {
    char p[1200]; snprintf(p, sizeof p, "%s/nsmbw_profile.txt", g_root);
    FILE *f = fopen(p, "w"); if (!f) return;
    fprintf(f, "# samples %llu lost %llu | columns: count leaf_offset caller_offset in_windows_dll\n", (unsigned long long)prof_samples, (unsigned long long)prof_lost);
    while (InterlockedExchange(&prof_lock, 1)) Sleep(0);
    for (int i = 0; i < PROF_N; i++) if (prof[i].count >= 2) fprintf(f, "%u %X %X %u\n", prof[i].count, prof[i].leaf, prof[i].caller, prof[i].ext);
    InterlockedExchange(&prof_lock, 0);
    fclose(f);
}
static DWORD WINAPI prof_main(LPVOID arg) {
    (void)arg;
    timeBeginPeriod(1);
    uint64_t last_write = GetTickCount64();
    for (;;) {
        Sleep(1);
        if (SuspendThread(prof_target) == (DWORD)-1) continue;
        CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(prof_target, &c)) {
            uint32_t ext = 0;
            for (int i = 0; i < 32 && c.Rip && !in_image(c.Rip); i++) {     /* unwind out of system DLLs */
                ext = 1;
                DWORD64 ib; RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(c.Rip, &ib, NULL);
                if (!rf) { c.Rip = *(DWORD64 *)c.Rsp; c.Rsp += 8; continue; }
                void *hd; DWORD64 ef; RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, rf, &c, &hd, &ef, NULL);
            }
            uint32_t leaf = in_image(c.Rip) ? (uint32_t)(c.Rip - img_base) : 0, caller = 0;
            if (leaf) {
                DWORD64 ib; RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(c.Rip, &ib, NULL);
                if (rf) { void *hd; DWORD64 ef; RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, rf, &c, &hd, &ef, NULL); }
                else { c.Rip = *(DWORD64 *)c.Rsp; }
                if (in_image(c.Rip)) caller = (uint32_t)(c.Rip - img_base);
            }
            ResumeThread(prof_target);
            while (InterlockedExchange(&prof_lock, 1)) Sleep(0);
            prof_add(leaf, caller, ext); prof_samples++;
            InterlockedExchange(&prof_lock, 0);
        } else ResumeThread(prof_target);
        if (GetTickCount64() - last_write > 20000) { prof_write(); last_write = GetTickCount64(); }
    }
    return 0;
}
static void prof_start(void) {
    /* Off unless NSMBW_PROFILE=1: suspending the game thread 1000 times a second costs it time
       on every sample, and when the sampler thread is descheduled while the game thread is
       suspended the game stalls for a whole scheduler slice (~15 ms) - a hitch. */
    const char *e = getenv("NSMBW_PROFILE"); if (!(e && *e == '1')) return;
    img_base = (uintptr_t)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)img_base; IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(img_base + dos->e_lfanew);
    img_end = img_base + nt->OptionalHeader.SizeOfImage;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &prof_target, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, prof_main, NULL, 0, NULL);
}

extern void rt_dump_threads(void);
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    static int in_crash; if (in_crash++) return EXCEPTION_EXECUTE_HANDLER;
    EXCEPTION_RECORD *er = ep->ExceptionRecord; CONTEXT c = *ep->ContextRecord;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    fprintf(stderr, "\n*** CRASH: exception %08lX at %p (image+0x%llX)", er->ExceptionCode, er->ExceptionAddress,
            (unsigned long long)((uintptr_t)er->ExceptionAddress - base));
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, " %s address %p", er->ExceptionInformation[0] ? "writing" : "reading", (void *)er->ExceptionInformation[1]);
    fprintf(stderr, "\nnative stack (image offsets):");
    for (int i = 0; i < 48 && c.Rip; i++) {
        fprintf(stderr, " %llX", (unsigned long long)(c.Rip - base));
        DWORD64 ib; RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(c.Rip, &ib, NULL);
        if (!rf) { c.Rip = *(DWORD64 *)c.Rsp; c.Rsp += 8; continue; }
        void *hd; DWORD64 ef;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, rf, &c, &hd, &ef, NULL);
    }
    fprintf(stderr, "\n");
    rt_dump_trace();
    fflush(stderr);
    MessageBoxA(NULL, "The game crashed. Details were written to recomp\\nsmbw_log.txt", "nsmbw", MB_OK | MB_ICONERROR);
    { extern void wm_shutdown_crash(void); wm_shutdown_crash(); }   /* (the remotes off here too) */
    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG WINAPI vectored(EXCEPTION_POINTERS *ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c == 0xC0000005 || c == 0xC00000FD || c == 0xC000001D || c == 0xC0000094 || c == 0xC0000409 || c == 0xC0000096 || c == 0x80000003)
        return crash_filter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}
/* Hang watchdog: if the game stops presenting frames for 8 s while the emulated hardware keeps
   running (audio, timers), write what every guest thread is waiting on to the log, once per hang.
   It only reads; the game keeps running (and a later "[hang] frames again" says it recovered). */
static DWORD WINAPI hang_watch(LPVOID a) {
    (void)a;
    extern uint64_t g_frames_presented, g_vi_fields;
    uint64_t last = 0; int still = 0, reported = 0;
    for (;;) {
        Sleep(1000);
        uint64_t f = g_frames_presented;
        if (f != last || !f) { if (reported) rt_log("hang", "frames again (frame %llu)", (unsigned long long)f); still = 0; reported = 0; last = f; continue; }
        if (++still == 8 && !reported) {
            reported = 1;
            rt_log("hang", "no new frame for %d s (frame %llu, %llu VI fields) - guest state:", still, (unsigned long long)f, (unsigned long long)g_vi_fields);
            rt_dump_trace();
            fflush(stderr);
        }
    }
    return 0;
}

void plat_init(void) {
    char lp[1300]; snprintf(lp, sizeof lp, "%s/nsmbw_log.txt", g_root);
    { const char *sd = rt_session_dir(); if (sd) snprintf(lp, sizeof lp, "%s/nsmbw_log.txt", sd); }   /* (an online game: its own log) */
    printf("NSMBW native: logging to %s\n", lp); fflush(stdout);
    if (freopen(lp, "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
    SetUnhandledExceptionFilter(crash_filter);
    AddVectoredExceptionHandler(1, vectored);
    g_verbose = getenv("NSMBW_VERBOSE") != NULL;
    
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "nsmbw_native"; wc.style = CS_OWNDC;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(1));   /* nsmbw.rc (game/icon.png) */
    RegisterClassA(&wc);
    /* window size: settings window_width x window_height; 0 = the largest of 1920x1440, 1600x1200,
       1280x960, 960x720, 640x480 that fits the work area */
    int ww = rt_setting("window_width", 0), wh = rt_setting("window_height", 0);
    if (ww <= 0 || wh <= 0) {
        RECT wa; SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        static const int s43[5][2] = {{1920, 1440}, {1600, 1200}, {1280, 960}, {960, 720}, {640, 480}};
        static const int s169[5][2] = {{2560, 1440}, {1920, 1080}, {1600, 900}, {1280, 720}, {854, 480}};
        const int (*sizes)[2] = rt_widescreen() ? s169 : s43;
        int i = 0;
        for (; i < 4; i++) { RECT t = {0, 0, sizes[i][0], sizes[i][1]}; AdjustWindowRect(&t, window_style(), FALSE);
                             if (t.right - t.left <= wa.right - wa.left && t.bottom - t.top <= wa.bottom - wa.top) break; }
        ww = sizes[i][0]; wh = sizes[i][1];
    }
    RECT r = {0, 0, ww, wh};
    AdjustWindowRect(&r, window_style(), FALSE);
    hwnd = CreateWindowA("nsmbw_native", "New Super Mario Bros. Wii - native recompilation", window_style() | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                         CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (rt_setting("fullscreen", 0)) set_fullscreen(1);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader; bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    timeBeginPeriod(1);
    prof_start();
    { const char *e = getenv("NSMBW_HANGWATCH"); if (!(e && *e == '0')) CreateThread(NULL, 0, hang_watch, NULL, 0, NULL); }
}

static void wm_shutdown(void);
int plat_pump(void) {
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
    if (quit_requested) { rt_log("win", "window closed"); net_shutdown(); wm_shutdown(); if (prof_target) prof_write(); ExitProcess(0); }
    return 1;
}

static void fps_title(void);
static uint32_t present_copy[640 * 528];
void plat_present(const uint32_t *rgba, int w, int h) {
    if (w > 640) w = 640; if (h > 528) h = 528;
    memcpy(present_copy, rgba, (size_t)w * h * 4);
    frame_px = present_copy; frame_w = w; frame_h = h;
    InvalidateRect(hwnd, NULL, FALSE);
    UpdateWindow(hwnd);
    fps_title();
    plat_pump();
}
typedef BOOL (WINAPI *PFN_wglSwapIntervalEXT)(int);
int plat_gl_present_begin(int *x, int *y, int *w, int *h, int *cw, int *ch) {
    if (!gl_window) return 0;
    RECT rc; GetClientRect(hwnd, &rc); *cw = rc.right; *ch = rc.bottom;
    picture_rect(rc.right, rc.bottom, x, y, w, h);
    *y = rc.bottom - (*y + *h);              /* GL: origin bottom-left */
    return 1;
}
void plat_gl_present_end(void) {
    SwapBuffers(gl_dc);
    ValidateRect(hwnd, NULL);
    fps_title();
    plat_pump();
}
static char title_note[120];
static void set_title(double fps) {
    char title[300];
    extern int g_gx_software;
    snprintf(title, sizeof title, "New Super Mario Bros. Wii - native recompilation  |  %.1f FPS%s%s%s", fps,
             g_gx_software ? "  |  SOFTWARE RENDERER (no OpenGL 3.3 GPU driver found - see nsmbw_log.txt)" : "",
             title_note[0] ? "  |  " : "", title_note);
    if (hwnd) SetWindowTextA(hwnd, title);
}
static double last_fps;
void plat_title_note(const char *note) {
    if (!strcmp(title_note, note ? note : "")) return;
    snprintf(title_note, sizeof title_note, "%s", note ? note : "");
    set_title(last_fps);
}
static void fps_title(void) {
    /* FPS counter in the title bar: frames the game presented per second (game runs at 60) */
    static uint64_t t_last; static int nframes;
    uint64_t now = rt_now_ns(); nframes++;
    if (!t_last) t_last = now;
    if (now - t_last >= 500000000ull) {
        double fps = nframes * 1e9 / (double)(now - t_last);
        last_fps = fps; set_title(fps);
        t_last = now; nframes = 0;
    }
}
uint32_t plat_buttons(void) { return buttons; }

/* idle: sleep about ns nanoseconds (a high-resolution waitable timer where Windows has one,
   Windows 10 1803+; otherwise ~1 ms Sleep granularity thanks to timeBeginPeriod(1)) */
void plat_idle_wait(uint64_t ns) {
    static HANDLE timer; static int tried;
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, 0x1F0003 /* TIMER_ALL_ACCESS */);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, 0x1F0003);
    }
    if (timer) {
        LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(ns / 100);
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { WaitForSingleObject(timer, 20); return; }
    }
    Sleep(ns >= 1500000 ? 1 : 0);
}

/* ------------------------------------------------------------------ OpenGL context (WGL)
 * Rendering goes to an offscreen framebuffer (the EFB); finished frames are scaled into the game
 * window and shown with SwapBuffers, so they never leave the GPU. opengl32.dll is loaded at runtime (its exports would
 * clash with the GL function pointers of the same names). */
typedef HGLRC (WINAPI *PFN_wglCreateContext)(HDC);
typedef BOOL (WINAPI *PFN_wglMakeCurrent)(HDC, HGLRC);
typedef BOOL (WINAPI *PFN_wglDeleteContext)(HGLRC);
typedef PROC (WINAPI *PFN_wglGetProcAddress)(LPCSTR);
typedef HGLRC (WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);
static HMODULE gl_lib;
static PFN_wglGetProcAddress p_wglGetProcAddress;
void *plat_gl_proc(const char *name) {
    void *p = p_wglGetProcAddress ? (void *)p_wglGetProcAddress(name) : NULL;
    if (!p || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) p = (void *)GetProcAddress(gl_lib, name);
    return p;
}
int plat_gl_create(void) {
    gl_lib = LoadLibraryA("opengl32.dll");
    if (!gl_lib) return 0;
    PFN_wglCreateContext p_create = (PFN_wglCreateContext)GetProcAddress(gl_lib, "wglCreateContext");
    PFN_wglMakeCurrent p_make = (PFN_wglMakeCurrent)GetProcAddress(gl_lib, "wglMakeCurrent");
    PFN_wglDeleteContext p_delete = (PFN_wglDeleteContext)GetProcAddress(gl_lib, "wglDeleteContext");
    p_wglGetProcAddress = (PFN_wglGetProcAddress)GetProcAddress(gl_lib, "wglGetProcAddress");
    if (!p_create || !p_make || !p_wglGetProcAddress) return 0;
    /* render into the game window itself (presented with SwapBuffers); NSMBW_GLWINDOW=0 falls back
       to a hidden window + GDI presentation */
    HDC dc = NULL; const char *gw_env = getenv("NSMBW_GLWINDOW"); int in_window = 0;
    if (hwnd && !(gw_env && *gw_env == '0')) { dc = GetDC(hwnd); in_window = 1; }
    else {
        WNDCLASSA wc = {0};
        wc.style = CS_OWNDC; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "nsmbw_gl";
        RegisterClassA(&wc);
        HWND gw = CreateWindowA("nsmbw_gl", "gl", WS_POPUP, 0, 0, 16, 16, NULL, NULL, wc.hInstance, NULL);
        if (!gw) return 0;
        dc = GetDC(gw);
    }
    gl_dc = dc;
    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof pfd; pfd.nVersion = 1; pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(dc, &pfd);
    /* ChoosePixelFormat may hand back one of Microsoft's software formats ("GDI Generic", no GPU)
       even when the driver has accelerated ones, e.g. when it can't match every requested bit.
       Prefer a hardware-accelerated format with the closest match. */
    {
        PIXELFORMATDESCRIPTOR d; int best = 0, best_score = -1;
        int n = DescribePixelFormat(dc, 1, sizeof d, &d);
        for (int i = 1; i <= n; i++) {
            if (!DescribePixelFormat(dc, i, sizeof d, &d)) continue;
            DWORD need = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
            if ((d.dwFlags & need) != need || d.iPixelType != PFD_TYPE_RGBA) continue;
            if ((d.dwFlags & PFD_GENERIC_FORMAT) && !(d.dwFlags & PFD_GENERIC_ACCELERATED)) continue;   /* software */
            int score = 100;
            if (d.cColorBits == 32 || d.cColorBits == 24) score += 40;
            if (d.cDepthBits == 24) score += 30; else if (d.cDepthBits >= 16) score += 10;
            if (d.cAlphaBits) score += 5;
            if (d.cAccumBits) score -= 5;
            if (score > best_score) { best_score = score; best = i; }
        }
        PIXELFORMATDESCRIPTOR cur;
        int cur_sw = pf && DescribePixelFormat(dc, pf, sizeof cur, &cur) && (cur.dwFlags & PFD_GENERIC_FORMAT) && !(cur.dwFlags & PFD_GENERIC_ACCELERATED);
        if (best && (!pf || cur_sw)) { rt_log("gl", "pixel format %d is software-only, using accelerated format %d", pf, best); pf = best; }
        if (pf) DescribePixelFormat(dc, pf, sizeof pfd, &pfd);
    }
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) { rt_log("gl", "no usable OpenGL pixel format (error %lu)", GetLastError()); return 0; }
    HGLRC legacy = p_create(dc);
    if (!legacy || !p_make(dc, legacy)) { rt_log("gl", "wglCreateContext failed (error %lu) - is a graphics driver installed?", GetLastError()); return 0; }
    PFN_wglCreateContextAttribsARB p_attribs = (PFN_wglCreateContextAttribsARB)p_wglGetProcAddress("wglCreateContextAttribsARB");
    if (p_attribs) {
        /* 3.3 core first; some drivers only hand out 3.3+ as a compatibility context, so try that
           next, and finally the newest context the driver gives without a version (4.x on most) */
        static const int attrs[3][8] = {
            {0x2091, 3, 0x2092, 3, 0x9126, 1, 0},          /* major 3, minor 3, core profile */
            {0x2091, 3, 0x2092, 3, 0x9126, 2, 0},          /* ... compatibility profile */
            {0x2091, 4, 0x2092, 0, 0},
        };
        HGLRC ctx = NULL;
        for (int i = 0; i < 3 && !ctx; i++) ctx = p_attribs(dc, NULL, attrs[i]);
        if (ctx) { p_make(dc, ctx); p_delete(legacy); }
        else rt_log("gl", "wglCreateContextAttribsARB could not create a 3.3 context; keeping the default context");
    }
    {   /* the renderer needs OpenGL 3.3: say which GPU/driver we got, and why it won't work */
        typedef const unsigned char *(WINAPI *PFN_glGetString_)(unsigned);
        PFN_glGetString_ gs = (PFN_glGetString_)GetProcAddress(gl_lib, "glGetString");
        const char *ven = gs ? (const char *)gs(0x1F00) : NULL, *ren = gs ? (const char *)gs(0x1F01) : NULL, *ver = gs ? (const char *)gs(0x1F02) : NULL;
        rt_log("gl", "driver: %s | %s | %s", ven ? ven : "?", ren ? ren : "?", ver ? ver : "?");
        int major = ver ? atoi(ver) : 0, minor = 0;
        if (ver) { const char *dot = strchr(ver, '.'); if (dot) minor = atoi(dot + 1); }
        if (ren && strstr(ren, "GDI Generic")) {
            rt_log("gl", "this is Microsoft's software OpenGL (no GPU driver in use): install/update the graphics driver");
            return 0;
        }
        if (major < 3 || (major == 3 && minor < 3)) { rt_log("gl", "OpenGL %d.%d is too old (3.3 needed)", major, minor); return 0; }
    }
    PFN_wglSwapIntervalEXT p_swap = (PFN_wglSwapIntervalEXT)p_wglGetProcAddress("wglSwapIntervalEXT");
    if (p_swap) p_swap(0);                  /* the game paces itself off the emulated video timer */
    gl_window = in_window;
    return 1;
}

/* ------------------------------------------------------------------ audio (waveOut, 32 kHz stereo)
 * The game produces audio paced by the emulated clock; the sound card consumes it at its own
 * slightly different rate, and the game's output arrives in bursts when a frame runs long.
 * Like Dolphin's mixer, samples go into a ring buffer that is kept near a target fill level by
 * resampling the input very slightly (at most +-0.5 % speed, inaudible) instead of dropping or
 * starving. The device is fed from the ring in small blocks. */
#define ABUFS 8
#define AFRAMES 256                         /* 8 ms per device block */
#define RING 16384                          /* frames */
#define TARGET 1600                         /* ~50 ms kept in the ring */
static HWAVEOUT wo; static int wo_failed, wo_started;
static WAVEHDR whdr[ABUFS]; static int16_t wbuf[ABUFS][AFRAMES * 2];
static int16_t ring[RING * 2]; static uint32_t ring_rd, ring_wr;
static double rs_pos; static int16_t rs_prev[2];
static int queued_blocks(void) { int n = 0; for (int i = 0; i < ABUFS; i++) if ((whdr[i].dwFlags & WHDR_PREPARED) && !(whdr[i].dwFlags & WHDR_DONE)) n++; return n; }
static void audio_feed(void) {
    int q = queued_blocks();
    if (!wo_started) { if (ring_wr - ring_rd < TARGET) return; wo_started = 1; }
    if (q == 0 && ring_wr - ring_rd < TARGET / 2) { wo_started = 0; return; }   /* starved: re-prime */
    for (int i = 0; i < ABUFS && ring_wr - ring_rd >= AFRAMES; i++) {
        WAVEHDR *h = &whdr[i];
        if ((h->dwFlags & WHDR_PREPARED) && !(h->dwFlags & WHDR_DONE)) continue;
        if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, h, sizeof *h);
        for (int k = 0; k < AFRAMES; k++, ring_rd++) { wbuf[i][2 * k] = ring[(ring_rd & (RING - 1)) * 2]; wbuf[i][2 * k + 1] = ring[(ring_rd & (RING - 1)) * 2 + 1]; }
        memset(h, 0, sizeof *h); h->lpData = (LPSTR)wbuf[i]; h->dwBufferLength = AFRAMES * 4;
        waveOutPrepareHeader(wo, h, sizeof *h); waveOutWrite(wo, h, sizeof *h);
    }
}
void plat_audio_push(const int16_t *lr, int frames) {
    if (wo_failed) return;
    if (!wo) {
        WAVEFORMATEX f = {0};
        f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 2; f.nSamplesPerSec = 32000; f.wBitsPerSample = 16;
        f.nBlockAlign = 4; f.nAvgBytesPerSec = 32000 * 4;
        if (waveOutOpen(&wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { wo_failed = 1; rt_log("audio", "no audio output device"); return; }
    }
    if (frames <= 0) return;
    /* speed correction from the fill level (ring + what the device still has queued) */
    int fill = (int)(ring_wr - ring_rd) + queued_blocks() * AFRAMES;
    double err = (double)(fill - (TARGET + ABUFS / 2 * AFRAMES)) / TARGET;
    if (err > 1) err = 1; if (err < -1) err = -1;
    double step = 1.0 + 0.005 * err;        /* too full: consume input faster (fewer output samples) */
    if (fill > RING - 2048) step = 2.0;      /* way behind (long stall): catch up quickly */
    /* linear resampler; position 0 = previous chunk's last sample, k = lr[k-1] */
    double pos = rs_pos;
    while (pos < frames) {
        int i = (int)pos; double t = pos - i;
        for (int c = 0; c < 2; c++) {
            int a = i == 0 ? rs_prev[c] : lr[(i - 1) * 2 + c], b = lr[i * 2 + c];
            if (ring_wr - ring_rd < RING) ring[(ring_wr & (RING - 1)) * 2 + c] = (int16_t)(a + (b - a) * t);
        }
        if (ring_wr - ring_rd < RING) ring_wr++;
        pos += step;
    }
    rs_pos = pos - frames;
    rs_prev[0] = lr[(frames - 1) * 2]; rs_prev[1] = lr[(frames - 1) * 2 + 1];
    audio_feed();
}

/* ------------------------------------------------------------------ real Wii Remotes (Bluetooth HID)
 * A Wii Remote connected to Windows shows up as a HID device (057E:0306, or 057E:0330 for the -TR
 * model). Like Dolphin's "Real Wii Remote" mode, its HID reports are passed straight through.
 * Up to eight at once: a scanner thread opens new ones every second; each has a reader and a
 * writer thread. Its Bluetooth address (to remember it by) is the HID serial number string the
 * Microsoft Bluetooth stack gives (12 hex digits). hid.dll / setupapi.dll load at runtime.
 *
 * Pairing for good (plat_wm_pair), the way Dolphin's "Sync" does it: Windows' Bluetooth API
 * (bthprops.cpl) looks for remotes in pairing mode and pairs each with PIN = the PC's Bluetooth
 * address - what a remote expects after its red SYNC button: it then stores the PC like a Wii and
 * reconnects by itself when any button is pressed, after reboots too. (1+2 pairing - PIN = the
 * remote's own address - is a guest pairing the remote doesn't keep; it's the fallback.) The PIN
 * goes to BluetoothAuthenticateDevice as six WCHARs, one address byte each (lowest byte first),
 * length 6 - 0 bytes included. Then BluetoothEnumerateInstalledServices (makes Windows keep the
 * pairing) and the HID service switched on, so the remote becomes a HID device. (The newer
 * BluetoothAuthenticateDeviceEx + authentication callback never called back here: it hung.) */
#include <setupapi.h>
#include <bluetoothapis.h>
typedef struct { ULONG Size; USHORT VendorID, ProductID, VersionNumber; } HIDATTR;
typedef struct { USHORT Usage, UsagePage, InputReportByteLength, OutputReportByteLength, FeatureReportByteLength, Reserved[17], pad[10]; } HIDCAPS;
static void (WINAPI *pHidD_GetHidGuid)(GUID *);
static BOOLEAN (WINAPI *pHidD_GetAttributes)(HANDLE, HIDATTR *);
static BOOLEAN (WINAPI *pHidD_SetOutputReport)(HANDLE, void *, ULONG);
static BOOLEAN (WINAPI *pHidD_GetPreparsedData)(HANDLE, void **);
static BOOLEAN (WINAPI *pHidD_FreePreparsedData)(void *);
static BOOLEAN (WINAPI *pHidD_GetSerialNumberString)(HANDLE, PVOID, ULONG);
static LONG (WINAPI *pHidP_GetCaps)(void *, HIDCAPS *);
static HDEVINFO (WINAPI *pSetupDiGetClassDevsA)(const GUID *, PCSTR, HWND, DWORD);
static BOOL (WINAPI *pSetupDiEnumDeviceInterfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD, PSP_DEVICE_INTERFACE_DATA);
static BOOL (WINAPI *pSetupDiGetDeviceInterfaceDetailA)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_A, DWORD, PDWORD, PSP_DEVINFO_DATA);
static BOOL (WINAPI *pSetupDiDestroyDeviceInfoList)(HDEVINFO);

#define WMDEV 8
#define WMQ 256
typedef struct {
    int used, id;                    /* used: slot taken (until its threads are done) */
    volatile LONG alive;             /* the handle is good */
    HANDLE h, wake;
    uint64_t addr;
    char path[512];
    int out_len, in_len;
    uint8_t in[WMQ][23]; int in_n[WMQ]; unsigned in_head, in_tail;     /* device -> game */
    uint8_t out[WMQ][23]; int out_n[WMQ]; unsigned out_head, out_tail; /* game -> device */
    int threads;                     /* running threads (2 while alive) */
} wmdev_t;
static wmdev_t WD[WMDEV];
static CRITICAL_SECTION wm_lock;
static int wm_next_id = 1;

static int hid_load(void) {
    static int done, ok;
    if (done) return ok;
    done = 1;
    InitializeCriticalSection(&wm_lock);
    HMODULE h = LoadLibraryA("hid.dll"), s = LoadLibraryA("setupapi.dll");
    if (!h || !s) return 0;
#define GP(m, n) if (!(*(void **)&p##n = (void *)GetProcAddress(m, #n))) return 0;
    GP(h, HidD_GetHidGuid) GP(h, HidD_GetAttributes) GP(h, HidD_SetOutputReport) GP(h, HidD_GetPreparsedData)
    GP(h, HidD_FreePreparsedData) GP(h, HidD_GetSerialNumberString) GP(h, HidP_GetCaps)
    GP(s, SetupDiGetClassDevsA) GP(s, SetupDiEnumDeviceInterfaces) GP(s, SetupDiGetDeviceInterfaceDetailA) GP(s, SetupDiDestroyDeviceInfoList)
#undef GP
    return ok = 1;
}

static wmdev_t *dev_by_id(int id) { for (int i = 0; i < WMDEV; i++) if (WD[i].used && WD[i].id == id) return &WD[i]; return NULL; }
static int path_open(const char *p) { for (int i = 0; i < WMDEV; i++) if (WD[i].used && !strcmp(WD[i].path, p)) return 1; return 0; }

static void dev_release(wmdev_t *d) {                       /* a thread leaves; the last one frees the slot */
    EnterCriticalSection(&wm_lock);
    if (--d->threads == 0) {
        CloseHandle(d->h); CloseHandle(d->wake);
        rt_log("wm", "Wii Remote %012llX disconnected", (unsigned long long)d->addr);
        d->used = 0;
    }
    LeaveCriticalSection(&wm_lock);
}
static DWORD WINAPI dev_reader(LPVOID a) {
    wmdev_t *d = a;
    OVERLAPPED ov; memset(&ov, 0, sizeof ov); ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    uint8_t buf[64];
    while (d->alive) {
        ResetEvent(ov.hEvent);
        DWORD got = 0;
        if (!ReadFile(d->h, buf, d->in_len, &got, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) break;
            if (!GetOverlappedResult(d->h, &ov, &got, TRUE)) break;
        }
        if (got < 2) continue;
        EnterCriticalSection(&wm_lock);
        if (d->in_head - d->in_tail < WMQ) { int k = d->in_head++ % WMQ; memcpy(d->in[k], buf, got > 23 ? 23 : got); d->in_n[k] = got > 23 ? 23 : (int)got; }
        LeaveCriticalSection(&wm_lock);
    }
    InterlockedExchange(&d->alive, 0); SetEvent(d->wake);
    CloseHandle(ov.hEvent);
    dev_release(d);
    return 0;
}
static DWORD WINAPI dev_writer(LPVOID a) {
    wmdev_t *d = a;
    OVERLAPPED ov; memset(&ov, 0, sizeof ov); ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    while (d->alive) {
        EnterCriticalSection(&wm_lock);
        int have = d->out_head != d->out_tail;
        uint8_t buf[23]; memset(buf, 0, sizeof buf);
        if (have) { int k = d->out_tail++ % WMQ; memcpy(buf, d->out[k], d->out_n[k]); }
        LeaveCriticalSection(&wm_lock);
        if (!have) { WaitForSingleObject(d->wake, 500); continue; }
        /* Microsoft Bluetooth stack: WriteFile with a full-size report; other stacks: HidD_SetOutputReport */
        ResetEvent(ov.hEvent);
        DWORD wr = 0; BOOL ok = WriteFile(d->h, buf, d->out_len, &wr, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 200) != WAIT_OBJECT_0) { CancelIo(d->h); ok = FALSE; }
            else ok = GetOverlappedResult(d->h, &ov, &wr, FALSE);
        }
        if (!ok && !pHidD_SetOutputReport(d->h, buf, d->out_len)) {
            DWORD e = GetLastError();
            if (e == ERROR_DEVICE_NOT_CONNECTED || e == ERROR_INVALID_HANDLE) { InterlockedExchange(&d->alive, 0); CancelIoEx(d->h, NULL); }
        }
    }
    CancelIoEx(d->h, NULL);                                 /* (wakes the reader) */
    CloseHandle(ov.hEvent);
    dev_release(d);
    return 0;
}

/* the Bluetooth address from the HID serial number ("0017ab3c4d5e"); 0 = not given */
static uint64_t hid_bt_addr(HANDLE h) {
    WCHAR s[64]; memset(s, 0, sizeof s);
    if (!pHidD_GetSerialNumberString(h, s, sizeof s - 2)) return 0;
    uint64_t a = 0; int n = 0;
    for (int i = 0; s[i] && i < 63; i++) {
        WCHAR c = s[i]; int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) continue;                                /* (separators) */
        a = (a << 4) | (uint64_t)v; n++;
    }
    return n == 12 ? a : 0;
}

static void scan_once(void) {
    GUID g; pHidD_GetHidGuid(&g);
    HDEVINFO di = pSetupDiGetClassDevsA(&g, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof ifd;
    for (DWORD i = 0; pSetupDiEnumDeviceInterfaces(di, NULL, &g, i, &ifd); i++) {
        DWORD need = 0; pSetupDiGetDeviceInterfaceDetailA(di, &ifd, NULL, 0, &need, NULL);
        if (!need) continue;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_A det = malloc(need); det->cbSize = sizeof *det;
        if (pSetupDiGetDeviceInterfaceDetailA(di, &ifd, det, need, NULL, NULL) && strlen(det->DevicePath) < 512) {
            EnterCriticalSection(&wm_lock); int open = path_open(det->DevicePath); LeaveCriticalSection(&wm_lock);
            HANDLE h = open ? INVALID_HANDLE_VALUE : CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                HIDATTR a; a.Size = sizeof a;
                wmdev_t *d = NULL;
                if (pHidD_GetAttributes(h, &a) && a.VendorID == 0x057E && (a.ProductID == 0x0306 || a.ProductID == 0x0330)) {
                    EnterCriticalSection(&wm_lock);
                    for (int k = 0; k < WMDEV && !d; k++) if (!WD[k].used) d = &WD[k];
                    if (d) { memset(d, 0, sizeof *d); d->used = 1; d->id = wm_next_id++; }
                    LeaveCriticalSection(&wm_lock);
                }
                if (d) {
                    void *pp; HIDCAPS caps; memset(&caps, 0, sizeof caps);
                    if (pHidD_GetPreparsedData(h, &pp)) { pHidP_GetCaps(pp, &caps); pHidD_FreePreparsedData(pp); }
                    d->out_len = caps.OutputReportByteLength ? caps.OutputReportByteLength : 22;
                    d->in_len = caps.InputReportByteLength ? caps.InputReportByteLength : 22;
                    if (d->out_len > 23) d->out_len = 23; if (d->in_len > 23) d->in_len = 23;
                    d->h = h; d->addr = hid_bt_addr(h); strcpy(d->path, det->DevicePath);
                    d->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
                    d->threads = 2; d->alive = 1;
                    rt_log("wm", "Wii Remote %012llX connected (%s)", (unsigned long long)d->addr, a.ProductID == 0x0330 ? "RVL-CNT-01-TR" : "RVL-CNT-01");
                    CreateThread(NULL, 0, dev_reader, d, 0, NULL);
                    CreateThread(NULL, 0, dev_writer, d, 0, NULL);
                } else CloseHandle(h);
            }
        }
        free(det);
    }
    pSetupDiDestroyDeviceInfoList(di);
}
static DWORD WINAPI wm_scanner(LPVOID a) {
    (void)a;
    for (;;) {
        int n = 0; EnterCriticalSection(&wm_lock); for (int i = 0; i < WMDEV; i++) n += WD[i].used; LeaveCriticalSection(&wm_lock);
        if (n < WMDEV) scan_once();                     /* (opens HID devices to look: every 2 s) */
        Sleep(2000);
    }
    return 0;
}

int plat_wm_list(plat_wm_dev *out, int max) {
    static int started;
    if (!started) { started = 1; if (hid_load()) CreateThread(NULL, 0, wm_scanner, NULL, 0, NULL); }
    if (!hid_load()) return 0;
    int n = 0;
    EnterCriticalSection(&wm_lock);
    for (int i = 0; i < WMDEV && n < max; i++) if (WD[i].used && WD[i].alive) { out[n].id = WD[i].id; out[n].addr = WD[i].addr; n++; }
    LeaveCriticalSection(&wm_lock);
    return n;
}
void plat_wm_write(int id, const uint8_t *r, int len) {
    if (!hid_load()) return;
    if (len > 23) len = 23;
    EnterCriticalSection(&wm_lock);
    wmdev_t *d = dev_by_id(id);
    if (d && d->alive && d->out_head - d->out_tail < WMQ) { int k = d->out_head++ % WMQ; memcpy(d->out[k], r, len); d->out_n[k] = len; SetEvent(d->wake); }
    LeaveCriticalSection(&wm_lock);
}
int plat_wm_read(int id, uint8_t *r, int max) {
    if (!hid_load()) return 0;
    int n = 0;
    EnterCriticalSection(&wm_lock);
    wmdev_t *d = dev_by_id(id);
    if (d && d->in_head != d->in_tail) { int k = d->in_tail++ % WMQ; n = d->in_n[k] < max ? d->in_n[k] : max; memcpy(r, d->in[k], n); }
    LeaveCriticalSection(&wm_lock);
    return n;
}

/* ---- pairing (bthprops.cpl) ---- */
static HBLUETOOTH_RADIO_FIND (WINAPI *pBluetoothFindFirstRadio)(const BLUETOOTH_FIND_RADIO_PARAMS *, HANDLE *);
static BOOL (WINAPI *pBluetoothFindRadioClose)(HBLUETOOTH_RADIO_FIND);
static BOOL (WINAPI *pBluetoothFindNextRadio)(HBLUETOOTH_RADIO_FIND, HANDLE *);
static DWORD (WINAPI *pBluetoothGetRadioInfo)(HANDLE, PBLUETOOTH_RADIO_INFO);
static HBLUETOOTH_DEVICE_FIND (WINAPI *pBluetoothFindFirstDevice)(const BLUETOOTH_DEVICE_SEARCH_PARAMS *, BLUETOOTH_DEVICE_INFO *);
static BOOL (WINAPI *pBluetoothFindNextDevice)(HBLUETOOTH_DEVICE_FIND, BLUETOOTH_DEVICE_INFO *);
static BOOL (WINAPI *pBluetoothFindDeviceClose)(HBLUETOOTH_DEVICE_FIND);
static DWORD (WINAPI *pBluetoothEnumerateInstalledServices)(HANDLE, const BLUETOOTH_DEVICE_INFO *, DWORD *, GUID *);
static DWORD (WINAPI *pBluetoothAuthenticateDevice)(HWND, HANDLE, BLUETOOTH_DEVICE_INFO *, PWSTR, ULONG);
static DWORD (WINAPI *pBluetoothSetServiceState)(HANDLE, const BLUETOOTH_DEVICE_INFO *, const GUID *, DWORD);
static DWORD (WINAPI *pBluetoothRemoveDevice)(const BLUETOOTH_ADDRESS *);
/* (optional) the binary-PIN way: a PIN with a 0 byte can't go through BluetoothAuthenticateDevice */
static DWORD (WINAPI *pBluetoothRegisterForAuthenticationEx)(const BLUETOOTH_DEVICE_INFO *, HBLUETOOTH_AUTHENTICATION_REGISTRATION *, PFN_AUTHENTICATION_CALLBACK_EX, PVOID);
static DWORD (WINAPI *pBluetoothAuthenticateDeviceEx)(HWND, HANDLE, BLUETOOTH_DEVICE_INFO *, PBLUETOOTH_OOB_DATA_INFO, AUTHENTICATION_REQUIREMENTS);
static DWORD (WINAPI *pBluetoothSendAuthenticationResponseEx)(HANDLE, PBLUETOOTH_AUTHENTICATE_RESPONSE);
static BOOL (WINAPI *pBluetoothUnregisterAuthentication)(HBLUETOOTH_AUTHENTICATION_REGISTRATION);
static int bt_load(void) {
    static int done, ok;
    if (done) return ok;
    done = 1;
    HMODULE b = LoadLibraryA("bthprops.cpl");
    if (!b) { rt_log("wm", "pairing: no Bluetooth API (bthprops.cpl)"); return 0; }
#define GP(n) if (!(*(void **)&p##n = (void *)GetProcAddress(b, #n))) { rt_log("wm", "pairing: no " #n); return 0; }
    GP(BluetoothFindFirstRadio) GP(BluetoothFindRadioClose) GP(BluetoothGetRadioInfo) GP(BluetoothFindFirstDevice)
    GP(BluetoothFindNextDevice) GP(BluetoothFindDeviceClose) GP(BluetoothEnumerateInstalledServices)
    GP(BluetoothAuthenticateDevice) GP(BluetoothSetServiceState) GP(BluetoothRemoveDevice)
#undef GP
#define GO(n) *(void **)&p##n = (void *)GetProcAddress(b, #n);
    GO(BluetoothFindNextRadio) GO(BluetoothRegisterForAuthenticationEx) GO(BluetoothAuthenticateDeviceEx) GO(BluetoothSendAuthenticationResponseEx) GO(BluetoothUnregisterAuthentication)
#undef GO
    return ok = 1;
}
/* HumanInterfaceDeviceServiceClass_UUID */
static const GUID hid_service = {0x00001124, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};

static volatile LONG pair_on, pair_running;
static volatile int pair_st;                     /* plat_wm_pair_state */
static char pair_msg[160];
static volatile uint64_t pair_last_addr;         /* plat_wm_pair_last */
static volatile unsigned pair_gen;
static void pair_set(int st, const char *fmt, ...) {
    EnterCriticalSection(&wm_lock);
    pair_st = st;
    va_list ap; va_start(ap, fmt); vsnprintf(pair_msg, sizeof pair_msg, fmt, ap); va_end(ap);
    LeaveCriticalSection(&wm_lock);
    rt_log("wm", "pairing: %s", pair_msg);
}
/* pair with a 6-byte PIN, one WCHAR per byte (Dolphin's AuthenticateWiimote). Windows takes the
   passkey as a string, so a PIN with a 0 byte is cut short there (the user's PC address ends in
   :00 -> the SYNC PIN starts with 0 -> ERROR_NOT_AUTHENTICATED 1244 every time). */
static DWORD pair_string_pin(HANDLE radio, BLUETOOTH_DEVICE_INFO *info, const uint8_t pin[6]) {
    WCHAR pass[6]; for (int i = 0; i < 6; i++) pass[i] = pin[i];
    return pBluetoothAuthenticateDevice(NULL, radio, info, pass, 6);
}
/* the binary way: BluetoothAuthenticateDeviceEx, answering Windows' PIN request from our
   callback with the 6 bytes as they are. Run on its own thread with a time limit (an earlier
   version of this hung on the user's PC). */
typedef struct {
    HANDLE radio; BLUETOOTH_DEVICE_INFO info; uint8_t pin[6];
    volatile LONG asked; DWORD answer, result; HANDLE done;
} binpin_t;
static BOOL CALLBACK binpin_cb(LPVOID pv, PBLUETOOTH_AUTHENTICATION_CALLBACK_PARAMS p) {
    binpin_t *c = (binpin_t *)pv;
    BLUETOOTH_AUTHENTICATE_RESPONSE r; memset(&r, 0, sizeof r);
    r.bthAddressRemote = p->deviceInfo.Address;
    r.authMethod = p->authenticationMethod;
    if (p->authenticationMethod == BLUETOOTH_AUTHENTICATION_METHOD_LEGACY) { memcpy(r.pinInfo.pin, c->pin, 6); r.pinInfo.pinLength = 6; }
    else if (p->authenticationMethod == BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY) r.passkeyInfo.passkey = p->Passkey;
    else r.numericCompInfo.NumericValue = p->Numeric_Value;   /* (yes to numeric comparison / notification) */
    c->answer = pBluetoothSendAuthenticationResponseEx(NULL, &r);
    InterlockedIncrement(&c->asked);
    rt_log("wm", "pairing: Windows asked for the PIN (method %d) -> answered %lu", (int)p->authenticationMethod, c->answer);
    return TRUE;
}
static DWORD WINAPI binpin_thread(LPVOID pv) {
    binpin_t *c = (binpin_t *)pv;
    HBLUETOOTH_AUTHENTICATION_REGISTRATION reg = NULL;
    DWORD e = pBluetoothRegisterForAuthenticationEx(&c->info, &reg, binpin_cb, c);
    if (e != ERROR_SUCCESS) { rt_log("wm", "pairing: register for authentication -> %lu", e); c->result = e; SetEvent(c->done); return 0; }
    c->result = pBluetoothAuthenticateDeviceEx(NULL, c->radio, &c->info, NULL, MITMProtectionNotRequired);
    pBluetoothUnregisterAuthentication(reg);
    SetEvent(c->done);
    return 0;
}
static DWORD pair_binary_pin(HANDLE radio, BLUETOOTH_DEVICE_INFO *info, const uint8_t pin[6]) {
    if (!pBluetoothRegisterForAuthenticationEx || !pBluetoothAuthenticateDeviceEx || !pBluetoothSendAuthenticationResponseEx || !pBluetoothUnregisterAuthentication)
        return ERROR_NOT_SUPPORTED;
    binpin_t *c = (binpin_t *)calloc(1, sizeof *c);       /* (left behind if Windows never answers) */
    c->radio = radio; c->info = *info; memcpy(c->pin, pin, 6); c->done = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, binpin_thread, c, 0, NULL);
    if (!t) { CloseHandle(c->done); free(c); return GetLastError(); }
    CloseHandle(t);
    if (WaitForSingleObject(c->done, 20000) != WAIT_OBJECT_0) {
        rt_log("wm", "pairing: no answer from Windows in 20 s (asked for the PIN: %ld)", c->asked);
        return WAIT_TIMEOUT;
    }
    DWORD r = c->result;
    if (r == ERROR_SUCCESS) *info = c->info;
    rt_log("wm", "pairing: binary PIN -> %lu (Windows asked for it: %ld)", r, c->asked);
    CloseHandle(c->done); free(c);
    return r;
}
/* SYNC button pairing: the PIN is the PC's address (as stored, reversed). 1 = paired */
static int pair_sync(HANDLE radio, BLUETOOTH_DEVICE_INFO *info, const uint8_t pin[6]) {
    int zero = 0; for (int i = 0; i < 6; i++) if (!pin[i]) zero = 1;
    DWORD e;
    if (!zero) {
        e = pair_string_pin(radio, info, pin);
        rt_log("wm", "pairing: authenticate (PIN as text) -> %lu", e);
        if (e == ERROR_SUCCESS) return 1;
    }
    e = pair_binary_pin(radio, info, pin);
    if (e == ERROR_SUCCESS) return 1;
    if (zero && e == ERROR_NOT_SUPPORTED) {                 /* (no Ex API: the text way is all there is) */
        e = pair_string_pin(radio, info, pin);
        rt_log("wm", "pairing: authenticate (PIN as text) -> %lu", e);
        if (e == ERROR_SUCCESS) return 1;
    }
    return 0;
}
static int is_wiimote_name(const WCHAR *n) { return !wcsncmp(n, L"Nintendo RVL-CNT-01", 19); }
static void addr_text(const BLUETOOTH_ADDRESS *a, char *s) {
    sprintf(s, "%02X:%02X:%02X:%02X:%02X:%02X", a->rgBytes[5], a->rgBytes[4], a->rgBytes[3], a->rgBytes[2], a->rgBytes[1], a->rgBytes[0]);
}
/* remotes Windows remembers but can't use (not connected, not authenticated): forget them, as
   Dolphin does - Windows can't reconnect them and they'd stand in the way of pairing */
static void remove_unusable(HANDLE radio) {
    BLUETOOTH_DEVICE_SEARCH_PARAMS sp; memset(&sp, 0, sizeof sp);
    sp.dwSize = sizeof sp; sp.fReturnAuthenticated = TRUE; sp.fReturnRemembered = TRUE; sp.fReturnUnknown = TRUE;
    sp.fReturnConnected = TRUE; sp.fIssueInquiry = FALSE; sp.hRadio = radio;
    BLUETOOTH_DEVICE_INFO info; memset(&info, 0, sizeof info); info.dwSize = sizeof info;
    HBLUETOOTH_DEVICE_FIND df = pBluetoothFindFirstDevice(&sp, &info);
    if (!df) return;
    do {
        if (!is_wiimote_name(info.szName) || !info.fRemembered || info.fConnected || info.fAuthenticated) continue;
        char a[20]; addr_text(&info.Address, a);
        rt_log("wm", "pairing: removing unusable pairing of %s -> %lu", a, pBluetoothRemoveDevice(&info.Address));
    } while (pBluetoothFindNextDevice(df, &info));
    pBluetoothFindDeviceClose(df);
}
static DWORD WINAPI pair_thread(LPVOID unused) {
    (void)unused;
    BLUETOOTH_FIND_RADIO_PARAMS rp = {sizeof rp};
    HANDLE radio = NULL;
    HBLUETOOTH_RADIO_FIND rf = pBluetoothFindFirstRadio(&rp, &radio);
    if (!rf) { pair_set(-1, "No Bluetooth on this PC (or it is switched off)"); InterlockedExchange(&pair_running, 0); return 0; }
    pBluetoothFindRadioClose(rf);
    BLUETOOTH_RADIO_INFO ri; memset(&ri, 0, sizeof ri); ri.dwSize = sizeof ri;
    if (pBluetoothGetRadioInfo(radio, &ri) != ERROR_SUCCESS) { pair_set(-1, "Can't read this PC's Bluetooth address"); CloseHandle(radio); InterlockedExchange(&pair_running, 0); return 0; }
    char host[20]; addr_text(&ri.address, host);
    rt_log("wm", "pairing: PC Bluetooth address %s", host);
    remove_unusable(radio);
    while (pair_on) {
        if (pair_st != 3) pair_set(1, "Press the red SYNC button on the Wii Remote (in the battery compartment)");
        BLUETOOTH_DEVICE_SEARCH_PARAMS sp; memset(&sp, 0, sizeof sp);
        sp.dwSize = sizeof sp; sp.fReturnAuthenticated = TRUE; sp.fReturnRemembered = TRUE; sp.fReturnUnknown = TRUE;
        sp.fReturnConnected = TRUE; sp.fIssueInquiry = TRUE; sp.cTimeoutMultiplier = 2; sp.hRadio = radio;
        BLUETOOTH_DEVICE_INFO info; memset(&info, 0, sizeof info); info.dwSize = sizeof info;
        HBLUETOOTH_DEVICE_FIND df = pBluetoothFindFirstDevice(&sp, &info);
        if (!df) { Sleep(200); continue; }
        do {
            if (!pair_on || !is_wiimote_name(info.szName) || info.fConnected) continue;   /* (connected: in use) */
            char a[20]; addr_text(&info.Address, a);
            rt_log("wm", "pairing: found %s (remembered %d, authenticated %d)", a, info.fRemembered, info.fAuthenticated);
            pair_set(2, "Pairing Wii Remote %s...", a);
            if (info.fRemembered) {                         /* an old pairing the remote may have lost: start over */
                pBluetoothRemoveDevice(&info.Address);
                info.fRemembered = info.fAuthenticated = FALSE;
            }
            /* SYNC button: PIN = the PC's address. (Only that: the remote's own address is for 1+2,
               and the wrong PIN makes Windows drop the remote.) */
            int ok = pair_sync(radio, &info, ri.address.rgBytes);
            if (ok) {                                       /* "must be done to make the remote remember the pairing" (Dolphin) */
                DWORD n = 0, e = pBluetoothEnumerateInstalledServices(radio, &info, &n, NULL);
                if (e != ERROR_SUCCESS && e != ERROR_MORE_DATA) rt_log("wm", "pairing: enumerate services -> %lu", e);
            }
            /* the HID service on either way, as Dolphin does: without pairing the remote still
               connects, for this session */
            DWORD e = pBluetoothSetServiceState(radio, &info, &hid_service, BLUETOOTH_SERVICE_ENABLE);
            rt_log("wm", "pairing: HID service -> %lu", e);
            if (!ok && e != ERROR_SUCCESS) { pair_set(-1, "Pairing %s failed - press SYNC again", a); Sleep(1500); continue; }
            EnterCriticalSection(&wm_lock); pair_last_addr = info.Address.ullLong; pair_gen++; LeaveCriticalSection(&wm_lock);
            pair_set(3, ok ? "Paired %s - it will reconnect with any button from now on" :
                             "Connected %s for this session only (pairing failed) - press SYNC to try again", a);
        } while (pair_on && pBluetoothFindNextDevice(df, &info));
        pBluetoothFindDeviceClose(df);
    }
    CloseHandle(radio);
    InterlockedExchange(&pair_running, 0);
    return 0;
}
int plat_wm_pair_supported(void) { hid_load(); return bt_load(); }
void plat_wm_pair(int on) {
    if (!plat_wm_pair_supported()) return;
    InterlockedExchange(&pair_on, on ? 1 : 0);
    if (on) {
        pair_st = 0;
        if (!InterlockedExchange(&pair_running, 1)) CreateThread(NULL, 0, pair_thread, NULL, 0, NULL);
    } else { pair_st = 0; pair_msg[0] = 0; }
}
int plat_wm_pair_state(char *msg, int n) {
    if (!plat_wm_pair_supported()) { if (msg && n) snprintf(msg, n, "Pairing isn't available on this PC"); return -1; }
    EnterCriticalSection(&wm_lock);
    int st = pair_on ? pair_st : 0;
    if (msg && n) snprintf(msg, n, "%s", pair_on ? pair_msg : "");
    LeaveCriticalSection(&wm_lock);
    return st;
}
uint64_t plat_wm_pair_last(unsigned *gen) {
    *gen = 0;
    if (!hid_load()) return 0;                       /* (sets up wm_lock - this can be the first plat_wm call) */
    EnterCriticalSection(&wm_lock);
    uint64_t a = pair_last_addr; *gen = pair_gen;
    LeaveCriticalSection(&wm_lock);
    return a;
}
int plat_wm_forget(uint64_t addr) {
    if (!plat_wm_pair_supported() || !addr) return 0;
    BLUETOOTH_ADDRESS a; a.ullLong = addr;
    DWORD e = pBluetoothRemoveDevice(&a);
    rt_log("wm", "forget %012llX -> %lu", (unsigned long long)addr, e);
    return e == ERROR_SUCCESS;
}

/* closing the game: drop the Bluetooth link of every Wii Remote - a remote switches itself off
   when its link goes (it stays paired, and any button connects it again next time). Windows has
   no public call for it; IOCTL_BTH_DISCONNECT_DEVICE on the radio (as DS4Windows does for its
   controllers) takes the remote's address. The remotes: the ones we have open, plus any other
   connected "Nintendo RVL-CNT-01" Windows knows of. */
#define IOCTL_BTH_DISCONNECT_DEVICE_ 0x41000C   /* CTL_CODE(FILE_DEVICE_BLUETOOTH, 3, METHOD_BUFFERED, FILE_ANY_ACCESS) */
static void wm_shutdown(void) {
    static int done;
    if (done || !hid_load()) return;
    done = 1;
    uint64_t addrs[32]; int n = 0;
    if (TryEnterCriticalSection(&wm_lock)) {                /* (after a crash it may be held: Windows' list below still has them) */
        for (int i = 0; i < WMDEV; i++) if (WD[i].alive && WD[i].addr && n < 32) addrs[n++] = WD[i].addr;
        LeaveCriticalSection(&wm_lock);
    }
    if (!bt_load()) { if (n) rt_log("wm", "closing: no Bluetooth API - remotes left on"); return; }
    BLUETOOTH_FIND_RADIO_PARAMS rp = {sizeof rp};
    HANDLE radio = NULL;
    HBLUETOOTH_RADIO_FIND rf = pBluetoothFindFirstRadio(&rp, &radio);
    if (!rf) { if (n) rt_log("wm", "closing: no Bluetooth radio - remotes left on"); return; }
    int off = 0;
    do {
        uint64_t mine[32]; int m = n; memcpy(mine, addrs, sizeof(uint64_t) * n);
        BLUETOOTH_DEVICE_SEARCH_PARAMS sp; memset(&sp, 0, sizeof sp);   /* (no inquiry: what Windows knows now) */
        sp.dwSize = sizeof sp; sp.fReturnConnected = TRUE; sp.fReturnAuthenticated = TRUE; sp.fReturnRemembered = TRUE; sp.hRadio = radio;
        BLUETOOTH_DEVICE_INFO info; memset(&info, 0, sizeof info); info.dwSize = sizeof info;
        HBLUETOOTH_DEVICE_FIND df = pBluetoothFindFirstDevice(&sp, &info);
        if (df) {
            do {
                if (!info.fConnected || !is_wiimote_name(info.szName)) continue;
                int dup = 0; for (int k = 0; k < m; k++) if (mine[k] == info.Address.ullLong) dup = 1;
                if (!dup && m < 32) mine[m++] = info.Address.ullLong;
            } while (pBluetoothFindNextDevice(df, &info));
            pBluetoothFindDeviceClose(df);
        }
        for (int k = 0; k < m; k++) {
            ULONGLONG a = mine[k]; DWORD got = 0;
            BOOL ok = DeviceIoControl(radio, IOCTL_BTH_DISCONNECT_DEVICE_, &a, sizeof a, NULL, 0, &got, NULL);
            rt_log("wm", "closing: Wii Remote %012llX off -> %s", (unsigned long long)a, ok ? "ok" : "failed");
            if (ok) off++;
        }
        CloseHandle(radio); radio = NULL;
    } while (pBluetoothFindNextRadio && pBluetoothFindNextRadio(rf, &radio));
    pBluetoothFindRadioClose(rf);
    if (off) Sleep(100);                                    /* (let the disconnects go out) */
}

void wm_shutdown_crash(void) { wm_shutdown(); }

/* start a fresh copy of the game (same exe, same folder) and quit this one */
void plat_restart(void) {
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, sizeof exe);
    STARTUPINFOA si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); si.cb = sizeof si; memset(&pi, 0, sizeof pi);
    char cmd[MAX_PATH + 4]; snprintf(cmd, sizeof cmd, "\"%s\"", exe);
    if (CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    else rt_log("win", "restart failed (%lu)", GetLastError());
    if (prof_target) prof_write();
    ExitProcess(0);
}
