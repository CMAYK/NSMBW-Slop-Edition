/* rt_plat_win.c - Windows platform: Win32 window (GDI blit), fibers, keyboard. */
#include "rt.h"
#include <windows.h>

struct rt_fiber { void *f; };
static rt_fiber main_fiber;
static HWND hwnd;
static BITMAPINFO bmi;
static const uint32_t *frame_px; static int frame_w = 640, frame_h = 456;
static volatile uint32_t buttons;
static int quit_requested;
static HDC gl_dc; static int gl_window;      /* GL renders straight into the game window */

rt_fiber *plat_fiber_main(void) {
    main_fiber.f = ConvertThreadToFiber(NULL);
    return &main_fiber;
}
typedef struct { void (*fn)(void *); void *arg; } fstart;
static void WINAPI fiber_entry(void *p) { fstart *s = p; s->fn(s->arg); }
rt_fiber *plat_fiber_create(void (*fn)(void *), void *arg) {
    rt_fiber *f = calloc(1, sizeof *f);
    fstart *s = malloc(sizeof *s); s->fn = fn; s->arg = arg;
    f->f = CreateFiberEx(1 << 20, 16 << 20, 0, fiber_entry, s);
    if (!f->f) rt_fatal("CreateFiber failed");
    return f;
}
void plat_fiber_switch(rt_fiber *from, rt_fiber *to) { (void)from; SwitchToFiber(to->f); }

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
    case WM_KEYDOWN:
        if (wp == VK_F11) { set_fullscreen(!fullscreen); return 0; }
        if (wp == VK_ESCAPE) { if (fullscreen) set_fullscreen(0); else quit_requested = 1; return 0; }
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
 * addresses are image offsets, resolved against the linker map. NSMBW_PROFILE=0 turns it off. */
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
    const char *e = getenv("NSMBW_PROFILE"); if (e && *e == '0') return;
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
    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG WINAPI vectored(EXCEPTION_POINTERS *ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c == 0xC0000005 || c == 0xC00000FD || c == 0xC000001D || c == 0xC0000094 || c == 0xC0000409 || c == 0xC0000096 || c == 0x80000003)
        return crash_filter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}
void plat_init(void) {
    char lp[1200]; snprintf(lp, sizeof lp, "%s/nsmbw_log.txt", g_root);
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
}

int plat_pump(void) {
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
    if (quit_requested) { rt_log("win", "window closed"); if (prof_target) prof_write(); ExitProcess(0); }
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
static void fps_title(void) {
    /* FPS counter in the title bar: frames the game presented per second (game runs at 60) */
    static uint64_t t_last; static int nframes;
    uint64_t now = rt_now_ns(); nframes++;
    if (!t_last) t_last = now;
    if (now - t_last >= 500000000ull) {
        double fps = nframes * 1e9 / (double)(now - t_last);
        char title[160];
        snprintf(title, sizeof title, "New Super Mario Bros. Wii - native recompilation  |  %.1f FPS", fps);
        SetWindowTextA(hwnd, title);
        t_last = now; nframes = 0;
    }
}
uint32_t plat_buttons(void) { return buttons; }

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
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) return 0;
    HGLRC legacy = p_create(dc);
    if (!legacy || !p_make(dc, legacy)) return 0;
    PFN_wglCreateContextAttribsARB p_attribs = (PFN_wglCreateContextAttribsARB)p_wglGetProcAddress("wglCreateContextAttribsARB");
    if (p_attribs) {
        const int attr[] = {0x2091, 3, 0x2092, 3, 0x9126, 1, 0};   /* major 3, minor 3, core profile */
        HGLRC core = p_attribs(dc, NULL, attr);
        if (core) { p_make(dc, core); p_delete(legacy); }
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

/* ------------------------------------------------------------------ real Wii Remote (Bluetooth HID)
 * A Wii Remote paired with Windows shows up as a HID device (057E:0306, or 057E:0330 for the -TR
 * model). Like Dolphin's "Real Wii Remote" mode, its HID reports are passed straight through:
 * output reports from the game are written to it, input reports it sends go to the game.
 * hid.dll / setupapi.dll are loaded at runtime. Reads and writes run on their own threads. */
#include <setupapi.h>
typedef struct { ULONG Size; USHORT VendorID, ProductID, VersionNumber; } HIDATTR;
typedef struct { USHORT Usage, UsagePage, InputReportByteLength, OutputReportByteLength, FeatureReportByteLength, Reserved[17], pad[10]; } HIDCAPS;
static void (WINAPI *pHidD_GetHidGuid)(GUID *);
static BOOLEAN (WINAPI *pHidD_GetAttributes)(HANDLE, HIDATTR *);
static BOOLEAN (WINAPI *pHidD_SetOutputReport)(HANDLE, void *, ULONG);
static BOOLEAN (WINAPI *pHidD_GetPreparsedData)(HANDLE, void **);
static BOOLEAN (WINAPI *pHidD_FreePreparsedData)(void *);
static LONG (WINAPI *pHidP_GetCaps)(void *, HIDCAPS *);
static HDEVINFO (WINAPI *pSetupDiGetClassDevsA)(const GUID *, PCSTR, HWND, DWORD);
static BOOL (WINAPI *pSetupDiEnumDeviceInterfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD, PSP_DEVICE_INTERFACE_DATA);
static BOOL (WINAPI *pSetupDiGetDeviceInterfaceDetailA)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_A, DWORD, PDWORD, PSP_DEVINFO_DATA);
static BOOL (WINAPI *pSetupDiDestroyDeviceInfoList)(HDEVINFO);

static HANDLE wm_dev = INVALID_HANDLE_VALUE;
static volatile LONG wm_alive;             /* 1 while the device handle is good */
static int wm_out_len = 22, wm_in_len = 22;
static CRITICAL_SECTION wm_lock; static CONDITION_VARIABLE wm_cv;
#define WMQ 256
static uint8_t wm_in[WMQ][23]; static int wm_in_n[WMQ]; static unsigned wm_in_head, wm_in_tail;     /* device -> game */
static uint8_t wm_out[WMQ][23]; static int wm_out_n[WMQ]; static unsigned wm_out_head, wm_out_tail;  /* game -> device */

static int hid_load(void) {
    static int done, ok;
    if (done) return ok;
    done = 1;
    HMODULE h = LoadLibraryA("hid.dll"), s = LoadLibraryA("setupapi.dll");
    if (!h || !s) return 0;
#define GP(m, n) if (!(*(void **)&p##n = (void *)GetProcAddress(m, #n))) return 0;
    GP(h, HidD_GetHidGuid) GP(h, HidD_GetAttributes) GP(h, HidD_SetOutputReport) GP(h, HidD_GetPreparsedData)
    GP(h, HidD_FreePreparsedData) GP(h, HidP_GetCaps)
    GP(s, SetupDiGetClassDevsA) GP(s, SetupDiEnumDeviceInterfaces) GP(s, SetupDiGetDeviceInterfaceDetailA) GP(s, SetupDiDestroyDeviceInfoList)
#undef GP
    InitializeCriticalSection(&wm_lock); InitializeConditionVariable(&wm_cv);
    return ok = 1;
}

static HANDLE find_wiimote(void) {
    GUID g; pHidD_GetHidGuid(&g);
    HDEVINFO di = pSetupDiGetClassDevsA(&g, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof ifd;
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && pSetupDiEnumDeviceInterfaces(di, NULL, &g, i, &ifd); i++) {
        DWORD need = 0; pSetupDiGetDeviceInterfaceDetailA(di, &ifd, NULL, 0, &need, NULL);
        if (!need) continue;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_A det = malloc(need); det->cbSize = sizeof *det;
        if (pSetupDiGetDeviceInterfaceDetailA(di, &ifd, det, need, NULL, NULL)) {
            HANDLE h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                HIDATTR a; a.Size = sizeof a;
                if (pHidD_GetAttributes(h, &a) && a.VendorID == 0x057E && (a.ProductID == 0x0306 || a.ProductID == 0x0330)) {
                    void *pp; HIDCAPS caps; memset(&caps, 0, sizeof caps);
                    if (pHidD_GetPreparsedData(h, &pp)) { pHidP_GetCaps(pp, &caps); pHidD_FreePreparsedData(pp); }
                    wm_out_len = caps.OutputReportByteLength ? caps.OutputReportByteLength : 22;
                    wm_in_len = caps.InputReportByteLength ? caps.InputReportByteLength : 22;
                    if (wm_out_len > 23) wm_out_len = 23; if (wm_in_len > 23) wm_in_len = 23;
                    rt_log("wm", "real Wii Remote found (%s, product %04X)", a.ProductID == 0x0330 ? "RVL-CNT-01-TR" : "RVL-CNT-01", a.ProductID);
                    found = h;
                } else CloseHandle(h);
            }
        }
        free(det);
    }
    pSetupDiDestroyDeviceInfoList(di);
    return found;
}

static void wm_lost(void) {
    if (InterlockedExchange(&wm_alive, 0)) rt_log("wm", "real Wii Remote disconnected");
}

static DWORD WINAPI wm_reader(LPVOID a) {
    (void)a;
    OVERLAPPED ov; memset(&ov, 0, sizeof ov); ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    uint8_t buf[64];
    for (;;) {
        if (!wm_alive) {                                   /* (re)connect: scan every 2 s */
            if (wm_dev != INVALID_HANDLE_VALUE) { CloseHandle(wm_dev); wm_dev = INVALID_HANDLE_VALUE; }
            HANDLE h = find_wiimote();
            if (h == INVALID_HANDLE_VALUE) { Sleep(2000); continue; }
            wm_dev = h; InterlockedExchange(&wm_alive, 1);
        }
        ResetEvent(ov.hEvent);
        DWORD got = 0;
        if (!ReadFile(wm_dev, buf, wm_in_len, &got, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) { wm_lost(); continue; }
            if (!GetOverlappedResult(wm_dev, &ov, &got, TRUE)) { wm_lost(); continue; }
        }
        if (got < 2) continue;
        EnterCriticalSection(&wm_lock);
        if (wm_in_head - wm_in_tail < WMQ) { int k = wm_in_head++ % WMQ; memcpy(wm_in[k], buf, got > 23 ? 23 : got); wm_in_n[k] = got > 23 ? 23 : (int)got; }
        LeaveCriticalSection(&wm_lock);
    }
    return 0;
}
static DWORD WINAPI wm_writer(LPVOID a) {
    (void)a;
    OVERLAPPED ov; memset(&ov, 0, sizeof ov); ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    for (;;) {
        EnterCriticalSection(&wm_lock);
        while (wm_out_head == wm_out_tail) SleepConditionVariableCS(&wm_cv, &wm_lock, INFINITE);
        int k = wm_out_tail++ % WMQ; uint8_t buf[23]; memset(buf, 0, sizeof buf); memcpy(buf, wm_out[k], wm_out_n[k]);
        LeaveCriticalSection(&wm_lock);
        if (!wm_alive) continue;
        /* Microsoft Bluetooth stack: WriteFile with a full-size report; other stacks: HidD_SetOutputReport */
        ResetEvent(ov.hEvent);
        DWORD wr = 0; BOOL ok = WriteFile(wm_dev, buf, wm_out_len, &wr, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 200) != WAIT_OBJECT_0) { CancelIo(wm_dev); ok = FALSE; }
            else ok = GetOverlappedResult(wm_dev, &ov, &wr, FALSE);
        }
        if (!ok && !pHidD_SetOutputReport(wm_dev, buf, wm_out_len)) {
            DWORD e = GetLastError();
            if (e == ERROR_DEVICE_NOT_CONNECTED || e == ERROR_INVALID_HANDLE) wm_lost();
        }
    }
    return 0;
}

/* 1 = a real Wii Remote is connected right now (starts the background search on first call) */
int plat_wiimote_present(void) {
    static int started;
    if (!started) {
        started = 1;
        if (!hid_load()) return 0;                        /* always on: the keyboard keeps working too */
        CreateThread(NULL, 0, wm_reader, NULL, 0, NULL);
        CreateThread(NULL, 0, wm_writer, NULL, 0, NULL);
    }
    return wm_alive != 0;
}
/* queue an output report (report id first) for the real remote */
void plat_wiimote_write(const uint8_t *r, int len) {
    if (len > 23) len = 23;
    EnterCriticalSection(&wm_lock);
    if (wm_out_head - wm_out_tail < WMQ) { int k = wm_out_head++ % WMQ; memcpy(wm_out[k], r, len); wm_out_n[k] = len; WakeConditionVariable(&wm_cv); }
    LeaveCriticalSection(&wm_lock);
}
/* next input report from the real remote (report id first); 0 = none */
int plat_wiimote_read(uint8_t *r, int max) {
    int n = 0;
    EnterCriticalSection(&wm_lock);
    if (wm_in_head != wm_in_tail) { int k = wm_in_tail++ % WMQ; n = wm_in_n[k] < max ? wm_in_n[k] : max; memcpy(r, wm_in[k], n); }
    LeaveCriticalSection(&wm_lock);
    return n;
}

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
