/*
 * Matrix digital rain - Windows screensaver (.scr)
 *
 *  - mirrored half-width katakana + digits + a few symbols (like the film)
 *  - white-hot head, green trail with exponential-looking fade, glyph flicker
 *  - glyph size scales with each monitor's resolution (about 34 rows per screen)
 *  - per-monitor independent rain, per-monitor DPI aware (Windows 10/11)
 *  - supports /s (run), /p <hwnd> (preview), /c (configure)
 *
 * Build (MinGW / zig cc):
 *   zig cc -target x86_64-windows-gnu -O2 -mwindows matrix.c -o Matrix.scr -lgdi32 -luser32
 * Build (MSVC):
 *   cl /O2 matrix.c /link /SUBSYSTEM:WINDOWS /OUT:Matrix.scr user32.lib gdi32.lib
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Portable core: atlas post-processing, simulation, compositing     */
/* ------------------------------------------------------------------ */

#define MAX_GLYPHS   96
#define MAX_SIMS     16
#define FLICKER_RATE 1.1f      /* glyph changes per second per visible cell */

typedef struct {
    int cw, ch, n;
    uint8_t *core;             /* n * cw * ch, glyph coverage          */
    uint8_t *glow;             /* n * cw * ch, blurred coverage (bloom) */
} Atlas;

typedef struct { float head, speed, trail; } Column;

typedef struct {
    int x0, y0, w, h;          /* region inside the shared back buffer */
    int cols, rows, ox;
    Atlas *atlas;
    Column *col;
    uint8_t *glyph;            /* cols * rows, indexed [c*rows + r]    */
} Sim;

static uint32_t g_rng = 2463534242u;
static inline uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static inline float rf(float a, float b) { return a + (b - a) * ((rnd() & 0xFFFFFF) / 16777216.0f); }

static void atlas_make_glow(Atlas *A)
{
    int w = A->cw, h = A->ch, sz = w * h;
    int r = h / 16; if (r < 1) r = 1;
    uint8_t *tmp = (uint8_t *)malloc((size_t)sz);
    A->glow = (uint8_t *)malloc((size_t)sz * A->n);
    for (int g = 0; g < A->n; g++) {
        const uint8_t *src = A->core + (size_t)g * sz;
        uint8_t *dst = A->glow + (size_t)g * sz;
        for (int y = 0; y < h; y++)                      /* horizontal box blur */
            for (int x = 0; x < w; x++) {
                int acc = 0;
                for (int d = -r; d <= r; d++) { int xx = x + d; if (xx >= 0 && xx < w) acc += src[y * w + xx]; }
                tmp[y * w + x] = (uint8_t)(acc / (2 * r + 1));
            }
        for (int y = 0; y < h; y++)                      /* vertical box blur */
            for (int x = 0; x < w; x++) {
                int acc = 0;
                for (int d = -r; d <= r; d++) { int yy = y + d; if (yy >= 0 && yy < h) acc += tmp[yy * w + x]; }
                int v = (acc / (2 * r + 1)) * 3;
                dst[y * w + x] = (uint8_t)(v > 255 ? 255 : v);
            }
    }
    free(tmp);
}

static void sim_init(Sim *s, Atlas *A, int x0, int y0, int w, int h)
{
    s->x0 = x0; s->y0 = y0; s->w = w; s->h = h; s->atlas = A;
    s->cols = w / A->cw; if (s->cols < 1) s->cols = 1;
    s->rows = h / A->ch + 1;
    s->ox = (w - s->cols * A->cw) / 2;
    s->col = (Column *)calloc((size_t)s->cols, sizeof(Column));
    s->glyph = (uint8_t *)malloc((size_t)s->cols * s->rows);
    for (int i = 0; i < s->cols * s->rows; i++) s->glyph[i] = (uint8_t)(rnd() % A->n);
    for (int c = 0; c < s->cols; c++) {
        Column *k = &s->col[c];
        k->speed = rf(5.f, 16.f);
        k->trail = rf(0.30f, 0.95f) * s->rows; if (k->trail < 8.f) k->trail = 8.f;
        k->head  = rf(-0.5f * s->rows, 1.0f * s->rows);       /* screen is already "running" */
    }
}

static void sim_free(Sim *s) { free(s->col); free(s->glyph); s->col = NULL; s->glyph = NULL; }

static void sim_update(Sim *s, float dt)
{
    const int n = s->atlas->n, rows = s->rows;
    const uint32_t pflick = (uint32_t)(FLICKER_RATE * dt * 65536.0f);
    for (int c = 0; c < s->cols; c++) {
        Column *k = &s->col[c];
        uint8_t *g = s->glyph + (size_t)c * rows;
        int o = (int)floorf(k->head);
        k->head += k->speed * dt;
        int h = (int)floorf(k->head);
        for (int r = o + 1; r <= h; r++)                      /* head enters new cell: fresh glyph */
            if (r >= 0 && r < rows) g[r] = (uint8_t)(rnd() % n);
        int r0 = h - (int)k->trail; if (r0 < 0) r0 = 0;       /* random glyph flicker in the trail */
        int r1 = h < rows ? h : rows - 1;
        for (int r = r0; r <= r1; r++)
            if ((rnd() & 0xFFFF) < pflick) g[r] = (uint8_t)(rnd() % n);
        if (k->head - k->trail > rows) {                      /* stream finished: respawn above screen */
            k->head  = -rf(1.f, 0.7f * rows);
            k->speed = rf(5.f, 16.f);
            k->trail = rf(0.30f, 0.95f) * rows; if (k->trail < 8.f) k->trail = 8.f;
        }
    }
}

static void sim_draw(const Sim *s, uint32_t *buf, int bw)
{
    const Atlas *A = s->atlas;
    const int cw = A->cw, ch = A->ch, gsz = cw * ch, rows = s->rows;
    const int ylim = s->y0 + s->h;
    for (int c = 0; c < s->cols; c++) {
        const Column *k = &s->col[c];
        const uint8_t *g = s->glyph + (size_t)c * rows;
        int hr = (int)floorf(k->head);
        float frac = k->head - (float)hr;
        int r0 = hr - (int)ceilf(k->trail); if (r0 < 0) r0 = 0;
        int r1 = hr < rows ? hr : rows - 1;
        int px0 = s->x0 + s->ox + c * cw;
        for (int r = r0; r <= r1; r++) {
            float age = frac + (float)(hr - r);
            float v = 1.0f - age / k->trail;
            if (v <= 0.f) continue;
            int head = (r == hr);
            float iv = head ? 1.f : powf(v, 1.7f);
            float L = head ? 1.f : (age < 3.f ? (1.f - age / 3.f) * 0.6f : 0.f);
            /* base green (0,255,70) blended toward white-green (220,255,230) near the head */
            int R = (int)((0.f   + L * (220.f - 0.f))   * iv);
            int G = (int)(255.f * iv);
            int B = (int)((70.f  + L * (230.f - 70.f))  * iv);
            int gk = head ? 190 : (int)(90 + 60 * L);
            const uint8_t *core = A->core + (size_t)g[r] * gsz;
            const uint8_t *glow = A->glow + (size_t)g[r] * gsz;
            int py0 = s->y0 + r * ch;
            for (int y = 0; y < ch; y++) {
                int py = py0 + y;
                if (py >= ylim) break;
                uint32_t *dst = buf + (size_t)py * bw + px0;
                const uint8_t *cr = core + y * cw, *gr = glow + y * cw;
                for (int x = 0; x < cw; x++) {
                    int a = cr[x] + ((gr[x] * gk) >> 8);
                    if (a > 255) a = 255;
                    dst[x] = ((uint32_t)((R * a) >> 8) << 16) | ((uint32_t)((G * a) >> 8) << 8) | (uint32_t)((B * a) >> 8);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Windows front end                                                  */
/* ------------------------------------------------------------------ */
#if defined(_WIN32) && !defined(MATRIX_CORE_ONLY)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

static const WCHAR kSymbols[] = L"0123456789:.=*+-<>|";

static int g_preview;
static HWND g_parent;
static HWND g_hwnd;
static HDC g_mdc;
static HBITMAP g_bmp;
static uint32_t *g_bits;
static int g_bw, g_bh;
static Sim g_sims[MAX_SIMS];
static int g_nsims;
static Atlas g_atlas[MAX_SIMS];
static int g_natlas;
static LARGE_INTEGER g_freq, g_last;
static POINT g_firstMouse;
static int g_haveMouse;

/* Render one glyph with GDI (white on black), optionally mirrored, into 8-bit coverage. */
static Atlas *get_atlas(int ch)
{
    for (int i = 0; i < g_natlas; i++) if (g_atlas[i].ch == ch) return &g_atlas[i];
    Atlas *A = &g_atlas[g_natlas++];
    memset(A, 0, sizeof(*A));
    int cw = (int)(ch * 0.62f + 0.5f); if (cw < 4) cw = 4;
    A->cw = cw; A->ch = ch;

    WCHAR list[MAX_GLYPHS]; unsigned char mirror[MAX_GLYPHS]; int n = 0;
    for (WCHAR c = 0xFF66; c <= 0xFF9D; c++) { list[n] = c; mirror[n++] = 1; }   /* half-width katakana, mirrored */
    for (const WCHAR *p = kSymbols; *p; p++) { list[n] = *p; mirror[n++] = 0; }
    A->n = n;
    A->core = (uint8_t *)calloc((size_t)n * cw * ch, 1);

    HDC sdc = GetDC(NULL);
    HDC mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = cw; bi.bmiHeader.biHeight = -ch;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ obmp = SelectObject(mdc, bmp);
    HFONT font = CreateFontW(-ch, 0, 0, 0, ch >= 18 ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"MS Gothic");
    HGDIOBJ ofont = SelectObject(mdc, font);
    SetBkMode(mdc, TRANSPARENT);
    SetTextColor(mdc, RGB(255, 255, 255));
    TEXTMETRICW tm; GetTextMetricsW(mdc, &tm);

    for (int i = 0; i < n; i++) {
        memset(bits, 0, (size_t)cw * ch * 4);
        SIZE sz; GetTextExtentPoint32W(mdc, &list[i], 1, &sz);
        int x = (cw - sz.cx) / 2, y = (ch - tm.tmHeight) / 2;
        TextOutW(mdc, x, y, &list[i], 1);
        GdiFlush();
        const uint32_t *px = (const uint32_t *)bits;
        uint8_t *dst = A->core + (size_t)i * cw * ch;
        for (int yy = 0; yy < ch; yy++)
            for (int xx = 0; xx < cw; xx++) {
                int sx = mirror[i] ? (cw - 1 - xx) : xx;
                dst[yy * cw + xx] = (uint8_t)((px[yy * cw + sx] >> 8) & 0xFF);   /* G channel = grey coverage */
            }
    }
    SelectObject(mdc, ofont); DeleteObject(font);
    SelectObject(mdc, obmp);  DeleteObject(bmp);
    DeleteDC(mdc); ReleaseDC(NULL, sdc);
    atlas_make_glow(A);
    return A;
}

static int cell_height_for(int monitorH)
{
    int ch = monitorH / 34;                   /* ~34 rows per screen, scales with resolution */
    if (ch < 16) ch = 16;
    if (ch > 180) ch = 180;
    return ch;
}

static BOOL CALLBACK enum_mon(HMONITOR hm, HDC hdc, LPRECT rc, LPARAM lp)
{
    (void)hm; (void)hdc;
    int vx = ((POINT *)lp)->x, vy = ((POINT *)lp)->y;
    if (g_nsims >= MAX_SIMS) return FALSE;
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    sim_init(&g_sims[g_nsims++], get_atlas(cell_height_for(h)), rc->left - vx, rc->top - vy, w, h);
    return TRUE;
}

static void create_backbuffer(HWND hwnd, int w, int h)
{
    HDC wdc = GetDC(hwnd);
    g_mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    g_bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, (void **)&g_bits, NULL, 0);
    SelectObject(g_mdc, g_bmp);
    ReleaseDC(hwnd, wdc);
    g_bw = w; g_bh = h;
}

static void present(HDC hdc) { BitBlt(hdc, 0, 0, g_bw, g_bh, g_mdc, 0, 0, SRCCOPY); }

static void step(HWND hwnd)
{
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    float dt = (float)((double)(now.QuadPart - g_last.QuadPart) / (double)g_freq.QuadPart);
    g_last = now;
    if (dt > 0.1f) dt = 0.1f;
    memset(g_bits, 0, (size_t)g_bw * g_bh * 4);
    for (int i = 0; i < g_nsims; i++) { sim_update(&g_sims[i], dt); sim_draw(&g_sims[i], g_bits, g_bw); }
    HDC hdc = GetDC(hwnd); present(hdc); ReleaseDC(hwnd, hdc);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (g_preview && g_parent && !IsWindow(g_parent)) { PostQuitMessage(0); return 0; }
        step(hwnd);
        return 0;
    case WM_PAINT: { PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps); if (g_mdc) present(hdc); EndPaint(hwnd, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: if (!g_preview) { SetCursor(NULL); return TRUE; } break;
    case WM_SYSCOMMAND:
        if (!g_preview && (wp == SC_SCREENSAVE || wp == SC_CLOSE)) return 0;
        break;
    case WM_MOUSEMOVE:
        if (!g_preview) {
            POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
            if (!g_haveMouse) { g_firstMouse = p; g_haveMouse = 1; }
            else if (abs(p.x - g_firstMouse.x) > 4 || abs(p.y - g_firstMouse.y) > 4) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        if (!g_preview) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_ACTIVATEAPP:
        if (!g_preview && wp == FALSE) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: KillTimer(hwnd, 1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void set_dpi_aware(void)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI *PFN_Ctx)(HANDLE);
    PFN_Ctx fn = (PFN_Ctx)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (fn && fn((HANDLE)(intptr_t)-4)) return;            /* PER_MONITOR_AWARE_V2 */
    typedef BOOL (WINAPI *PFN_Aware)(void);
    PFN_Aware f2 = (PFN_Aware)(void *)GetProcAddress(u, "SetProcessDPIAware");
    if (f2) f2();
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    (void)hPrev; (void)show;
    char mode = 'c'; HWND parent = NULL;
    const char *p = cmd ? cmd : "";
    while (*p == ' ') p++;
    if (*p == '/' || *p == '-') { mode = (char)((p[1] >= 'A' && p[1] <= 'Z') ? p[1] + 32 : p[1]); p += 2; }
    while (*p == ':' || *p == ' ') p++;
    if (*p) parent = (HWND)(uintptr_t)_strtoui64(p, NULL, 10);

    if (mode == 'c' || mode == 'a' || mode == 0) {
        MessageBoxW(parent, L"Matrix Digital Rain\n\nThere is nothing to configure: the glyph size scales automatically "
                            L"with each monitor's resolution.", L"Matrix Screensaver", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (mode != 's' && mode != 'p') return 0;

    g_preview = (mode == 'p');
    g_parent = parent;
    if (!g_preview) set_dpi_aware();

    QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_last);
    g_rng ^= (uint32_t)g_last.QuadPart; if (!g_rng) g_rng = 1;

    WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = L"MatrixRainSaver";
    wc.hCursor = NULL; wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    if (g_preview) {
        if (!parent) return 0;
        RECT rc; GetClientRect(parent, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w < 8 || h < 8) return 0;
        g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, w, h, parent, NULL, hInst, NULL);
        int ch = h / 14; if (ch < 8) ch = 8;
        sim_init(&g_sims[g_nsims++], get_atlas(ch), 0, 0, w, h);
        create_backbuffer(g_hwnd, w, h);
    } else {
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        g_hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"Matrix", WS_POPUP | WS_VISIBLE, vx, vy, vw, vh,
                                 NULL, NULL, hInst, NULL);
        POINT org = { vx, vy };
        EnumDisplayMonitors(NULL, NULL, enum_mon, (LPARAM)&org);
        create_backbuffer(g_hwnd, vw, vh);
        ShowCursor(FALSE);
        SetForegroundWindow(g_hwnd);
    }
    if (!g_hwnd) return 1;
    SetTimer(g_hwnd, 1, 33, NULL);                       /* ~30 fps */

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }

    if (!g_preview) ShowCursor(TRUE);
    for (int i = 0; i < g_nsims; i++) sim_free(&g_sims[i]);
    return 0;
}

#endif /* _WIN32 */
