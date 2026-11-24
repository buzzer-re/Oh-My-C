/* conpty_drive.c - drive a console program on a Windows pseudo console (ConPTY) and show its screen.
 *
 *   conpty_drive [-c COLS] [-r ROWS] [-o RAWFILE] [-m conpty|console] -- PROGRAM [ARGS...]   (script on stdin)
 *
 * -m conpty (default): a pseudo console; its VT output is parsed into a screen by a small emulator.
 * -m console: a real console window (conhost), hidden; keys go in as key events and the screen is read back from
 *   the console buffer, exactly what a user of that console sees.
 *
 * Script lines:
 *   sleep MS             wait, collecting output
 *   send TEXT            type TEXT (escapes: \r \n \t \e \\ \xHH)
 *   raw TEXT             console mode: TEXT as key-down records without a key (Vk 0), the way Windows Terminal's
 *                        console host relays the win32-input-mode records of a program that asked for them
 *   expect TEXT [MS]     wait until TEXT is on the screen (default 10000 ms); exit 1 with the screen if it never is
 *   screen               print the screen (rows, trailing blanks trimmed) between ==== markers
 *   wait [MS]            wait for the program to exit (default 10000 ms); its exit code becomes ours
 *
 * Output is parsed by a small VT emulator (cursor motion, erase, insert/delete, scroll regions, wide characters,
 * the alternate screen), enough for the screens ConPTY sends. Test tool only (tests/windows), never part of omc. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif
typedef void *hpcon_t;
typedef HRESULT(WINAPI *create_pc_t)(COORD, HANDLE, HANDLE, DWORD, hpcon_t *);
typedef void(WINAPI *close_pc_t)(hpcon_t);

/* ---------- screen ---------- */

static int W = 120, H = 40;
static uint32_t *g_cells, *g_alt, *g_main;
static int cx, cy, sx, sy, top, bot;
static bool g_alt_on, g_wrap_pending;
static FILE *g_raw;
static CRITICAL_SECTION g_lock;

static uint32_t *cell(int x, int y) { return &g_cells[y * W + x]; }

static void clear_cells(int x0, int y0, int x1, int y1) { /* inclusive start, exclusive end in reading order */
    int a = y0 * W + x0, b = y1 * W + x1;
    for (int i = a; i < b && i < W * H; i++) g_cells[i] = ' ';
}

static void scroll_up(int n) {
    for (int k = 0; k < n; k++) {
        memmove(&g_cells[top * W], &g_cells[(top + 1) * W], (size_t)(bot - top) * W * sizeof *g_cells);
        for (int x = 0; x < W; x++) *cell(x, bot) = ' ';
    }
}

static void scroll_down(int n) {
    for (int k = 0; k < n; k++) {
        memmove(&g_cells[(top + 1) * W], &g_cells[top * W], (size_t)(bot - top) * W * sizeof *g_cells);
        for (int x = 0; x < W; x++) *cell(x, top) = ' ';
    }
}

static void newline(void) {
    if (cy == bot) scroll_up(1);
    else if (cy < H - 1) cy++;
}

static int cp_width(uint32_t c) {
    if (c == 0 || (c >= 0x300 && c <= 0x36F) || c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F)) return 0;
    /* pictographs that default to text presentation (East Asian Width neutral): one column */
    if ((c >= 0x1F321 && c <= 0x1F32C) || (c >= 0x1F336 && c <= 0x1F336) || (c >= 0x1F37D && c <= 0x1F37D) ||
        (c >= 0x1F394 && c <= 0x1F39F) || (c >= 0x1F3CB && c <= 0x1F3CE) || (c >= 0x1F3D4 && c <= 0x1F3DF) ||
        (c >= 0x1F3F1 && c <= 0x1F3F3) || (c >= 0x1F3F5 && c <= 0x1F3F7) || c == 0x1F43F || c == 0x1F441 ||
        (c >= 0x1F4FD && c <= 0x1F4FE) || (c >= 0x1F53E && c <= 0x1F54A) || (c >= 0x1F54F && c <= 0x1F54F) ||
        (c >= 0x1F568 && c <= 0x1F579) || (c >= 0x1F57B && c <= 0x1F594) || (c >= 0x1F597 && c <= 0x1F5A3) ||
        (c >= 0x1F5A5 && c <= 0x1F5FA))
        return 1;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) ||
        (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) ||
        (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) || (c >= 0x1F900 && c <= 0x1F9FF) ||
        (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}

static void put(uint32_t c) {
    int w = cp_width(c);
    if (!w) return;
    if (g_wrap_pending) {
        cx = 0;
        newline();
        g_wrap_pending = false;
    }
    if (cx + w > W) {
        cx = 0;
        newline();
    }
    *cell(cx, cy) = c;
    if (w == 2 && cx + 1 < W) *cell(cx + 1, cy) = 0; /* the wide char's second column */
    cx += w;
    if (cx >= W) {
        cx = W - 1;
        g_wrap_pending = true;
    }
}

static int par(const int *p, int n, int i, int def) { return i < n && p[i] > 0 ? p[i] : def; }

static void csi(const int *p, int n, char priv, char fin) {
    g_wrap_pending = false;
    switch (fin) {
    case 'H':
    case 'f':
        cy = par(p, n, 0, 1) - 1;
        cx = par(p, n, 1, 1) - 1;
        break;
    case 'A': cy -= par(p, n, 0, 1); break;
    case 'B': cy += par(p, n, 0, 1); break;
    case 'C': cx += par(p, n, 0, 1); break;
    case 'D': cx -= par(p, n, 0, 1); break;
    case 'E': cy += par(p, n, 0, 1), cx = 0; break;
    case 'F': cy -= par(p, n, 0, 1), cx = 0; break;
    case 'G': cx = par(p, n, 0, 1) - 1; break;
    case 'd': cy = par(p, n, 0, 1) - 1; break;
    case 'J': {
        int m = n ? p[0] : 0;
        if (m == 0) clear_cells(cx, cy, W, H - 1), clear_cells(0, cy + 1, 0, H);
        else if (m == 1) clear_cells(0, 0, cx + 1, cy);
        else clear_cells(0, 0, 0, H);
        if (m == 0) clear_cells(cx, cy, 0, cy + 1);
        break;
    }
    case 'K': {
        int m = n ? p[0] : 0;
        int a = m == 0 ? cx : 0, b = m == 1 ? cx + 1 : W;
        for (int x = a; x < b && x < W; x++) *cell(x, cy) = ' ';
        break;
    }
    case 'X':
        for (int x = cx; x < cx + par(p, n, 0, 1) && x < W; x++) *cell(x, cy) = ' ';
        break;
    case 'P': {
        int k = par(p, n, 0, 1);
        for (int x = cx; x < W; x++) *cell(x, cy) = x + k < W ? *cell(x + k, cy) : ' ';
        break;
    }
    case '@': {
        int k = par(p, n, 0, 1);
        for (int x = W - 1; x >= cx; x--) *cell(x, cy) = x - k >= cx ? *cell(x - k, cy) : ' ';
        break;
    }
    case 'L':
    case 'M': {
        if (cy < top || cy > bot) break;
        int k = par(p, n, 0, 1), save = top;
        top = cy;
        if (fin == 'L') scroll_down(k);
        else scroll_up(k);
        top = save;
        break;
    }
    case 'S': scroll_up(par(p, n, 0, 1)); break;
    case 'T': scroll_down(par(p, n, 0, 1)); break;
    case 'r':
        top = par(p, n, 0, 1) - 1;
        bot = par(p, n, 1, H) - 1;
        if (top < 0 || bot >= H || top >= bot) top = 0, bot = H - 1;
        cx = cy = 0;
        break;
    case 's': sx = cx, sy = cy; break;
    case 'u': cx = sx, cy = sy; break;
    case 'h':
    case 'l':
        if (priv == '?')
            for (int i = 0; i < n; i++)
                if (p[i] == 1049 || p[i] == 1047 || p[i] == 47) {
                    bool on = fin == 'h';
                    if (on == g_alt_on) continue;
                    g_alt_on = on;
                    g_cells = on ? g_alt : g_main;
                    if (on) clear_cells(0, 0, 0, H);
                }
        break;
    default: break;
    }
    if (cx < 0) cx = 0;
    if (cx >= W) cx = W - 1;
    if (cy < 0) cy = 0;
    if (cy >= H) cy = H - 1;
}

/* the parser keeps its state across reads */
static enum { S_TEXT, S_ESC, S_CSI, S_OSC, S_OSC_ESC, S_STR, S_STR_ESC, S_CHARSET } g_st = S_TEXT;
static int g_par[32], g_np;
static char g_priv;
static uint32_t g_u8;
static int g_u8n;

static void feed(const unsigned char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = b[i];
        switch (g_st) {
        case S_TEXT:
            if (g_u8n) {
                if ((c & 0xC0) == 0x80) {
                    g_u8 = (g_u8 << 6) | (c & 0x3F);
                    if (--g_u8n == 0) put(g_u8);
                    continue;
                }
                g_u8n = 0;
            }
            if (c == 0x1b) g_st = S_ESC;
            else if (c == '\r') cx = 0, g_wrap_pending = false;
            else if (c == '\n' || c == 0x0b || c == 0x0c) newline(), g_wrap_pending = false;
            else if (c == '\b') { if (cx > 0) cx--; }
            else if (c == '\t') cx = (cx / 8 + 1) * 8 < W ? (cx / 8 + 1) * 8 : W - 1;
            else if (c < 0x20 || c == 0x7f) {
            } else if (c < 0x80) put(c);
            else if ((c & 0xE0) == 0xC0) g_u8 = c & 0x1F, g_u8n = 1;
            else if ((c & 0xF0) == 0xE0) g_u8 = c & 0x0F, g_u8n = 2;
            else if ((c & 0xF8) == 0xF0) g_u8 = c & 0x07, g_u8n = 3;
            break;
        case S_ESC:
            g_st = S_TEXT;
            if (c == '[') g_st = S_CSI, g_np = 0, g_priv = 0, memset(g_par, 0, sizeof g_par);
            else if (c == ']') g_st = S_OSC;
            else if (c == 'P' || c == '_' || c == '^' || c == 'X') g_st = S_STR;
            else if (c == '(' || c == ')' || c == '*' || c == '+') g_st = S_CHARSET;
            else if (c == '7') sx = cx, sy = cy;
            else if (c == '8') cx = sx, cy = sy;
            else if (c == 'M') { if (cy == top) scroll_down(1); else if (cy > 0) cy--; }
            else if (c == 'D') newline();
            else if (c == 'E') cx = 0, newline();
            else if (c == 'c') clear_cells(0, 0, 0, H), cx = cy = 0;
            break;
        case S_CHARSET: g_st = S_TEXT; break;
        case S_CSI:
            if (c >= '0' && c <= '9') {
                if (!g_np) g_np = 1;
                g_par[g_np - 1] = g_par[g_np - 1] * 10 + (c - '0');
            } else if (c == ';' || c == ':') {
                if (!g_np) g_np = 1;
                if (g_np < 32) g_par[g_np++] = 0;
            } else if (c == '?' || c == '>' || c == '<' || c == '=') {
                g_priv = (char)c;
            } else if (c >= 0x40 && c <= 0x7e) {
                csi(g_par, g_np, g_priv, (char)c);
                g_st = S_TEXT;
            } else if (c < 0x20 && c != 0x1b) {
            } else if (c == 0x1b) {
                g_st = S_ESC;
            }
            break;
        case S_OSC:
            if (c == 0x07) g_st = S_TEXT;
            else if (c == 0x1b) g_st = S_OSC_ESC;
            break;
        case S_OSC_ESC: g_st = c == '\\' ? S_TEXT : S_OSC; break;
        case S_STR:
            if (c == 0x1b) g_st = S_STR_ESC;
            break;
        case S_STR_ESC: g_st = c == '\\' ? S_TEXT : S_STR; break;
        }
    }
}

/* the screen as UTF-8 lines, trailing blanks trimmed */
static char *render(void) {
    size_t cap = (size_t)W * H * 4 + H * 2 + 1, n = 0;
    char *out = malloc(cap);
    for (int y = 0; y < H; y++) {
        size_t line_start = n, last = n;
        for (int x = 0; x < W; x++) {
            uint32_t c = *cell(x, y);
            if (!c) continue;
            wchar_t w[2];
            int wn = 0;
            if (c >= 0x10000) {
                c -= 0x10000;
                w[wn++] = (wchar_t)(0xD800 + (c >> 10));
                w[wn++] = (wchar_t)(0xDC00 + (c & 0x3FF));
            } else {
                w[wn++] = (wchar_t)c;
            }
            n += (size_t)WideCharToMultiByte(CP_UTF8, 0, w, wn, out + n, (int)(cap - n), NULL, NULL);
            if (*cell(x, y) != ' ') last = n;
        }
        (void)line_start;
        n = last;
        out[n++] = '\n';
    }
    out[n] = 0;
    return out;
}

/* ---------- the program ---------- */

static HANDLE g_out_r, g_in_w, g_proc;
static volatile LONG g_eof;

static DWORD WINAPI reader(void *arg) {
    (void)arg;
    unsigned char buf[16384];
    DWORD got;
    while (ReadFile(g_out_r, buf, sizeof buf, &got, NULL) && got) {
        EnterCriticalSection(&g_lock);
        if (g_raw) fwrite(buf, 1, got, g_raw);
        feed(buf, got);
        LeaveCriticalSection(&g_lock);
    }
    InterlockedExchange(&g_eof, 1);
    return 0;
}

static void unescape(const char *s, char *out, size_t *n) {
    size_t k = 0;
    for (; *s; s++) {
        if (*s != '\\' || !s[1]) {
            out[k++] = *s;
            continue;
        }
        s++;
        switch (*s) {
        case 'r': out[k++] = '\r'; break;
        case 'n': out[k++] = '\n'; break;
        case 't': out[k++] = '\t'; break;
        case 'e': out[k++] = 0x1b; break;
        case '\\': out[k++] = '\\'; break;
        case 'x': {
            unsigned v = 0;
            for (int i = 0; i < 2 && s[1]; i++, s++) v = v * 16 + (unsigned)(s[1] <= '9' ? s[1] - '0' : (s[1] | 32) - 'a' + 10);
            out[k++] = (char)v;
            break;
        }
        default: out[k++] = *s;
        }
    }
    *n = k;
}

/* ---------- the console backend (-m console): a real conhost window, hidden ---------- */

static bool g_console;
static HANDLE g_con_in = INVALID_HANDLE_VALUE, g_con_out = INVALID_HANDLE_VALUE;

/* the visible window of the console, as UTF-8 lines */
static char *console_render(void) {
    CONSOLE_SCREEN_BUFFER_INFO ci;
    if (!GetConsoleScreenBufferInfo(g_con_out, &ci)) return strdup("(no console)\n");
    int w = ci.srWindow.Right - ci.srWindow.Left + 1, h = ci.srWindow.Bottom - ci.srWindow.Top + 1;
    CHAR_INFO *cells = malloc((size_t)w * h * sizeof *cells);
    SMALL_RECT r = ci.srWindow;
    ReadConsoleOutputW(g_con_out, cells, (COORD){(SHORT)w, (SHORT)h}, (COORD){0, 0}, &r);
    size_t cap = (size_t)w * h * 4 + (size_t)h * 2 + 1, n = 0;
    char *out = malloc(cap);
    for (int y = 0; y < h; y++) {
        size_t last = n;
        for (int x = 0; x < w; x++) {
            CHAR_INFO *c = &cells[y * w + x];
            if (c->Attributes & COMMON_LVB_TRAILING_BYTE) continue;
            wchar_t wc[2] = {c->Char.UnicodeChar, 0};
            int wn = 1;
            if (wc[0] >= 0xD800 && wc[0] <= 0xDBFF && x + 1 < w) wc[1] = cells[y * w + x + 1].Char.UnicodeChar, wn = 2, x++;
            n += (size_t)WideCharToMultiByte(CP_UTF8, 0, wc, wn, out + n, (int)(cap - n), NULL, NULL);
            if (wc[0] != L' ') last = n;
        }
        n = last;
        out[n++] = '\n';
    }
    out[n] = 0;
    cx = ci.dwCursorPosition.X - ci.srWindow.Left;
    cy = ci.dwCursorPosition.Y - ci.srWindow.Top;
    free(cells);
    return out;
}

/* `raw`: each character as a Vk-0 key-down record, as OpenConsole hands relayed text to the program */
static void console_send_raw(const char *buf, size_t n) {
    wchar_t *w = malloc((n + 1) * sizeof *w);
    int wn = MultiByteToWideChar(CP_UTF8, 0, buf, (int)n, w, (int)n + 1);
    INPUT_RECORD *rec = calloc((size_t)wn + 1, sizeof *rec);
    for (int i = 0; i < wn; i++) {
        rec[i].EventType = KEY_EVENT;
        rec[i].Event.KeyEvent.bKeyDown = TRUE;
        rec[i].Event.KeyEvent.wRepeatCount = 1;
        rec[i].Event.KeyEvent.uChar.UnicodeChar = w[i];
    }
    DWORD written;
    WriteConsoleInputW(g_con_in, rec, (DWORD)wn, &written);
    free(rec);
    free(w);
}

static void console_send(const char *buf, size_t n) {
    wchar_t *w = malloc((n + 1) * sizeof *w);
    int wn = MultiByteToWideChar(CP_UTF8, 0, buf, (int)n, w, (int)n + 1);
    INPUT_RECORD *rec = calloc((size_t)wn * 2 + 1, sizeof *rec);
    int k = 0;
    for (int i = 0; i < wn; i++) {
        wchar_t c = w[i];
        WORD vk = c == L'\r' ? VK_RETURN : c == 0x1b ? VK_ESCAPE : c == L'\t' ? VK_TAB : c == 0x7f || c == L'\b' ? VK_BACK : 0;
        DWORD ctl = 0;
        if (!vk && c < 0x80) {
            SHORT s = VkKeyScanW(c);
            if (s != -1) {
                vk = (WORD)(s & 0xff);
                if (s & 0x100) ctl |= SHIFT_PRESSED;
                if (s & 0x200) ctl |= LEFT_CTRL_PRESSED;
            }
            if (c < 0x20 && c != L'\r' && c != 0x1b && c != L'\t' && c != L'\b') vk = (WORD)('A' + c - 1), ctl |= LEFT_CTRL_PRESSED;
        }
        for (int down = 1; down >= 0; down--) {
            KEY_EVENT_RECORD *ke = &rec[k].Event.KeyEvent;
            rec[k].EventType = KEY_EVENT;
            ke->bKeyDown = down;
            ke->wRepeatCount = 1;
            ke->wVirtualKeyCode = vk;
            ke->wVirtualScanCode = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
            ke->uChar.UnicodeChar = c;
            ke->dwControlKeyState = ctl;
            k++;
        }
    }
    DWORD written;
    WriteConsoleInputW(g_con_in, rec, (DWORD)k, &written);
    free(rec);
    free(w);
}

/* `key NAME [shift] [ctrl] [alt]`: one key with modifiers, as the console host sees a real keyboard. NAME is enter,
 * tab, esc, backspace, up, down, left, right, home, end, delete, space, or a single character. */
static void send_key(const char *spec) {
    char name[32] = "";
    DWORD ctl = 0;
    const char *p = spec;
    int k = 0;
    while (*p && *p != ' ' && k < 31) name[k++] = *p++;
    name[k] = 0;
    if (strstr(p, "shift")) ctl |= SHIFT_PRESSED;
    if (strstr(p, "ctrl")) ctl |= LEFT_CTRL_PRESSED;
    if (strstr(p, "alt")) ctl |= LEFT_ALT_PRESSED;
    static const struct {
        const char *n;
        WORD vk;
        wchar_t ch;
    } K[] = {{"enter", VK_RETURN, L'\r'}, {"tab", VK_TAB, L'\t'}, {"esc", VK_ESCAPE, 0x1b}, {"backspace", VK_BACK, 0x08},
             {"up", VK_UP, 0}, {"down", VK_DOWN, 0}, {"left", VK_LEFT, 0}, {"right", VK_RIGHT, 0}, {"home", VK_HOME, 0},
             {"end", VK_END, 0}, {"delete", VK_DELETE, 0}, {"space", VK_SPACE, L' '}};
    WORD vk = 0;
    wchar_t ch = 0;
    for (size_t i = 0; i < sizeof K / sizeof *K; i++)
        if (!strcmp(name, K[i].n)) vk = K[i].vk, ch = K[i].ch;
    if (!vk && name[0] && !name[1]) {
        ch = (wchar_t)name[0];
        SHORT s = VkKeyScanW(ch);
        vk = (WORD)(s & 0xff);
        if (ctl & LEFT_CTRL_PRESSED) ch = (ch >= L'a' && ch <= L'z') ? (wchar_t)(ch - L'a' + 1) : ch;
        if (ctl & SHIFT_PRESSED) ch = (wchar_t)towupper(ch);
    }
    if (ctl & LEFT_CTRL_PRESSED && vk == VK_RETURN) ch = L'\n';
    WORD sc = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (g_console) {
        INPUT_RECORD r[2];
        memset(r, 0, sizeof r);
        for (int d = 0; d < 2; d++) {
            r[d].EventType = KEY_EVENT;
            r[d].Event.KeyEvent.bKeyDown = d == 0;
            r[d].Event.KeyEvent.wRepeatCount = 1;
            r[d].Event.KeyEvent.wVirtualKeyCode = vk;
            r[d].Event.KeyEvent.wVirtualScanCode = sc;
            r[d].Event.KeyEvent.uChar.UnicodeChar = ch;
            r[d].Event.KeyEvent.dwControlKeyState = ctl;
        }
        DWORD w;
        WriteConsoleInputW(g_con_in, r, 2, &w);
        return;
    }
    char buf[128];
    int n = snprintf(buf, sizeof buf, "\x1b[%u;%u;%u;1;%lu;1_\x1b[%u;%u;%u;0;%lu;1_", vk, sc, (unsigned)ch, ctl, vk, sc, (unsigned)ch, ctl);
    DWORD w;
    WriteFile(g_in_w, buf, (DWORD)n, &w, NULL);
}

static bool screen_has(const char *text) {
    if (g_console) {
        char *s = console_render();
        bool r = strstr(s, text) != NULL;
        free(s);
        return r;
    }
    EnterCriticalSection(&g_lock);
    char *s = render();
    LeaveCriticalSection(&g_lock);
    bool r = strstr(s, text) != NULL;
    free(s);
    return r;
}

static void print_screen(void) {
    if (g_console) {
        char *s = console_render();
        printf("==== console %dx%d cursor %d,%d\n%s==== end\n", W, H, cx + 1, cy + 1, s);
        fflush(stdout);
        free(s);
        return;
    }
    EnterCriticalSection(&g_lock);
    char *s = render();
    LeaveCriticalSection(&g_lock);
    printf("==== screen %dx%d cursor %d,%d\n%s==== end\n", W, H, cx + 1, cy + 1, s);
    fflush(stdout);
    free(s);
}

static void append_arg(wchar_t **cl, size_t *len, const char *a) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, a, -1, NULL, 0);
    wchar_t *w = malloc((size_t)wn * sizeof *w);
    MultiByteToWideChar(CP_UTF8, 0, a, -1, w, wn);
    size_t need = *len + (size_t)wn * 2 + 4;
    *cl = realloc(*cl, need * sizeof **cl);
    wchar_t *o = *cl + *len;
    if (*len) *o++ = L' ';
    bool q = !*w || wcspbrk(w, L" \t\"");
    if (q) *o++ = L'"';
    for (wchar_t *p = w; *p; p++) {
        if (*p == L'"') *o++ = L'\\';
        *o++ = *p;
    }
    if (q) *o++ = L'"';
    *o = 0;
    *len = (size_t)(o - *cl);
    free(w);
}

int main(int argc, char **argv) {
    int i = 1;
    const char *rawp = NULL;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) W = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) H = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) rawp = argv[++i];
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) g_console = !strcmp(argv[++i], "console");
        else if (!strcmp(argv[i], "--")) {
            i++;
            break;
        } else break;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: conpty_drive [-c COLS] [-r ROWS] [-o RAW] -- PROGRAM [ARGS...] < script\n");
        return 2;
    }
    InitializeCriticalSection(&g_lock);
    g_main = malloc((size_t)W * H * sizeof *g_main);
    g_alt = malloc((size_t)W * H * sizeof *g_alt);
    g_cells = g_main;
    for (int k = 0; k < W * H; k++) g_main[k] = g_alt[k] = ' ';
    top = 0, bot = H - 1;
    if (rawp) g_raw = fopen(rawp, "wb");

    hpcon_t pc = NULL;
    close_pc_t close_pc = NULL;
    HANDLE th = NULL;
    if (g_console) {
        /* a console window of its own, never shown; this process attaches to it to type and to read the screen */
        wchar_t *cl = NULL;
        size_t cll = 0;
        for (int k = i; k < argc; k++) append_arg(&cl, &cll, argv[k]);
        STARTUPINFOW si;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USECOUNTCHARS;
        si.wShowWindow = SW_HIDE;
        si.dwXCountChars = (DWORD)W;
        si.dwYCountChars = (DWORD)H;
        PROCESS_INFORMATION pi;
        if (!CreateProcessW(NULL, cl, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
            fprintf(stderr, "conpty_drive: cannot start the program (error %lu)\n", GetLastError());
            return 2;
        }
        g_proc = pi.hProcess;
        CloseHandle(pi.hThread);
        FreeConsole();
        bool attached = false;
        for (int t = 0; t < 200 && !attached; t++) {
            attached = AttachConsole(pi.dwProcessId);
            if (!attached) Sleep(10);
        }
        if (!attached) {
            fprintf(stderr, "conpty_drive: cannot attach to the program's console (error %lu)\n", GetLastError());
            TerminateProcess(g_proc, 1);
            return 2;
        }
        g_con_in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        g_con_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        SetConsoleCtrlHandler(NULL, TRUE); /* no Ctrl+C of the program's console reaches this process */
        /* the window exactly COLS x ROWS: shrink it, size the buffer, grow it back */
        SMALL_RECT one = {0, 0, 0, 0};
        SetConsoleWindowInfo(g_con_out, TRUE, &one);
        SetConsoleScreenBufferSize(g_con_out, (COORD){(SHORT)W, (SHORT)H});
        SMALL_RECT full = {0, 0, (SHORT)(W - 1), (SHORT)(H - 1)};
        SetConsoleWindowInfo(g_con_out, TRUE, &full);
        goto script;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    create_pc_t create_pc = (create_pc_t)(void *)GetProcAddress(k32, "CreatePseudoConsole");
    close_pc = (close_pc_t)(void *)GetProcAddress(k32, "ClosePseudoConsole");
    if (!create_pc) {
        fprintf(stderr, "conpty_drive: this Windows has no pseudo console (needs 10 1809+)\n");
        return 2;
    }
    HANDLE in_r, out_w;
    CreatePipe(&in_r, &g_in_w, NULL, 0);
    CreatePipe(&g_out_r, &out_w, NULL, 0);
    if (FAILED(create_pc((COORD){(SHORT)W, (SHORT)H}, in_r, out_w, 0, &pc))) {
        fprintf(stderr, "conpty_drive: CreatePseudoConsole failed\n");
        return 2;
    }
    CloseHandle(in_r);
    CloseHandle(out_w);
    wchar_t *cl = NULL;
    size_t cll = 0;
    for (int k = i; k < argc; k++) append_arg(&cl, &cll, argv[k]);
    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    si.lpAttributeList = malloc(asz);
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &asz);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pc, sizeof pc, NULL, NULL);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cl, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi)) {
        fprintf(stderr, "conpty_drive: cannot start the program (error %lu)\n", GetLastError());
        return 2;
    }
    g_proc = pi.hProcess;
    CloseHandle(pi.hThread);
    th = CreateThread(NULL, 0, reader, NULL, 0, NULL);

script:;
    int rc = 0;
    char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        size_t ll = strlen(line);
        while (ll && (line[ll - 1] == '\n' || line[ll - 1] == '\r')) line[--ll] = 0;
        if (!ll || line[0] == '#') continue;
        if (!strncmp(line, "sleep ", 6)) {
            Sleep((DWORD)atoi(line + 6));
        } else if (!strncmp(line, "raw ", 4)) {
            char buf[8192];
            size_t n;
            unescape(line + 4, buf, &n);
            if (g_console) console_send_raw(buf, n);
            else {
                DWORD w;
                WriteFile(g_in_w, buf, (DWORD)n, &w, NULL);
            }
        } else if (!strncmp(line, "send ", 5)) {
            char buf[8192];
            size_t n;
            unescape(line + 5, buf, &n);
            if (g_console) {
                console_send(buf, n);
                continue;
            }
            /* conhost's ConPTY input parser holds a CR that ends a write and drops it with the next one; a focus-in
             * report after it (consumed by conhost, never seen by the program) lets the Enter key through */
            if (n && buf[n - 1] == '\r' && n + 3 < sizeof buf) memcpy(buf + n, "\x1b[I", 3), n += 3;
            DWORD w;
            if (getenv("CONPTY_DRIVE_DEBUG")) {
                fprintf(stderr, "send %lu:", (unsigned long)n);
                for (size_t q = 0; q < n; q++) fprintf(stderr, " %02x", (unsigned char)buf[q]);
                fprintf(stderr, "\n");
            }
            WriteFile(g_in_w, buf, (DWORD)n, &w, NULL);
        } else if (!strncmp(line, "expect ", 7)) {
            char *text = line + 7, *sp = strrchr(text, ' ');
            int ms = 10000;
            if (sp && sp[1] >= '0' && sp[1] <= '9' && strspn(sp + 1, "0123456789") == strlen(sp + 1)) {
                ms = atoi(sp + 1);
                *sp = 0;
            }
            DWORD start = GetTickCount();
            bool found = false;
            while (!(found = screen_has(text)) && GetTickCount() - start < (DWORD)ms) Sleep(50);
            printf("%s expect \"%s\"\n", found ? "ok  " : "FAIL", text);
            if (!found) {
                print_screen();
                rc = 1;
                break;
            }
        } else if (!strncmp(line, "key ", 4)) {
            send_key(line + 4);
        } else if (!strcmp(line, "screen")) {
            Sleep(100);
            print_screen();
        } else if (!strncmp(line, "wait", 4)) {
            int ms = line[4] == ' ' ? atoi(line + 5) : 10000;
            if (WaitForSingleObject(g_proc, (DWORD)ms) != WAIT_OBJECT_0) {
                printf("FAIL program still running after %d ms\n", ms);
                rc = 1;
                break;
            }
            DWORD code = 0;
            GetExitCodeProcess(g_proc, &code);
            printf("exit %lu\n", code);
            rc = (int)code;
        }
    }
    fflush(stdout);
    if (WaitForSingleObject(g_proc, 0) == WAIT_TIMEOUT) TerminateProcess(g_proc, 1);
    if (pc) close_pc(pc);
    if (th) WaitForSingleObject(th, 2000);
    if (g_raw) fclose(g_raw);
    return rc;
}
