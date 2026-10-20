/* cmscale: dsound.dll proxy for Championship Manager 03/04 (cm0304.exe 4.1.5).
 *
 * The game draws everything into a 32 bpp top-down framebuffer and copies dirty
 * rectangles to its window with one StretchDIBits call (0x987e25). It reads the
 * mouse position only from the lParam of WM_MOUSEMOVE. This DLL rewrites the
 * game's import slots so that:
 *
 *   - each present scales the framebuffer to the client area, keeps the aspect
 *     ratio and adds black bars. The OpenGL renderer uploads only the dirty rect
 *     to a texture and scales it on the GPU with a sharp-bilinear shader. GDI is
 *     the fallback;
 *   - the game sees a logical client size, the real size divided by zoom and
 *     clamped to its own min/max (WM_SIZE, GetClientRect), and cmscale maps mouse
 *     coordinates back;
 *   - in fullscreen, ChangeDisplaySettingsA does nothing and the WS_POPUP window
 *     covers the monitor (borderless), so the monitor keeps its native mode.
 *
 * cmscale also stops the pointer blinking under Wine (cursor_fix) and stops idle
 * screens spinning a core (idle_wait). It speeds up day processing with exact
 * replacements (fast_sqrt, fast_rand, fast_dyncast, prefetch) and by simulating
 * background matches on several threads (par_matches). It skips the startup logos
 * (skip_splash), maps the mouse's back/forward buttons to the screen history
 * (mouse_nav) and changes the watched match's highlight and Very Fast speeds
 * (match_hl_speed, match_speed_max). docs/ explains each one.
 *
 * Settings come from cmscale.ini next to the exe. Build: build.sh
 */

#include <windows.h>
#include <emmintrin.h>
#include <xmmintrin.h>
#include <GL/gl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* IAT slots in cm0304.exe 4.1.5 (image base 0x400000, no relocations). */
#define IAT_STRETCHDIBITS          0xab605c
#define IAT_CHANGEDISPLAYSETTINGSA 0xab6244
#define IAT_SHOWCURSOR             0xab6260
#define IAT_SETCURSOR              0xab6264
#define IAT_CLOSEWINDOW            0xab62ac
#define IAT_REGISTERCLASSA         0xab628c
#define IAT_CREATEWINDOWEXA        0xab62c0
#define IAT_MOVEWINDOW             0xab62cc
#define IAT_GETCLIENTRECT          0xab62d4
#define IAT_PEEKMESSAGEA           0xab62dc
#define IAT_RTDYNAMICCAST          0xab61f8
#define CM0304_TIMESTAMP           0x4048900c

/* The game's window object, from GetWindowLongA(hwnd, 0). 0x9889c0 sets its layout limits. */
#define OBJ_MIN_W 0x7e
#define OBJ_MIN_H 0x80
#define OBJ_MAX_W 0x82
#define OBJ_MAX_H 0x84

enum filter { FILTER_SHARP, FILTER_SMOOTH, FILTER_NEAREST };

static struct {
    double zoom;        /* logical size = client size / zoom (windowed) */
    int borderless;     /* fullscreen without a display mode change */
    enum filter filter;
    int integer;        /* only scale by whole multiples */
    int opengl;         /* 0 = GDI StretchDIBits */
    int vsync;
    int max_fps;        /* cap on GPU redraws; dirty rects in between are only uploaded */
    int cursor_fix;     /* stop the pointer blinking (WM_SETCURSOR, ShowCursor pairs) */
    int fast_sqrt;      /* replace the game's bit-by-bit integer square roots */
    int fast_rand;      /* 1 = scale the RNG's draws with one imul instead of _allmul, 2 = also refill its state with SSE2 */
    int idle_wait;      /* block briefly in the message pump instead of spinning on idle screens */
    int fast_dyncast;   /* memoise dynamic_cast results per (vtable, types) */
    int prefetch;       /* prefetch shortlist candidates' data ahead of the AI's scan */
    int par_matches;    /* simulate background matches on this many threads */
    int skip_splash;    /* skip the logos at startup, as a key press during them does */
    int mouse_nav;      /* mouse back/forward buttons go back/forward through the screen history */
    int match_hl_speed; /* the watched match's highlights: 0 = stock (Real Time), 1 = Fast (3x), 2 = Very Fast (8x) */
    int match_speed_max; /* the watched match's Very Fast speed (stock 8) */
} cfg = {
    .zoom = 1.0, .borderless = 1, .filter = FILTER_SHARP, .opengl = 1, .max_fps = 120, .cursor_fix = 1,
    .fast_sqrt = 1, .fast_rand = 2, .idle_wait = 1, .fast_dyncast = 1, .prefetch = 1, .par_matches = 8,
    .skip_splash = 1, .mouse_nav = 1, .match_speed_max = 8,
};

static HWND game_hwnd;
static WNDPROC game_wndproc;
static int log_w, log_h;   /* framebuffer size, from the last present */
static int real_w, real_h; /* real client size */
static int bars_dirty;     /* client size changed: repaint the letterbox bars */
static double scale = 1.0; /* framebuffer to client scale */
static int off_x, off_y;   /* letterbox offset */
static DWORD game_thread_id;

static int (WINAPI *real_StretchDIBits)(HDC, int, int, int, int, int, int, int, int,
                                        const void *, const BITMAPINFO *, UINT, DWORD);
static LONG (WINAPI *real_ChangeDisplaySettingsA)(DEVMODEA *, DWORD);
static BOOL (WINAPI *real_CloseWindow)(HWND);
static ATOM (WINAPI *real_RegisterClassA)(const WNDCLASSA *);
static HWND (WINAPI *real_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
                                           HWND, HMENU, HINSTANCE, LPVOID);
static BOOL (WINAPI *real_MoveWindow)(HWND, int, int, int, int, BOOL);
static BOOL (WINAPI *real_GetClientRect)(HWND, RECT *);
static BOOL (WINAPI *real_PeekMessageA)(MSG *, HWND, UINT, UINT, UINT);
static int (WINAPI *real_ShowCursor)(BOOL);
static HCURSOR (WINAPI *real_SetCursor)(HCURSOR);

static int is_fullscreen(HWND hwnd)
{
    return !(GetWindowLongA(hwnd, GWL_STYLE) & WS_CAPTION);
}

static short obj_short(HWND hwnd, int off)
{
    char *obj = (char *)GetWindowLongA(hwnd, 0);
    return obj ? *(short *)(obj + off) : -1;
}

static int clamp_dim(int v, short lo, short hi)
{
    if (lo > 0 && v < lo)
        v = lo;
    if (hi > 0 && v > hi)
        v = hi;
    return v;
}

/* The client size cmscale tells the game: the real size divided by zoom, clamped to the game's layout limits. */
static void logical_size(HWND hwnd, int rw, int rh, LONG *lw, LONG *lh)
{
    *lw = clamp_dim((int)lround(rw / cfg.zoom), obj_short(hwnd, OBJ_MIN_W), obj_short(hwnd, OBJ_MAX_W));
    *lh = clamp_dim((int)lround(rh / cfg.zoom), obj_short(hwnd, OBJ_MIN_H), obj_short(hwnd, OBJ_MAX_H));
}

static void update_mapping_locked(void)
{
    if (log_w <= 0 || log_h <= 0 || real_w <= 0 || real_h <= 0) {
        scale = 1.0;
        off_x = off_y = 0;
        return;
    }
    scale = fmin((double)real_w / log_w, (double)real_h / log_h);
    if (cfg.integer && scale >= 1.0)
        scale = floor(scale);
    off_x = (real_w - (int)lround(log_w * scale)) / 2;
    off_y = (real_h - (int)lround(log_h * scale)) / 2;
}

static CRITICAL_SECTION *mapping_lock;

/* The render thread snapshots the mapping under the same lock. */
static void update_mapping(void)
{
    EnterCriticalSection(mapping_lock);
    update_mapping_locked();
    LeaveCriticalSection(mapping_lock);
}

static void borders(HWND hwnd, DWORD style, DWORD exstyle, int *bw, int *bh)
{
    RECT r = {0, 0, 0, 0};
    if (hwnd) {
        style = GetWindowLongA(hwnd, GWL_STYLE);
        exstyle = GetWindowLongA(hwnd, GWL_EXSTYLE);
    }
    AdjustWindowRectEx(&r, style, FALSE, exstyle);
    *bw = r.right - r.left;
    *bh = r.bottom - r.top;
}

/* Turns the outer window size the game asks for into one whose client area is zoom times larger, fitted to the
 * work area. */
static void scale_window(DWORD style, DWORD exstyle, int *x, int *y, int *w, int *h)
{
    if (!(style & WS_CAPTION)) {
        if (cfg.borderless) { /* fullscreen popup: cover the primary monitor */
            *x = *y = 0;
            *w = GetSystemMetrics(SM_CXSCREEN);
            *h = GetSystemMetrics(SM_CYSCREEN);
        }
        return;
    }
    if (cfg.zoom == 1.0)
        return;
    int bw, bh;
    RECT work;
    borders(NULL, style, exstyle, &bw, &bh);
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    double cw = (*w - bw) * cfg.zoom, ch = (*h - bh) * cfg.zoom;
    double fit = fmin(1.0, fmin((work.right - work.left - bw) / cw, (work.bottom - work.top - bh) / ch));
    *w = (int)(cw * fit) + bw;
    *h = (int)(ch * fit) + bh;
    *x = work.left + (work.right - work.left - *w) / 2;
    *y = work.top + (work.bottom - work.top - *h) / 2;
}

static LPARAM map_mouse(LPARAM lp)
{
    if (scale == 1.0 && off_x == 0 && off_y == 0)
        return lp;
    int x = (int)floor(((short)LOWORD(lp) - off_x) / scale);
    int y = (int)floor(((short)HIWORD(lp) - off_y) / scale);
    x = x < 0 ? 0 : x >= log_w ? log_w - 1 : x;
    y = y < 0 ? 0 : y >= log_h ? log_h - 1 : y;
    return MAKELPARAM(x, y);
}

/* ---- OpenGL renderer (docs/scaling.md) ---- */

#define GL_BGRA 0x80E1
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82

static GLuint (APIENTRY *glCreateShader)(GLenum);
static void (APIENTRY *glShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
static void (APIENTRY *glCompileShader)(GLuint);
static void (APIENTRY *glGetShaderiv)(GLuint, GLenum, GLint *);
static GLuint (APIENTRY *glCreateProgram)(void);
static void (APIENTRY *glAttachShader)(GLuint, GLuint);
static void (APIENTRY *glLinkProgram)(GLuint);
static void (APIENTRY *glGetProgramiv)(GLuint, GLenum, GLint *);
static void (APIENTRY *glUseProgram)(GLuint);
static GLint (APIENTRY *glGetUniformLocation)(GLuint, const char *);
static void (APIENTRY *glUniform1f)(GLint, GLfloat);
static void (APIENTRY *glUniform2f)(GLint, GLfloat, GLfloat);
static BOOL (APIENTRY *wglSwapIntervalEXT)(int);

/* Sharp-bilinear filter. It samples the nearest source pixel inside each pixel and blends linearly over one output
 * pixel at the pixel's edges. */
static const char *vs_src =
    "void main() { gl_Position = gl_Vertex; gl_TexCoord[0] = gl_MultiTexCoord0; }";
static const char *fs_src =
    "uniform sampler2D tex; uniform vec2 tex_size; uniform float scale;\n"
    "void main() {\n"
    "  vec2 texel = gl_TexCoord[0].xy * tex_size;\n"
    "  vec2 d = fract(texel) - 0.5;\n"
    "  float r = 0.5 - 0.5 / scale;\n"
    "  vec2 t = floor(texel) + 0.5 + (d - clamp(d, -r, r)) * scale;\n"
    "  gl_FragColor = vec4(texture2D(tex, t / tex_size).rgb, 1.0);\n"
    "}\n";

#define MAX_DIRTY 32

/* All GL calls run on a render thread, so a GL call that blocks can only stall the picture, never the game
 * thread's message pump. The game thread only copies dirty rects into a staging buffer. */
static struct {
    volatile LONG failed; /* the render thread couldn't create a context, so GDI draws */
    HWND hwnd;
    HANDLE thread, wake;
    CRITICAL_SECTION lock; /* guards stage, dirty, redraw and the mapping globals */
    unsigned *stage;       /* game thread's copy of the framebuffer */
    int stage_w, stage_h;
    RECT dirty[MAX_DIRTY]; /* framebuffer coordinates. Separate rects, because their bounding box often covers half the frame */
    int ndirty;
    int redraw;            /* client area changed */
    /* render thread only */
    HDC dc;
    HGLRC rc;
    GLuint tex, prog;
    GLint u_tex_size, u_scale;
    unsigned *upload;      /* private copy of stage, so no GL call runs under the lock */
    int tex_w, tex_h;      /* power-of-two texture holding the fb_w x fb_h framebuffer */
    int fb_w, fb_h;
    LARGE_INTEGER last_draw, freq;
} gl;

static GLuint gl_shader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    return ok ? sh : 0;
}

/* Without GLSL, or if the shader doesn't build, the sharp filter falls back to smooth. */
static void gl_build_program(void)
{
#define LOAD(f) (*(PROC *)&f = wglGetProcAddress(#f))
    if (!LOAD(glCreateShader) | !LOAD(glShaderSource) | !LOAD(glCompileShader) | !LOAD(glGetShaderiv) |
        !LOAD(glCreateProgram) | !LOAD(glAttachShader) | !LOAD(glLinkProgram) | !LOAD(glGetProgramiv) |
        !LOAD(glUseProgram) | !LOAD(glGetUniformLocation) | !LOAD(glUniform1f) | !LOAD(glUniform2f))
        return;
#undef LOAD
    GLuint vs = gl_shader(GL_VERTEX_SHADER, vs_src), fs = gl_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
        return;
    GLuint prog = glCreateProgram();
    GLint ok;
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
        return;
    gl.prog = prog;
    gl.u_tex_size = glGetUniformLocation(prog, "tex_size");
    gl.u_scale = glGetUniformLocation(prog, "scale");
}

static int gl_init(HWND hwnd)
{
    PIXELFORMATDESCRIPTOR pfd = {
        .nSize = sizeof pfd,
        .nVersion = 1,
        .dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        .iPixelType = PFD_TYPE_RGBA,
        .cColorBits = 24,
        .iLayerType = PFD_MAIN_PLANE,
    };
    gl.dc = GetDC(hwnd); /* the class is CS_OWNDC, so this stays valid */
    int pf = ChoosePixelFormat(gl.dc, &pfd);
    if (!pf || !SetPixelFormat(gl.dc, pf, &pfd) || !(gl.rc = wglCreateContext(gl.dc)) || !wglMakeCurrent(gl.dc, gl.rc))
        return 0;
    *(PROC *)&wglSwapIntervalEXT = wglGetProcAddress("wglSwapIntervalEXT");
    if (wglSwapIntervalEXT)
        wglSwapIntervalEXT(cfg.vsync);
    if (cfg.filter == FILTER_SHARP)
        gl_build_program();
    glGenTextures(1, &gl.tex);
    glBindTexture(GL_TEXTURE_2D, gl.tex);
    GLint f = cfg.filter == FILTER_NEAREST ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    if (gl.prog)
        glUseProgram(gl.prog);
    QueryPerformanceFrequency(&gl.freq);
    return 1;
}

static int pow2(int v)
{
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

static void gl_upload(const void *bits, int lw, int lh, int x, int y, int w, int h)
{
    if (lw != gl.fb_w || lh != gl.fb_h) {
        if (pow2(lw) != gl.tex_w || pow2(lh) != gl.tex_h) {
            gl.tex_w = pow2(lw);
            gl.tex_h = pow2(lh);
            void *zero = calloc((size_t)gl.tex_w * gl.tex_h, 4); /* black beyond the frame edge for linear sampling */
            /* The unpack state still describes the last dirty-rect upload: the old framebuffer width as
             * the row length, plus skips. If it stays set, the driver reads past the end of `zero` when the
             * framebuffer shrinks. Under Wine that fault came back as an error from glTexImage2D, and the
             * next GL call never returned. */
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, gl.tex_w, gl.tex_h, 0, GL_BGRA, GL_UNSIGNED_BYTE, zero);
            free(zero);
        }
        gl.fb_w = lw;
        gl.fb_h = lh;
        x = y = 0;
        w = lw;
        h = lh;
    }
    if (w <= 0 || h <= 0)
        return;
    glPixelStorei(GL_UNPACK_ROW_LENGTH, lw);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, x);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, y);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_BGRA, GL_UNSIGNED_BYTE, bits);
}

static void gl_draw(int rw, int rh, double sc, int ox, int oy)
{
    int dw = (int)lround(gl.fb_w * sc), dh = (int)lround(gl.fb_h * sc);
    float u = (float)gl.fb_w / gl.tex_w, v = (float)gl.fb_h / gl.tex_h;
    glViewport(0, 0, rw, rh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(ox, rh - oy - dh, dw, dh);
    if (gl.prog) {
        glUniform2f(gl.u_tex_size, (float)gl.tex_w, (float)gl.tex_h);
        glUniform1f(gl.u_scale, (float)fmax(sc, 1.0));
    }
    glBegin(GL_QUADS); /* the framebuffer is top-down, so texture row 0 is the top of the picture */
    glTexCoord2f(0, 0); glVertex2f(-1, 1);
    glTexCoord2f(u, 0); glVertex2f(1, 1);
    glTexCoord2f(u, v); glVertex2f(1, -1);
    glTexCoord2f(0, v); glVertex2f(-1, -1);
    glEnd();
    SwapBuffers(gl.dc);
    QueryPerformanceCounter(&gl.last_draw);
}

static void copy_rect(unsigned *dst, const unsigned *src, int stride, int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; y++)
        memcpy(dst + (size_t)y * stride + x0, src + (size_t)y * stride + x0, (size_t)(x1 - x0) * 4);
}

static DWORD WINAPI render_thread(void *unused)
{
    if (!gl_init(gl.hwnd)) {
        InterlockedExchange(&gl.failed, 1);
        return 0;
    }
    DWORD timeout = INFINITE;
    for (;;) {
        WaitForSingleObject(gl.wake, timeout);
        /* nothing to show while the window is hidden (during a mode switch) or minimised, so poll until it's back */
        if (!IsWindowVisible(gl.hwnd) || IsIconic(gl.hwnd)) {
            timeout = 50;
            continue;
        }
        timeout = INFINITE;
        if (cfg.max_fps > 0) { /* let further presents collect into this redraw */
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            LONGLONG wait_ms = (gl.freq.QuadPart / cfg.max_fps - (now.QuadPart - gl.last_draw.QuadPart)) * 1000 / gl.freq.QuadPart;
            if (wait_ms > 0)
                Sleep((DWORD)wait_ms);
        }
        EnterCriticalSection(&gl.lock);
        int sw = gl.stage_w, sh = gl.stage_h, redraw = gl.redraw, n = 0;
        int rw = real_w, rh = real_h, ox = off_x, oy = off_y;
        double sc = scale;
        RECT rects[MAX_DIRTY];
        if (gl.stage && (sw != gl.fb_w || sh != gl.fb_h)) {
            free(gl.upload);
            gl.upload = malloc((size_t)sw * sh * 4);
            rects[n++] = (RECT){0, 0, sw, sh};
        } else {
            n = gl.ndirty;
            memcpy(rects, gl.dirty, n * sizeof *rects);
        }
        if (gl.upload)
            for (int i = 0; i < n; i++)
                copy_rect(gl.upload, gl.stage, sw, rects[i].left, rects[i].top, rects[i].right, rects[i].bottom);
        gl.ndirty = 0;
        gl.redraw = 0;
        LeaveCriticalSection(&gl.lock);
        if (!gl.upload || (!n && !redraw))
            continue;
        for (int i = 0; i < n; i++)
            gl_upload(gl.upload, sw, sh, rects[i].left, rects[i].top, rects[i].right - rects[i].left,
                      rects[i].bottom - rects[i].top);
        gl_draw(rw, rh, sc, ox, oy);
    }
}

static int area(RECT r)
{
    return (r.right - r.left) * (r.bottom - r.top);
}

/* Adds r to the dirty list. The caller holds the lock. A rect that is already covered changes nothing. When the
 * list is full, r merges into whichever rect grows least. */
static void add_dirty(RECT r)
{
    int best = -1, best_growth = 0;
    for (int i = 0; i < gl.ndirty; i++) {
        RECT u;
        UnionRect(&u, &gl.dirty[i], &r);
        int growth = area(u) - area(gl.dirty[i]);
        if (growth == 0) /* already covered */
            return;
        if (best < 0 || growth < best_growth)
            best = i, best_growth = growth;
    }
    if (gl.ndirty < MAX_DIRTY)
        gl.dirty[gl.ndirty++] = r;
    else
        UnionRect(&gl.dirty[best], &gl.dirty[best], &r);
}

/* Game thread: copy the dirty rect into the staging buffer and wake the render thread.
 * Returns 0 if the present should go through GDI instead. */
static int gl_present(HDC hdc, const void *bits, int lw, int lh, int x, int y, int w, int h)
{
    if (!cfg.opengl || gl.failed)
        return 0;
    if (!gl.thread) {
        gl.hwnd = WindowFromDC(hdc);
        gl.wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        gl.thread = CreateThread(NULL, 0, render_thread, NULL, 0, NULL);
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > lw) w = lw - x;
    if (y + h > lh) h = lh - y;
    EnterCriticalSection(&gl.lock);
    if (lw != gl.stage_w || lh != gl.stage_h) {
        free(gl.stage);
        gl.stage = malloc((size_t)lw * lh * 4);
        gl.stage_w = lw;
        gl.stage_h = lh;
        x = y = 0, w = lw, h = lh;
        gl.ndirty = 0;
    }
    if (gl.stage && w > 0 && h > 0) {
        copy_rect(gl.stage, bits, lw, x, y, x + w, y + h);
        add_dirty((RECT){x, y, x + w, y + h});
    }
    LeaveCriticalSection(&gl.lock);
    SetEvent(gl.wake);
    return 1;
}

/* ---- cursor (docs/cursor.md) ----
 * The game draws a hardware cursor: 0x9e37a0 builds it with CreateIconIndirect, and the cursor object's
 * show method (vtable 0xc53e88 +4, 0x9e3720) does ShowCursor(FALSE); SetCursor(h); ShowCursor(TRUE).
 * Each pair briefly hides the pointer. The window class also has no cursor and the game doesn't handle
 * WM_SETCURSOR, so DefWindowProc blanks the pointer on every mouse move until the game sets it again.
 * Both show up as a blinking pointer. cmscale answers WM_SETCURSOR with the game's cursor and keeps the
 * game's ShowCursor count itself, applying it to the real cursor only from the message pump. */

static HCURSOR game_cursor, shown_cursor;
static int cursor_count, real_cursor_count; /* the game's ShowCursor display count, and the real one */

static int WINAPI hook_ShowCursor(BOOL show)
{
    return cursor_count += show ? 1 : -1;
}

static HCURSOR WINAPI hook_SetCursor(HCURSOR h)
{
    HCURSOR prev = game_cursor;
    game_cursor = h;
    if (h != shown_cursor)
        real_SetCursor(shown_cursor = h);
    return prev;
}

static void sync_cursor(void)
{
    while (cursor_count >= 0 && real_cursor_count < 0)
        real_cursor_count = real_ShowCursor(TRUE);
    while (cursor_count < 0 && real_cursor_count >= 0)
        real_cursor_count = real_ShowCursor(FALSE);
}

/* The main loop (0x99a980) polls the pump (0x988240, PeekMessage PM_NOREMOVE) with no wait, about 190k
 * times a second on an idle screen. After a run of back-to-back empty polls, wait up to 1 ms for input.
 * Day processing pumps only every 20-90 ms, which resets the run, so it is never delayed (docs/idle-wait.md). */
static void idle_wait(BOOL got)
{
    static LARGE_INTEGER freq, last;
    static int empty_run;
    LARGE_INTEGER now;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (got || (now.QuadPart - last.QuadPart) * 1000 > freq.QuadPart * 2)
        empty_run = 0;
    else if (++empty_run > 20)
        MsgWaitForMultipleObjects(0, NULL, FALSE, 1, QS_ALLINPUT);
    QueryPerformanceCounter(&last);
}

/* ---- mouse_nav (docs/mouse-nav.md) ----
 * The window's key handler (0x99b8e0, 'keyp') goes back or forward through the screen history (0x9c34b0,
 * index -3 / -2) on game key 12 or 13 (VK_LEFT / VK_RIGHT, 0x987e90) with Alt (mods bit 4). The wndproc
 * stores the key in the window object (+0x3c) and the Shift/Ctrl/Alt state from GetKeyState (+0x40) on
 * every message, and the next frame (0x9904b0) turns them into a 'keyp' message. In the stock game the
 * combination never arrives, because Alt+arrow is WM_SYSKEYDOWN, which the wndproc ignores. For a side
 * button cmscale stores the key and Alt the same way, and sets Alt again after each message until the frame
 * has taken the key. */

#define OBJ_KEY_CODE 0x3c
#define OBJ_KEY_MODS 0x40
#define GAME_KEY_LEFT 0x0c
#define GAME_KEY_RIGHT 0x0d
#define MODS_ALT 4

static char *nav_obj; /* window object with a side-button key the frame hasn't taken yet */

static void nav_press(HWND hwnd, int forward)
{
    char *obj = (char *)GetWindowLongA(hwnd, 0);
    if (!obj)
        return;
    *(int *)(obj + OBJ_KEY_CODE) = forward ? GAME_KEY_RIGHT : GAME_KEY_LEFT;
    *(obj + OBJ_KEY_MODS) = MODS_ALT;
    nav_obj = obj;
}

static void nav_keep_alt(void)
{
    int code = *(int *)(nav_obj + OBJ_KEY_CODE);
    if (code == GAME_KEY_LEFT || code == GAME_KEY_RIGHT)
        *(nav_obj + OBJ_KEY_MODS) = MODS_ALT;
    else
        nav_obj = NULL;
}

/* Applies the game's cursor visibility, then waits briefly on idle screens. */
static BOOL WINAPI hook_PeekMessageA(MSG *msg, HWND hwnd, UINT lo, UINT hi, UINT flags)
{
    if (cfg.cursor_fix)
        sync_cursor();
    BOOL got = real_PeekMessageA(msg, hwnd, lo, hi, flags);
    if (cfg.idle_wait)
        idle_wait(got);
    return got;
}

static LRESULT CALLBACK hook_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED)
            break;
        game_hwnd = hwnd;
        real_w = LOWORD(lp);
        real_h = HIWORD(lp);
        bars_dirty = 1;
        InvalidateRect(hwnd, NULL, FALSE);
        LONG lw, lh;
        logical_size(hwnd, real_w, real_h, &lw, &lh);
        lp = MAKELPARAM(lw, lh);
        if (!is_fullscreen(hwnd)) { /* the game reallocates its framebuffer to this size */
            log_w = lw;
            log_h = lh;
        }
        update_mapping();
        if (gl.thread) {
            EnterCriticalSection(&gl.lock);
            gl.redraw = 1;
            LeaveCriticalSection(&gl.lock);
            SetEvent(gl.wake);
        }
        break;
    case WM_GETMINMAXINFO: {
        /* Drop the game's size limits and keep the OS defaults. logical_size never tells the game less than
         * its minimum layout size, and the scaler shrinks the picture to fit instead. Some Wayland and X
         * window managers resize past a minimum track size, and Wine then draws a window bigger than the one
         * on screen, which crops the interface. */
        MINMAXINFO *mmi = (MINMAXINFO *)lp, os = *mmi;
        LRESULT r = CallWindowProcA(game_wndproc, hwnd, msg, wp, lp);
        *mmi = os;
        return r;
    }
    case WM_SETCURSOR:
        if (cfg.cursor_fix && LOWORD(lp) == HTCLIENT && game_cursor) {
            real_SetCursor(shown_cursor = game_cursor);
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        lp = map_mouse(lp);
        break;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
        if (cfg.mouse_nav) {
            nav_press(hwnd, GET_XBUTTON_WPARAM(wp) == XBUTTON2);
            return TRUE;
        }
        break;
    case WM_XBUTTONUP:
        if (cfg.mouse_nav)
            return TRUE;
        break;
    case WM_APPCOMMAND: /* browser keys on keyboards, and side buttons some drivers send this way */
        if (cfg.mouse_nav && (GET_APPCOMMAND_LPARAM(lp) == APPCOMMAND_BROWSER_BACKWARD ||
                              GET_APPCOMMAND_LPARAM(lp) == APPCOMMAND_BROWSER_FORWARD)) {
            nav_press(hwnd, GET_APPCOMMAND_LPARAM(lp) == APPCOMMAND_BROWSER_FORWARD);
            return TRUE;
        }
        break;
    }
    LRESULT r = CallWindowProcA(game_wndproc, hwnd, msg, wp, lp);
    if (nav_obj)
        nav_keep_alt();
    return r;
}

/* ---- GDI renderer ---- */

static void paint_bars(HDC hdc, int dw, int dh)
{
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT bars[4] = {
        {0, 0, real_w, off_y},
        {0, off_y + dh, real_w, real_h},
        {0, off_y, off_x, off_y + dh},
        {off_x + dw, off_y, real_w, off_y + dh},
    };
    for (int i = 0; i < 4; i++)
        if (bars[i].right > bars[i].left && bars[i].bottom > bars[i].top)
            FillRect(hdc, &bars[i], black);
    bars_dirty = 0;
}

static int WINAPI hook_StretchDIBits(HDC hdc, int xd, int yd, int wd, int hd, int xs, int ys, int ws, int hs,
                                     const void *bits, const BITMAPINFO *bmi, UINT usage, DWORD rop)
{
    int lw = bmi->bmiHeader.biWidth, lh = abs(bmi->bmiHeader.biHeight);
    if (lw != log_w || lh != log_h) {
        log_w = lw;
        log_h = lh;
        update_mapping();
    }
    if (gl_present(hdc, bits, lw, lh, xd, yd, wd, hd))
        return hd;
    if (gl.failed == 1) { /* GL never came up, so GDI takes over, starting with a full frame */
        gl.failed = 2;
        bars_dirty = 1;
    }
    if (scale == 1.0) {
        if (bars_dirty)
            paint_bars(hdc, lw, lh);
        return real_StretchDIBits(hdc, xd + off_x, yd + off_y, wd, hd, xs, ys, ws, hs, bits, bmi, usage, rop);
    }

    /* Stretching a dirty rect on its own leaves seams, so stretch the whole frame but clip it to the
     * scaled dirty rect plus a filter margin. GDI computes only the pixels inside the clip. Under Wine a
     * full 1024x768 to 2400x1800 HALFTONE stretch costs ~33 ms, and one clipped to 240x100 ~0.2 ms. */
    int dw = (int)lround(lw * scale), dh = (int)lround(lh * scale);
    int margin = (int)ceil(scale) + 1;
    int saved = SaveDC(hdc);
    if (!bars_dirty)
        IntersectClipRect(hdc, off_x + (int)floor(xd * scale) - margin, off_y + (int)floor(yd * scale) - margin,
                          off_x + (int)ceil((xd + wd) * scale) + margin, off_y + (int)ceil((yd + hd) * scale) + margin);
    int mode = cfg.filter == FILTER_NEAREST ? COLORONCOLOR : HALFTONE;
    SetStretchBltMode(hdc, mode);
    if (mode == HALFTONE)
        SetBrushOrgEx(hdc, 0, 0, NULL);
    real_StretchDIBits(hdc, off_x, off_y, dw, dh, 0, 0, lw, lh, bits, bmi, usage, rop);
    RestoreDC(hdc, saved);
    if (bars_dirty)
        paint_bars(hdc, dw, dh);
    return hd;
}

static BOOL WINAPI hook_GetClientRect(HWND hwnd, RECT *r)
{
    BOOL ok = real_GetClientRect(hwnd, r);
    if (ok && hwnd == game_hwnd)
        logical_size(hwnd, r->right, r->bottom, &r->right, &r->bottom);
    return ok;
}

static ATOM WINAPI hook_RegisterClassA(const WNDCLASSA *wc)
{
    if (HIWORD(wc->lpszClassName) && !strcmp(wc->lpszClassName, "main")) {
        WNDCLASSA c = *wc;
        game_wndproc = c.lpfnWndProc;
        c.lpfnWndProc = hook_wndproc;
        c.style |= CS_OWNDC; /* one DC for the window's lifetime, needed by the OpenGL context */
        return real_RegisterClassA(&c);
    }
    return real_RegisterClassA(wc);
}

static HWND WINAPI hook_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w,
                                        int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    if (HIWORD(cls) && !strcmp(cls, "main")) {
        scale_window(style, ex, &x, &y, &w, &h);
        game_hwnd = real_CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
        return game_hwnd;
    }
    return real_CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
}

static BOOL WINAPI hook_MoveWindow(HWND hwnd, int x, int y, int w, int h, BOOL repaint)
{
    if (hwnd == game_hwnd)
        scale_window(GetWindowLongA(hwnd, GWL_STYLE), GetWindowLongA(hwnd, GWL_EXSTYLE), &x, &y, &w, &h);
    return real_MoveWindow(hwnd, x, y, w, h, repaint);
}

static LONG WINAPI hook_ChangeDisplaySettingsA(DEVMODEA *dm, DWORD flags)
{
    return DISP_CHANGE_SUCCESSFUL;
}

/* In fullscreen the game minimises itself when it loses focus, which is pointless without a mode switch. */
static BOOL WINAPI hook_CloseWindow(HWND hwnd)
{
    return hwnd == game_hwnd ? TRUE : real_CloseWindow(hwnd);
}

/* ---- fast_sqrt: the match engine's distance function (docs/fast-sqrt.md) ----
 * Day processing spends ~18% of its time in 0x8d1cc0, distance(dx, dy) = floor(sqrt(dx*dx + dy*dy)). It
 * uses a branchless bit-by-bit loop, and returns 4095 when the 32-bit sum is 2^24 or more. 0x8d1d50 is the
 * same algorithm for a full 32-bit isqrt. cmscale replaces both with SSE2 sqrtsd, which gives the same
 * result for every input: a double holds n exactly and sqrtsd rounds correctly, so the floor is exact. SSE
 * keeps the game's x87 state and precision control out of it. */

#define FN_HYPOT24 0x8d1cc0
#define FN_ISQRT32 0x8d1d50

static int __stdcall fast_hypot24(int dx, int dy)
{
    unsigned n = (unsigned)dx * (unsigned)dx + (unsigned)dy * (unsigned)dy;
    if (n >= 1u << 24)
        return 4095;
    __m128d d = _mm_cvtsi32_sd(_mm_setzero_pd(), (int)n);
    return _mm_cvttsd_si32(_mm_sqrt_sd(d, d));
}

static unsigned __stdcall fast_isqrt32(unsigned n)
{
    /* converts the unsigned n to a double exactly, in two halves */
    __m128d d = _mm_set_sd((double)(int)(n >> 1) * 2.0 + (double)(int)(n & 1));
    return (unsigned)_mm_cvttsd_si32(_mm_sqrt_sd(d, d));
}

/* Overwrites the start of fn with a jmp to repl, if its first bytes are as expected. */
static int patch_jmp(DWORD fn, const void *expect, int len, void *repl)
{
    if (memcmp((void *)fn, expect, len))
        return 0;
    DWORD old;
    VirtualProtect((void *)fn, 5, PAGE_EXECUTE_READWRITE, &old);
    *(BYTE *)fn = 0xe9;
    *(DWORD *)(fn + 1) = (DWORD)repl - (fn + 5);
    VirtualProtect((void *)fn, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)fn, 5);
    return 1;
}

/* ---- fast_rand: the RNG's scaling step (docs/fast-rand.md) ----
 * 0x983a50 is the game's Mersenne Twister rand(n) (thiscall, ret 4). It tempers the next word r, then
 * returns (r31 * (int64)(int)(2n)) >> 32 in eax (edx = its sign), with r31 = r & 0x7fffffff. The tail
 * at 0x983a97 builds both operands as 64-bit values on the stack and calls _allmul (0xa41760), which is
 * ~2.4% of match-day processing on its own. One signed 32-by-32-bit imul gives the same 64-bit product, because
 * the operands are the sign extensions of r31 and int(2n). cmscale writes the new tail over the old one in
 * place. It leaves eax, edx and ecx exactly as the original did (_allmul leaves int(2n) in ecx). */

#define FN_RAND_TAIL 0x983a97

static const BYTE rand_tail_orig[31] = {
    0x8b, 0x44, 0x24, 0x04,             /* mov eax,[esp+4]   n */
    0x03, 0xc0,                         /* add eax,eax */
    0x99,                               /* cdq */
    0x52, 0x50,                         /* push edx; push eax   (int64)(int)(2n) */
    0x81, 0xe1, 0xff, 0xff, 0xff, 0x7f, /* and ecx,0x7fffffff */
    0x6a, 0x00, 0x51,                   /* push 0; push ecx     (int64)r31 */
    0xe8, 0xb2, 0xdc, 0x0b, 0x00,       /* call _allmul */
    0x8b, 0xc2,                         /* mov eax,edx */
    0xc1, 0xfa, 0x1f,                   /* sar edx,31 */
    0xc2, 0x04, 0x00,                   /* ret 4 */
};
static const BYTE rand_tail_fast[31] = {
    0x8b, 0x44, 0x24, 0x04,             /* mov eax,[esp+4] */
    0x03, 0xc0,                         /* add eax,eax */
    0x81, 0xe1, 0xff, 0xff, 0xff, 0x7f, /* and ecx,0x7fffffff */
    0x91,                               /* xchg eax,ecx         eax = r31, ecx = int(2n) */
    0xf7, 0xe9,                         /* imul ecx             edx:eax = r31 * int(2n) */
    0x8b, 0xc2,                         /* mov eax,edx */
    0xc1, 0xfa, 0x1f,                   /* sar edx,31 */
    0xc2, 0x04, 0x00,                   /* ret 4 */
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
};

/* 0x9839a0 regenerates the generator's 624 words (thiscall, plain ret) with the standard MT19937 recurrence
 *   mt[i] = mt[(i + 397) % 624] ^ (y >> 1) ^ (y & 1 ? 0x9908b0df : 0),  y = (mt[i] & 0x80000000) | (mt[i + 1] & 0x7fffffff)
 * in three runs: i < 227 (source words not yet updated), 227 <= i < 623 (source words 227 back, already updated) and
 * i = 623 (next word is the new mt[0]). Then it sets the index at +0x9c0 to 625. Within each run four
 * consecutive words never depend on each other, so SSE2 does them together. rand (0x983a5a) relies on ecx
 * surviving the call, so a thunk keeps it and leaves eax and edx as the original's last iteration does. */

#define FN_RAND_REGEN 0x9839a0

static inline __m128i mt_step4(const DWORD *cur, const DWORD *src)
{
    __m128i c = _mm_loadu_si128((const __m128i *)cur), n = _mm_loadu_si128((const __m128i *)(cur + 1));
    __m128i y = _mm_or_si128(_mm_and_si128(c, _mm_set1_epi32(0x80000000)), _mm_and_si128(n, _mm_set1_epi32(0x7fffffff)));
    __m128i mag = _mm_and_si128(_mm_srai_epi32(_mm_slli_epi32(n, 31), 31), _mm_set1_epi32(0x9908b0df));
    return _mm_xor_si128(_mm_xor_si128(_mm_loadu_si128((const __m128i *)src), _mm_srli_epi32(y, 1)), mag);
}

static inline DWORD mt_step1(DWORD cur, DWORD next, DWORD src)
{
    DWORD y = (cur & 0x80000000) | (next & 0x7fffffff);
    return src ^ (y >> 1) ^ (-(next & 1) & 0x9908b0df);
}

void __stdcall mt_regen_sse2(DWORD *mt) __asm__("mt_regen_sse2");
void __stdcall mt_regen_sse2(DWORD *mt)
{
    int i = 0;
    for (; i + 4 <= 227; i += 4)
        _mm_storeu_si128((__m128i *)(mt + i), mt_step4(mt + i, mt + i + 397));
    for (; i < 227; i++)
        mt[i] = mt_step1(mt[i], mt[i + 1], mt[i + 397]);
    for (; i < 623; i += 4) /* 396 words, exactly 99 steps */
        _mm_storeu_si128((__m128i *)(mt + i), mt_step4(mt + i, mt + i - 227));
    mt[623] = mt_step1(mt[623], mt[0], mt[396]);
    mt[624] = 625; /* the index at +0x9c0 */
}

void fast_mt_regen(void) __asm__("fast_mt_regen");
__asm__("fast_mt_regen:\n"
        "push %ecx\npush %ecx\ncall mt_regen_sse2\npop %ecx\n"
        "lea 0x9bc(%ecx),%eax\n"                          /* the original's pointer, at mt[623] */
        "mov (%ecx),%edx\nand $1,%edx\nneg %edx\nand $0x9908b0df,%edx\n" /* its last mag term, from the new mt[0] */
        "ret\n");

/* Overwrites len bytes at va with repl, if they are orig. */
static int patch_bytes(DWORD va, const BYTE *orig, const BYTE *repl, int len)
{
    if (memcmp((void *)va, orig, len))
        return 0;
    DWORD old;
    VirtualProtect((void *)va, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *)va, repl, len);
    VirtualProtect((void *)va, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)va, len);
    return 1;
}

/* ---- skip_splash (docs/skip-splash.md) ----
 * Startup (0x44a9b0) installs the logo player 0x449a90 as the main window's per-frame callback at +0x48, and
 * 0x997490 calls it until it returns false. It plays a table of four logos, each 3 s with fades: a black
 * second, SI, Eidos and Let's Kick Racism Out Of Football. The table marks all four as skippable. Input sets
 * the skip flag 0xd4c78a, which the player's first call clears. This callback sets the flag after every
 * call, so the player takes its own skip path at once, as if a key had been pressed when the first logo
 * appeared. */

#define FN_SPLASH 0x449a90
#define SPLASH_INSTALL 0x44ab65 /* mov dword [edi+0x48],0x449a90 */
#define SPLASH_SKIP (*(volatile BYTE *)0xd4c78a)

static char skip_splash(void)
{
    char more = ((char (*)(void))FN_SPLASH)();
    SPLASH_SKIP = 1;
    return more;
}

static int install_skip_splash(void)
{
    BYTE repl[7] = {0xc7, 0x47, 0x48};
    *(DWORD *)(repl + 3) = (DWORD)skip_splash;
    return patch_bytes(SPLASH_INSTALL, (const BYTE *)"\xc7\x47\x48\x90\x9a\x44\x00", repl, sizeof repl);
}

/* ---- hooks on the match simulation and on 0x61b7b0, for par_matches ---- */

#define FN_MATCH_SIM 0x8e4350
BYTE *sim_tramp __asm__("sim_tramp");

static void par_drain(void);

void __cdecl play_begin(void) __asm__("play_begin");
void __cdecl play_begin(void)
{
    if (GetCurrentThreadId() == game_thread_id)
        par_drain(); /* the game thread plays a fixture itself, so finish the ones in flight first */
}

/* 0x61b7b0 (sets up, simulates and records one match): its first 8 bytes (push -1; mov eax,fs:[0]) move to a
 * stub that calls play_begin first, keeping every register and the flags. */
static int install_play_hook(void)
{
    BYTE *stub = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub)
        return 0;
    stub[0] = 0x9c, stub[1] = 0x60, stub[2] = 0xe8; /* pushfd; pushad; call play_begin */
    *(DWORD *)(stub + 3) = (DWORD)play_begin - (DWORD)(stub + 7);
    stub[7] = 0x61, stub[8] = 0x9d; /* popad; popfd */
    memcpy(stub + 9, "\x6a\xff\x64\xa1\x00\x00\x00\x00\xe9", 9);
    *(DWORD *)(stub + 18) = 0x61b7b8 - (DWORD)(stub + 22);
    return patch_jmp(0x61b7b0, "\x6a\xff\x64\xa1\x00\x00\x00\x00", 8, stub);
}

typedef int (__attribute__((thiscall)) *sim_fn)(DWORD match, DWORD, DWORD, DWORD, DWORD, DWORD);
struct par_worker;
static struct par_worker *par_current(void);
static int par_simulate(struct par_worker *w, DWORD match, const DWORD *a);

int __cdecl sim_run(DWORD ret, DWORD match, DWORD a0, DWORD a1, DWORD a2, DWORD a3, DWORD a4) __asm__("sim_run");
int __cdecl sim_run(DWORD ret, DWORD match, DWORD a0, DWORD a1, DWORD a2, DWORD a3, DWORD a4)
{
    /* only the background matches that 0x61b7b0 plays (0x61bb8d), not the human manager's watched match,
     * which can call back into the UI */
    struct par_worker *w;
    if (ret == 0x61bb92 && (w = par_current())) {
        DWORD args[5] = {a0, a1, a2, a3, a4};
        return par_simulate(w, match, args);
    }
    return ((sim_fn)sim_tramp)(match, a0, a1, a2, a3, a4);
}

/* The jmp at 0x8e4350 lands here. It calls sim_run(return address, match, the five stack arguments). */
void sim_entry(void) __asm__("sim_entry");
__asm__("sim_entry:\n"
        "push %ebp\nmov %esp,%ebp\npush %ecx\n"
        "pushl 0x18(%ebp)\npushl 0x14(%ebp)\npushl 0x10(%ebp)\npushl 0xc(%ebp)\npushl 0x8(%ebp)\n"
        "pushl -4(%ebp)\npushl 4(%ebp)\ncall sim_run\nadd $28,%esp\n"
        "leave\nret $0x14\n");

/* Diverts 0x8e4350 through sim_entry. */
static int install_sim_hook(void)
{
    sim_tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!sim_tramp)
        return 0;
    memcpy(sim_tramp, "\x83\xec\x2c\x53\x55\xe9", 6); /* sub esp,0x2c; push ebx; push ebp; jmp 0x8e4355 */
    *(DWORD *)(sim_tramp + 6) = (FN_MATCH_SIM + 5) - (DWORD)(sim_tramp + 10);
    return patch_jmp(FN_MATCH_SIM, "\x83\xec\x2c\x53\x55\x56\x8b\xf1", 8, sim_entry);
}

/* Retargets a `call rel32` at site (which must call `expect`) to target. */
static int patch_call(DWORD site, DWORD expect, void *target)
{
    if (*(BYTE *)site != 0xe8 || site + 5 + *(int *)(site + 1) != expect)
        return 0;
    DWORD old;
    VirtualProtect((void *)site, 5, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD *)(site + 1) = (DWORD)target - (site + 5);
    VirtualProtect((void *)site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)site, 5);
    return 1;
}

/* ---- par_matches=K: simulating a day's background matches on K threads (docs/par-matches.md) ----
 * The simulation (0x8e4350) keeps its state in its own match object, but shares some state with every
 * other match and with the rest of the game. cmscale gives each thread its own copy of that state, or locks it:
 *
 * - 0xdc0118 and 0xdc011c (1 145 instructions, from tools/par_sites.py): each instruction becomes a jmp to a
 *   thunk that runs the same instruction with an fs: override and a displacement into the TEB's TlsSlots, so
 *   each thread reads and writes its own copy. jmp leaves the flags alone, so a cmp still feeds the jcc
 *   after it.
 * - The function-local static PLAYER_TACTICS at 0xeedbd0, which 0x911ab0 fills and then reads within one
 *   call (2.4% of match processing). cmscale runs 0x911ab0 under a lock.
 * - The pool of per-person track buffers. 0x8d8090 takes one and 0x8d38f0 returns it, and cmscale runs both
 *   under a lock.
 * - Global RNGs such as 0xeef510 and 0xd52ce8. The simulation draws from 0xeef510 at 0x95c6a2. cmscale locks
 *   every draw through 0x983ae0 from an RNG in the exe's data rather than in a match object.
 * - The text system. Its 14 string heap classes (COMMON_STRING_SCRATCH_HEAP, ..._MALLOC_HEAP and so on) and
 *   the formatting code around them share state. Every non-trivial method in the heaps' vtables, and every call
 *   from the match engine into a text function (par_text_calls, from tools/par_sites.py), goes through a
 *   forwarder that holds one lock around the call. */

#include "par_sites.h"

static CRITICAL_SECTION par_tactics_lock, par_pool_lock, par_rng_lock;

/* A copy of fn's first len bytes (whole instructions, nothing relative), then a jmp to fn + len. */
static BYTE *make_tramp(DWORD fn, int len)
{
    BYTE *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (t) {
        memcpy(t, (void *)fn, len);
        t[len] = 0xe9;
        *(DWORD *)(t + len + 1) = (fn + len) - (DWORD)(t + len + 5);
    }
    return t;
}

/* Machine code for a thiscall function with nargs stack arguments that calls tramp (the original) while
 * holding lock: enter, re-push the arguments, call, leave, ret nargs*4. Keeps ecx for the call, and eax and
 * edx from it. */
static BYTE *make_locked_thiscall(BYTE *tramp, int nargs, CRITICAL_SECTION *lock)
{
    static void (WINAPI *enter)(CRITICAL_SECTION *) = EnterCriticalSection, (WINAPI *leave)(CRITICAL_SECTION *) = LeaveCriticalSection;
    BYTE *w = VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE), *p = w;
    if (!w)
        return NULL;
    *p++ = 0x55, *p++ = 0x89, *p++ = 0xe5, *p++ = 0x51;              /* push ebp; mov ebp,esp; push ecx */
    *p++ = 0x68, *(DWORD *)p = (DWORD)lock, p += 4;                  /* push lock */
    *p++ = 0xff, *p++ = 0x15, *(DWORD *)p = (DWORD)&enter, p += 4;   /* call [enter] */
    *p++ = 0x8b, *p++ = 0x4d, *p++ = 0xfc;                           /* mov ecx,[ebp-4] */
    for (int i = nargs - 1; i >= 0; i--)
        *p++ = 0xff, *p++ = 0x75, *p++ = 8 + 4 * i;                  /* push [ebp+8+4i] */
    *p++ = 0xe8, *(DWORD *)p = (DWORD)tramp - (DWORD)(p + 4), p += 4; /* call original */
    *p++ = 0x50, *p++ = 0x52;                                        /* push eax; push edx */
    *p++ = 0x68, *(DWORD *)p = (DWORD)lock, p += 4;                  /* push lock */
    *p++ = 0xff, *p++ = 0x15, *(DWORD *)p = (DWORD)&leave, p += 4;   /* call [leave] */
    *p++ = 0x5a, *p++ = 0x58, *p++ = 0xc9;                           /* pop edx; pop eax; leave */
    *p++ = 0xc2, *(WORD *)p = nargs * 4;                             /* ret nargs*4 */
    return w;
}

static void *(__cdecl *pool_get)(void);
static void (__cdecl *pool_put)(void *);

static void *__cdecl locked_pool_get(void)
{
    EnterCriticalSection(&par_pool_lock);
    void *p = pool_get();
    LeaveCriticalSection(&par_pool_lock);
    return p;
}

static void __cdecl locked_pool_put(void *b)
{
    EnterCriticalSection(&par_pool_lock);
    pool_put(b);
    LeaveCriticalSection(&par_pool_lock);
}

/* 0x983ae0 (add ecx,4; jmp 0x983a50) becomes a jmp here */
CRITICAL_SECTION *par_rng_lock_p __asm__("par_rng_lock_p") = &par_rng_lock;
void par_rand(void) __asm__("par_rand");
__asm__("par_rand:\n"
        "cmp $0xd0a000,%ecx\njb 1f\ncmp $0xf96000,%ecx\njae 1f\n"
        "push %ecx\npushl par_rng_lock_p\ncall *__imp__EnterCriticalSection@4\npop %ecx\n"
        "pushl 4(%esp)\nadd $4,%ecx\nmov $0x983a50,%eax\ncall *%eax\n"
        "push %eax\npush %edx\npushl par_rng_lock_p\ncall *__imp__LeaveCriticalSection@4\npop %edx\npop %eax\n"
        "ret $4\n"
        "1:\nadd $4,%ecx\nmov $0x983a50,%eax\njmp *%eax\n");

/* vtables of the string heap classes (COMMON_STRING_*, from the exe's RTTI), 10 slots each */
static const DWORD par_string_vtables[] = {0xc4d8c8, 0xc4d8f4, 0xc4d920, 0xc4d94c, 0xc4d978, 0xc4d9a4, 0xc4d9d0,
                                           0xc4d9fc, 0xc4da28, 0xc4da54, 0xc4da80, 0xc4daac, 0xc4dad8, 0xc4db04};
static CRITICAL_SECTION par_string_lock;
/* the callers' return addresses. Only the thread that holds par_string_lock touches them */
static DWORD par_string_ret[256], par_string_depth;

/* Machine code that calls fn with lock held, whatever fn's arguments and calling convention: it keeps the
 * caller's return address aside, so fn sees the caller's stack, and returns eax, edx and st(0) from fn. */
static BYTE *make_locked_forwarder(DWORD fn, CRITICAL_SECTION *lock)
{
    static void (WINAPI *enter)(CRITICAL_SECTION *) = EnterCriticalSection, (WINAPI *leave)(CRITICAL_SECTION *) = LeaveCriticalSection;
    BYTE *w = VirtualAlloc(NULL, 96, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE), *p = w;
    if (!w)
        return NULL;
    *p++ = 0x51;                                                          /* push ecx */
    *p++ = 0x68, *(DWORD *)p = (DWORD)lock, p += 4;                       /* push lock */
    *p++ = 0xff, *p++ = 0x15, *(DWORD *)p = (DWORD)&enter, p += 4;        /* call [enter] */
    *p++ = 0x59, *p++ = 0x58;                                             /* pop ecx; pop eax (return address) */
    *p++ = 0x8b, *p++ = 0x15, *(DWORD *)p = (DWORD)&par_string_depth, p += 4;  /* mov edx,[depth] */
    *p++ = 0x89, *p++ = 0x04, *p++ = 0x95, *(DWORD *)p = (DWORD)par_string_ret, p += 4; /* mov [ret+edx*4],eax */
    *p++ = 0x42;                                                          /* inc edx */
    *p++ = 0x89, *p++ = 0x15, *(DWORD *)p = (DWORD)&par_string_depth, p += 4;  /* mov [depth],edx */
    *p++ = 0xe8, *(DWORD *)p = fn - (DWORD)(p + 4), p += 4;               /* call fn */
    *p++ = 0x8b, *p++ = 0x0d, *(DWORD *)p = (DWORD)&par_string_depth, p += 4;  /* mov ecx,[depth] */
    *p++ = 0x49;                                                          /* dec ecx */
    *p++ = 0x89, *p++ = 0x0d, *(DWORD *)p = (DWORD)&par_string_depth, p += 4;  /* mov [depth],ecx */
    *p++ = 0xff, *p++ = 0x34, *p++ = 0x8d, *(DWORD *)p = (DWORD)par_string_ret, p += 4; /* push [ret+ecx*4] */
    *p++ = 0x50, *p++ = 0x52;                                             /* push eax; push edx */
    *p++ = 0x68, *(DWORD *)p = (DWORD)lock, p += 4;                       /* push lock */
    *p++ = 0xff, *p++ = 0x15, *(DWORD *)p = (DWORD)&leave, p += 4;        /* call [leave] */
    *p++ = 0x5a, *p++ = 0x58, *p++ = 0xc3;                                /* pop edx; pop eax; ret */
    return w;
}

static int install_par_string_lock(void)
{
    DWORD fns[160], wrapped[160], old;
    int n = 0;
    InitializeCriticalSection(&par_string_lock);
    for (unsigned v = 0; v < sizeof par_string_vtables / sizeof *par_string_vtables; v++) {
        DWORD *vt = (DWORD *)par_string_vtables[v];
        if (!VirtualProtect(vt, 40, PAGE_READWRITE, &old))
            return 0;
        for (int i = 0; i < 10; i++) {
            BYTE *f = (BYTE *)vt[i];
            if (f[0] == 0xc3 || f[0] == 0xc2 || (f[0] == 0x32 && f[1] == 0xc0 && f[2] == 0xc3))
                continue; /* empty stubs, shared with other classes */
            int k = 0;
            while (k < n && fns[k] != vt[i])
                k++;
            if (k == n) {
                fns[n] = vt[i];
                if (!(wrapped[n++] = (DWORD)make_locked_forwarder(vt[i], &par_string_lock)))
                    return 0;
            }
            vt[i] = wrapped[k];
        }
        VirtualProtect(vt, 40, old, &old);
    }
    /* the engine's calls into text functions */
    BYTE *fwd[sizeof par_text_functions / sizeof *par_text_functions] = {0};
    int calls = sizeof par_text_calls / sizeof *par_text_calls;
    for (int i = 0; i < calls; i++) {
        const struct par_text_call *c = &par_text_calls[i];
        DWORD fn = par_text_functions[c->function];
        if (!fwd[c->function] && !(fwd[c->function] = make_locked_forwarder(fn, &par_string_lock)))
            return 0;
        if (!patch_call(c->addr, fn, fwd[c->function]))
            return 0;
    }
    return 1;
}

static int install_par_locks(void)
{
    InitializeCriticalSection(&par_tactics_lock);
    InitializeCriticalSection(&par_pool_lock);
    InitializeCriticalSection(&par_rng_lock);
    BYTE *t = make_tramp(0x911ab0, 8), *w = t ? make_locked_thiscall(t, 6, &par_tactics_lock) : NULL;
    pool_get = (void *)make_tramp(0x8d8090, 8);
    pool_put = (void *)make_tramp(0x8d38f0, 7);
    return w && pool_get && pool_put &&
           patch_jmp(0x911ab0, "\x6a\xff\x64\xa1\x00\x00\x00\x00", 8, w) &&
           patch_jmp(0x8d8090, "\x56\x57\x8b\x3d\xe4\xda\xee\x00", 8, locked_pool_get) &&
           patch_jmp(0x8d38f0, "\x53\x8b\x1d\xe4\xda\xee\x00", 7, locked_pool_put) &&
           patch_jmp(0x983ae0, "\x83\xc1\x04\xe9", 4, par_rand) && install_par_string_lock();
}

static const DWORD par_globals[2] = {0xdc0118, 0xdc011c};
static DWORD par_tls_index[2];

static int install_par_tls(void)
{
    int n = sizeof par_sites / sizeof *par_sites;
    for (int k = 0; k < 2; k++) {
        par_tls_index[k] = TlsAlloc();
        if (par_tls_index[k] >= 64) /* only the first 64 slots live in the TEB itself */
            return 0;
        TlsSetValue(par_tls_index[k], (void *)*(DWORD *)par_globals[k]);
    }
    for (int i = 0; i < n; i++)
        if (*(DWORD *)(par_sites[i].addr + par_sites[i].disp) != par_globals[par_sites[i].global])
            return 0;
    BYTE *thunks = VirtualAlloc(NULL, n * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE), *t = thunks;
    DWORD old;
    if (!thunks || !VirtualProtect((void *)0x401000, 0x6b5000, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    for (int i = 0; i < n; i++, t += 16) {
        const struct par_site *p = &par_sites[i];
        BYTE *code = (BYTE *)p->addr;
        t[0] = 0x64; /* fs: */
        memcpy(t + 1, code, p->len);
        *(DWORD *)(t + 1 + p->disp) = 0xe10 + 4 * par_tls_index[p->global]; /* TEB TlsSlots[index] */
        t[1 + p->len] = 0xe9;
        *(DWORD *)(t + 2 + p->len) = (p->addr + p->len) - (DWORD)(t + 6 + p->len);
        code[0] = 0xe9;
        *(DWORD *)(code + 1) = (DWORD)t - (p->addr + 5);
        memset(code + 5, 0xcc, p->len - 5);
    }
    VirtualProtect((void *)0x401000, 0x6b5000, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)0x401000, 0x6b5000);
    return 1;
}

/* Workers. The day loop 0x61cb90 plays one background fixture per pass with `call 0x61b7b0` at 0x61cdd4,
 * which sets up, simulates (0x8e4350) and records the match. The day loop then shows the score with 0x617720.
 * cmscale points that call at play_async. A worker runs the whole of 0x61b7b0 on its own stack and stops at
 * the simulation (sim_run). The game thread waits for that, then returns 0, so the day loop goes on and draws
 * its screen until the next fixture. Every K fixtures (K workers), and at the last one, the batch runs. The
 * K simulations run together while the game thread waits, then each worker records its fixture in turn and
 * the game thread shows its score. So simulations only ever run alongside each other, never alongside other
 * game code, because the game thread's screen reads strings that the simulations' string heaps can move.
 * Whenever the game thread enters 0x61b7b0 itself, the pending batch runs first. The order is the same in
 * every run.
 *
 * Each worker runs with the game thread's x87 control word and MXCSR (the game computes in x87 floats), on a
 * stack as large as the game thread's (1 MB). */
#define PAR_MAX 16
static struct par_worker {
    HANDLE thread, run, handoff, go_sim, sim_done, go_post;
    DWORD id, self, args[4];
    int result, finished;
    short idx, count;
    WORD fpucw;
    DWORD mxcsr;
} par_workers[PAR_MAX];
static int par_pending; /* fixtures set up and waiting for their simulation, on workers 0 to par_pending - 1 */

typedef int (__attribute__((thiscall)) *play_fn)(DWORD self, DWORD fixture, DWORD, DWORD, DWORD);
typedef void (__attribute__((thiscall)) *score_fn)(DWORD self, int result, int idx, int count);

static DWORD WINAPI par_worker_main(void *arg)
{
    struct par_worker *w = arg;
    for (;;) {
        WaitForSingleObject(w->run, INFINITE);
        __asm__ volatile("fninit\n\tfldcw %0\n\tldmxcsr %1" : : "m"(w->fpucw), "m"(w->mxcsr));
        w->result = ((play_fn)0x61b7b0)(w->self, w->args[0], w->args[1], w->args[2], w->args[3]);
        w->finished = 1;
        SetEvent(w->handoff);
    }
    return 0;
}

static struct par_worker *par_current(void)
{
    DWORD id = GetCurrentThreadId();
    for (int i = 0; i < cfg.par_matches; i++)
        if (par_workers[i].id == id)
            return &par_workers[i];
    return NULL;
}

/* On a worker, at the background simulation: hand the game thread back, then simulate and record when
 * the batch runs. */
static int par_simulate(struct par_worker *w, DWORD match, const DWORD *a)
{
    SetEvent(w->handoff);
    WaitForSingleObject(w->go_sim, INFINITE);
    int r = ((sim_fn)sim_tramp)(match, a[0], a[1], a[2], a[3], a[4]);
    SetEvent(w->sim_done);
    WaitForSingleObject(w->go_post, INFINITE);
    return r;
}

/* Game thread: simulate the pending fixtures together, then record them and show their scores in order. */
static void par_run_batch(void)
{
    int n = par_pending;
    if (!n)
        return;
    for (int i = 0; i < n; i++)
        if (!par_workers[i].finished)
            SetEvent(par_workers[i].go_sim);
    for (int i = 0; i < n; i++)
        if (!par_workers[i].finished)
            WaitForSingleObject(par_workers[i].sim_done, INFINITE);
    for (int i = 0; i < n; i++) {
        struct par_worker *w = &par_workers[i];
        if (!w->finished) {
            SetEvent(w->go_post);
            WaitForSingleObject(w->handoff, INFINITE);
        }
        if (w->result)
            ((score_fn)0x617720)(w->self, w->result, w->idx, w->count);
    }
    par_pending = 0;
}

static void par_drain(void)
{
    par_run_batch();
}

int __attribute__((thiscall)) play_async(DWORD self, DWORD fixture, DWORD a1, DWORD a2, DWORD a3)
{
    struct par_worker *w = &par_workers[par_pending++];
    w->self = self, w->args[0] = fixture, w->args[1] = a1, w->args[2] = a2, w->args[3] = a3;
    w->idx = *(short *)(self + 0x48);
    w->count = *(DWORD *)(self + 8) ? *(short *)(*(DWORD *)(self + 8) + 8) : 0;
    w->finished = 0;
    __asm__ volatile("fnstcw %0\n\tstmxcsr %1" : "=m"(w->fpucw), "=m"(w->mxcsr));
    SetEvent(w->run);
    WaitForSingleObject(w->handoff, INFINITE); /* set up (or finished without a simulation) */
    if (par_pending == cfg.par_matches || w->idx >= w->count)
        par_run_batch();
    return 0;
}

static int install_par_workers(void)
{
    for (int i = 0; i < cfg.par_matches; i++) {
        struct par_worker *w = &par_workers[i];
        w->run = CreateEventA(NULL, FALSE, FALSE, NULL);
        w->handoff = CreateEventA(NULL, FALSE, FALSE, NULL);
        w->go_post = CreateEventA(NULL, FALSE, FALSE, NULL);
        w->go_sim = CreateEventA(NULL, FALSE, FALSE, NULL);
        w->sim_done = CreateEventA(NULL, FALSE, FALSE, NULL);
        w->thread = CreateThread(NULL, 1 << 20, par_worker_main, w, STACK_SIZE_PARAM_IS_A_RESERVATION, &w->id);
        if (!w->run || !w->handoff || !w->go_post || !w->go_sim || !w->sim_done || !w->thread)
            return 0;
    }
    return patch_call(0x61cdd4, 0x61b7b0, play_async);
}

static int install_par(void)
{
    if (cfg.par_matches > PAR_MAX)
        cfg.par_matches = PAR_MAX;
    return install_par_tls() && install_par_locks() && install_sim_hook() && install_play_hook() && install_par_workers();
}

/* ---- fast_dyncast (docs/fast-dyncast.md) ----
 * The game calls dynamic_cast (msvcr71 __RTDynamicCast) from 703 sites; the hottest is 0x722b90,
 * dynamic_cast<ACTUAL_PLAYER *>(PERSON *), once per shortlist candidate. The runtime walks the class
 * hierarchy comparing mangled type names with strcmp: ~9% of a long holiday under Wine. The result
 * depends only on the object's vfptr (which fixes its complete type and subobject), the delta to it and
 * the two types, so cmscale memoises the pointer adjustment for each (vfptr, delta, source, target).
 * References, which can throw bad_cast, and null pointers go to the runtime. */

#define DYNCAST_BITS 12

static void *(__cdecl *real_RTDynamicCast)(void *, LONG, void *, void *, int);
static struct dyncast { DWORD vfptr; LONG delta; void *src, *dst; LONG adjust; int ok; } *dyncasts;

static void *__cdecl hook_RTDynamicCast(void *p, LONG delta, void *src, void *dst, int is_ref)
{
    if (!p || is_ref)
        return real_RTDynamicCast(p, delta, src, dst, is_ref);
    DWORD vfptr = *(DWORD *)p;
    DWORD h = (vfptr ^ (DWORD)dst * 31 ^ (DWORD)src * 7 ^ (DWORD)delta) * 2654435761u >> (32 - DYNCAST_BITS);
    struct dyncast *d = &dyncasts[h];
    if (d->ok && d->vfptr == vfptr && d->delta == delta && d->src == src && d->dst == dst)
        return d->adjust == LONG_MIN ? NULL : (char *)p + d->adjust;
    void *r = real_RTDynamicCast(p, delta, src, dst, is_ref);
    *d = (struct dyncast){vfptr, delta, src, dst, r ? (char *)r - (char *)p : LONG_MIN, 1};
    return r;
}

/* ---- prefetch (docs/prefetch.md) ----
 * get_playing_staff_for_position's candidate loop (0x6904ad), and its loan variant's (0x691351), spend
 * their time on cache misses as they follow each candidate's pointers from the person to the contract
 * array, the contract and the club. At the loop head this code looks ahead in the candidate array (0x14-byte
 * entries, with a person-table index at +0) and prefetches in stages, so each stage finds its pointers
 * already cached. It prefetches the person object PF_D1 candidates ahead, its contract array's storage
 * PF_D2 ahead, and the contracts and their clubs PF_D3 ahead. It only reads, and checks bounds and null
 * pointers, so the game's results can't change. */

#define PF_LOOP_HEAD 0x6904ad   /* mov esi,[esp+0x70] (person table holder); edi = current candidate */
#define PF_LOAN_LOOP_HEAD 0x691351 /* the loan variant (0x691140), which keeps everything in stack slots */
#define PF_STRIDE 0x14
#define PF_D1 8
#define PF_D2 4
#define PF_D3 2
#define PF_LINES 4

/* Diverts addr to a stub that calls fn(caller esp) with all registers and flags saved, then replays the
 * first len bytes of addr (whole instructions, no relative branches) and continues at addr + len.
 * fn reads the saved registers below esp: esp[-1] eax, [-2] ecx, [-3] edx, [-4] ebx, [-6] ebp, [-7] esi,
 * [-8] edi (pushad order). */
static int probe(DWORD addr, int len, void (__cdecl *fn)(DWORD *esp))
{
    BYTE *t = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE), *p = t;
    if (!t || len < 5 || len > 16)
        return 0;
    *p++ = 0x60;                                        /* pushad */
    *p++ = 0x9c;                                        /* pushfd */
    *p++ = 0x8d; *p++ = 0x44; *p++ = 0x24; *p++ = 0x24; /* lea eax,[esp+0x24]: esp before pushad */
    *p++ = 0x50;                                        /* push eax */
    *p++ = 0xe8;                                        /* call fn */
    *(DWORD *)p = (DWORD)fn - ((DWORD)p + 4);
    p += 4;
    *p++ = 0x83; *p++ = 0xc4; *p++ = 0x04;              /* add esp,4 */
    *p++ = 0x9d;                                        /* popfd */
    *p++ = 0x61;                                        /* popad */
    memcpy(p, (void *)addr, len);
    p += len;
    *p++ = 0xe9;
    *(DWORD *)p = addr + len - ((DWORD)p + 4);
    DWORD old;
    VirtualProtect((void *)addr, 5, PAGE_EXECUTE_READWRITE, &old);
    *(BYTE *)addr = 0xe9;
    *(DWORD *)(addr + 1) = (DWORD)t - (addr + 5);
    VirtualProtect((void *)addr, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)addr, 5);
    return 1;
}

static inline void pf(const void *p)
{
    _mm_prefetch((const char *)p, _MM_HINT_T0);
}

/* Returns the person's contract DEL_ARRAY<CONTRACT*,char>, or NULL for an unknown person type. It follows the
 * this-adjusting thunk in vtable slot +0xdc, which does sub ecx,[ecx-4] and then adds a constant. The array
 * sits at the adjusted this - 0x6c. */
static const BYTE *pf_contracts(const BYTE *person)
{
    int adj;
    switch ((*(const DWORD **)person)[0xdc / 4]) {
    case 0x76e900: adj = 0xcc; break;  /* ACTUAL_PLAYER, ACTUAL_NON_PLAYER, HUMAN_NON_PLAYER, ... */
    case 0x727c80: adj = -0x1c; break; /* YOUTH_PLAYER */
    case 0x7272b0: adj = 0; break;     /* ACTUAL_PERSON */
    case 0x7993e0: adj = -0x18; break; /* OFFICIAL */
    default: return NULL;
    }
    return person - *(const int *)(person - 4) + adj - 0x6c;
}

/* cand is the current candidate entry, and table is the person table (an ARRAY of PERSON*, with get at vtable +0x24) */
static void pf_scan(const BYTE *cand, int index, int count, const BYTE *table)
{
    LONG size = *(const LONG *)(table + 8);
    void *(__attribute__((thiscall)) *get)(const void *, LONG) = (void *)(*(const DWORD **)table)[0x24 / 4];
#define PF_PERSON(d) ({ LONG id_ = *(const LONG *)(cand + (d) * PF_STRIDE); \
                        id_ >= 0 && id_ < size ? (const BYTE *)get(table, id_) : NULL; })
    if (index + PF_D1 < count) {
        const BYTE *p = PF_PERSON(PF_D1);
        for (int i = 0; p && i < PF_LINES; i++)
            pf(p + 64 * i);
    }
    if (index + PF_D2 < count) {
        const BYTE *p = PF_PERSON(PF_D2), *list = p ? pf_contracts(p) : NULL;
        if (list && *(const signed char *)(list + 8) > 0)
            pf(*(void *const *)(list + 4));
    }
    if (index + PF_D3 < count) {
        const BYTE *p = PF_PERSON(PF_D3), *list = p ? pf_contracts(p) : NULL;
        int n = list ? *(const signed char *)(list + 8) : 0;
        const DWORD *data = n > 0 ? *(const DWORD *const *)(list + 4) : NULL;
        for (int i = 0; data && i < n && i < 4; i++) {
            const DWORD *c = (const DWORD *)data[i];
            if (c) {
                pf(c);
                if (c[2])
                    pf((const void *)c[2]);
            }
        }
    }
#undef PF_PERSON
}

/* get_playing_staff_for_position: edi = candidate, [esp+0x98] index, [esp+0x9c] count, [esp+0x70] table holder */
static void __cdecl pf_candidate(DWORD *esp)
{
    pf_scan((const BYTE *)esp[-8], esp[0x98 / 4], esp[0x9c / 4], *(const BYTE **)esp[0x70 / 4]);
}

/* get_playing_staff_for_position_loan: [esp+0x28] candidate, [esp+0x38] index, [esp+0x3c] count, [esp+0x44] table holder */
static void __cdecl pf_loan_candidate(DWORD *esp)
{
    pf_scan((const BYTE *)esp[0x28 / 4], esp[0x38 / 4], esp[0x3c / 4], *(const BYTE **)esp[0x44 / 4]);
}

static int install_prefetch(void)
{
    static const BYTE expect[] = {0x8b, 0x74, 0x24, 0x70, 0x8b, 0x0e};             /* mov esi,[esp+70]; mov ecx,[esi] */
    static const BYTE expect_loan[] = {0x8b, 0x54, 0x24, 0x28, 0x8b, 0x4c, 0x24, 0x44}; /* mov edx,[esp+28]; mov ecx,[esp+44] */
    int ok = 0;
    if (!memcmp((void *)PF_LOOP_HEAD, expect, sizeof expect))
        ok += probe(PF_LOOP_HEAD, sizeof expect, pf_candidate);
    if (!memcmp((void *)PF_LOAN_LOOP_HEAD, expect_loan, sizeof expect_loan))
        ok += probe(PF_LOAN_LOOP_HEAD, sizeof expect_loan, pf_loan_candidate);
    return ok;
}

static int patch_slot(DWORD va, void *hook, void **real, const char *dll, const char *fn)
{
    void **slot = (void **)va;
    void *expect = (void *)GetProcAddress(GetModuleHandleA(dll), fn);
    if (*slot != expect)
        return 0;
    DWORD old;
    VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old);
    *real = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return 1;
}

/* ---- the watched match: match_hl_speed and match_speed_max (docs/match-speed.md) ----
 * The watched match's per-frame update 0x59e9c0 steps the match when the screen's timer at +0x2c20 expires:
 * 0x59de80 advances the engine by one or more ticks and re-arms the timer (at 0x59e1ef, with the delay
 * already pushed). The screen's mode byte +0x2cc8 is 1 while a highlight plays, 2 while the match runs on
 * between highlights and 3 for commentary between them. In modes 2 and 3 the game divides the delays by the
 * match speed at +0x2cc0 (0.6, 0.72, 1, 3 or 8). In mode 1 each tick takes 125 ms whatever the speed: ticks x
 * 1000 x 0.125, with the 1000 from the replay speed +0x2e10 via 0x596d60.
 * match_speed_max replaces Very Fast's 8, the double at 0xd24220 that the Speed menu copies into +0x2cc0.
 * match_hl_speed gives highlights a fixed speed of their own, whatever the match speed. It divides the mode-1
 * delay by the game's Fast (3) or Very Fast (8) factor, except in a replay (+0x2e04 set) or the highlights
 * viewer (+0x2e00 set), which have their own speed and slow motion. */

#define VERY_FAST_SPEED (*(double *)0xd24220) /* in .data. The Speed menu reads it when the game builds the menu and when you choose an entry */
#define LIVE_ARM_AT 0x59e1ef    /* lea ecx,[esi+0x2c20] */

static void __cdecl live_arm(DWORD *esp)
{
    BYTE *s = (BYTE *)esp[-7];
    static const double hl_speed[3] = {1.0, 3.0, 8.0}; /* the match speeds Medium, Fast and Very Fast */
    if (s[0x2cc8] == 1 && *(int *)(s + 0x2e00) == -1 && *(int *)(s + 0x2e04) == -1) {
        DWORD d = (DWORD)(esp[0] / hl_speed[cfg.match_hl_speed] + 0.5);
        esp[0] = d ? d : 1;
    }
}

static int install_match_hl_speed(void)
{
    return !memcmp((void *)LIVE_ARM_AT, "\x8d\x8e\x20\x2c\x00\x00", 6) && probe(LIVE_ARM_AT, 6, live_arm);
}

static void load_config(const char *dir)
{
    char ini[MAX_PATH + 16], buf[32];
    snprintf(ini, sizeof ini, "%s\\cmscale.ini", dir);
    GetPrivateProfileStringA("cmscale", "zoom", "1.0", buf, sizeof buf, ini);
    cfg.zoom = atof(buf);
    if (cfg.zoom < 0.5 || cfg.zoom > 8)
        cfg.zoom = 1.0;
    cfg.borderless = GetPrivateProfileIntA("cmscale", "borderless", 1, ini);
    cfg.integer = GetPrivateProfileIntA("cmscale", "integer", 0, ini);
    GetPrivateProfileStringA("cmscale", "filter", "sharp", buf, sizeof buf, ini);
    cfg.filter = !_stricmp(buf, "nearest") ? FILTER_NEAREST : !_stricmp(buf, "smooth") ? FILTER_SMOOTH : FILTER_SHARP;
    GetPrivateProfileStringA("cmscale", "renderer", "opengl", buf, sizeof buf, ini);
    cfg.opengl = _stricmp(buf, "gdi") != 0;
    cfg.vsync = GetPrivateProfileIntA("cmscale", "vsync", 0, ini);
    cfg.max_fps = GetPrivateProfileIntA("cmscale", "max_fps", 120, ini);
    cfg.cursor_fix = GetPrivateProfileIntA("cmscale", "cursor_fix", 1, ini);
    cfg.prefetch = GetPrivateProfileIntA("cmscale", "prefetch", 1, ini);
    cfg.fast_dyncast = GetPrivateProfileIntA("cmscale", "fast_dyncast", 1, ini);
    cfg.par_matches = GetPrivateProfileIntA("cmscale", "par_matches", 8, ini);
    if (cfg.par_matches < 0)
        cfg.par_matches = 0;
    cfg.idle_wait = GetPrivateProfileIntA("cmscale", "idle_wait", 1, ini);
    cfg.fast_sqrt = GetPrivateProfileIntA("cmscale", "fast_sqrt", 1, ini);
    cfg.fast_rand = GetPrivateProfileIntA("cmscale", "fast_rand", 2, ini);
    cfg.skip_splash = GetPrivateProfileIntA("cmscale", "skip_splash", 1, ini);
    cfg.mouse_nav = GetPrivateProfileIntA("cmscale", "mouse_nav", 1, ini);
    cfg.match_hl_speed = GetPrivateProfileIntA("cmscale", "match_hl_speed", 0, ini);
    if (cfg.match_hl_speed < 0 || cfg.match_hl_speed > 2)
        cfg.match_hl_speed = 0;
    cfg.match_speed_max = GetPrivateProfileIntA("cmscale", "match_speed_max", 8, ini);
    if (cfg.match_speed_max < 8 || cfg.match_speed_max > 1000)
        cfg.match_speed_max = 8;
}

static void install(void)
{
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    char *slash = strrchr(dir, '\\');
    if (slash)
        *slash = 0;
    load_config(dir);

    /* patch only cm0304.exe 4.1.5. Any other exe just gets its sound forwarded */
    HMODULE exe = GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    if ((DWORD)exe != 0x400000 || nt->FileHeader.TimeDateStamp != CM0304_TIMESTAMP)
        return;
    LoadLibraryA("gdi32.dll");
    patch_slot(IAT_STRETCHDIBITS, hook_StretchDIBits, (void **)&real_StretchDIBits, "gdi32.dll", "StretchDIBits");
    patch_slot(IAT_REGISTERCLASSA, hook_RegisterClassA, (void **)&real_RegisterClassA, "user32.dll", "RegisterClassA");
    patch_slot(IAT_CREATEWINDOWEXA, hook_CreateWindowExA, (void **)&real_CreateWindowExA, "user32.dll", "CreateWindowExA");
    patch_slot(IAT_MOVEWINDOW, hook_MoveWindow, (void **)&real_MoveWindow, "user32.dll", "MoveWindow");
    patch_slot(IAT_GETCLIENTRECT, hook_GetClientRect, (void **)&real_GetClientRect, "user32.dll", "GetClientRect");
    patch_slot(IAT_PEEKMESSAGEA, hook_PeekMessageA, (void **)&real_PeekMessageA, "user32.dll", "PeekMessageA");
    if (cfg.cursor_fix) {
        patch_slot(IAT_SHOWCURSOR, hook_ShowCursor, (void **)&real_ShowCursor, "user32.dll", "ShowCursor");
        patch_slot(IAT_SETCURSOR, hook_SetCursor, (void **)&real_SetCursor, "user32.dll", "SetCursor");
    }
    if (cfg.borderless) {
        patch_slot(IAT_CHANGEDISPLAYSETTINGSA, hook_ChangeDisplaySettingsA, (void **)&real_ChangeDisplaySettingsA,
                   "user32.dll", "ChangeDisplaySettingsA");
        patch_slot(IAT_CLOSEWINDOW, hook_CloseWindow, (void **)&real_CloseWindow, "user32.dll", "CloseWindow");
    }
    if (cfg.idle_wait)
        timeBeginPeriod(1);
    if (cfg.fast_sqrt) {
        patch_jmp(FN_HYPOT24, "\x8b\x4c\x24\x04\x33\xc0\x0f\xaf\xc9\x8b\x54\x24\x08", 13, fast_hypot24);
        patch_jmp(FN_ISQRT32, "\x8b\x54\x24\x04\x33\xc0\x53\x56\x57\xbb\x00\x00\x01\x00", 14, fast_isqrt32);
    }
    if (cfg.fast_rand)
        patch_bytes(FN_RAND_TAIL, rand_tail_orig, rand_tail_fast, sizeof rand_tail_orig);
    if (cfg.fast_rand >= 2)
        patch_jmp(FN_RAND_REGEN, "\x53\x56\x57\x8b\xc1\xbe\xe3\x00\x00\x00", 10, fast_mt_regen);
    if (cfg.skip_splash)
        install_skip_splash();
    InitializeCriticalSection(&gl.lock);
    mapping_lock = &gl.lock;
    game_thread_id = GetCurrentThreadId();
    if (cfg.prefetch)
        install_prefetch();
    if (cfg.par_matches)
        install_par();
    if (cfg.match_speed_max != 8 && VERY_FAST_SPEED == 8.0)
        VERY_FAST_SPEED = cfg.match_speed_max;
    if (cfg.match_hl_speed)
        install_match_hl_speed();
    if (cfg.fast_dyncast) {
        dyncasts = VirtualAlloc(NULL, sizeof *dyncasts << DYNCAST_BITS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (dyncasts)
            patch_slot(IAT_RTDYNAMICCAST, hook_RTDynamicCast, (void **)&real_RTDynamicCast, "msvcr71.dll", "__RTDynamicCast");
    }
}

/* ---- dsound.dll forwarding (cm0304.exe imports only ordinal 1) ---- */

static HRESULT (WINAPI *real_DirectSoundCreate)(LPCGUID, void **, IUnknown *);

HRESULT WINAPI DirectSoundCreate(LPCGUID guid, void **ds, IUnknown *outer)
{
    if (!real_DirectSoundCreate) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, sizeof path);
        strcat(path, "\\dsound.dll");
        HMODULE m = LoadLibraryA(path);
        real_DirectSoundCreate = m ? (void *)GetProcAddress(m, "DirectSoundCreate") : NULL;
        if (!real_DirectSoundCreate)
            return E_FAIL;
    }
    return real_DirectSoundCreate(guid, ds, outer);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        install();
    }
    return TRUE;
}
