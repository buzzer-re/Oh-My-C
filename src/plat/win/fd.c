/* plat/win/fd.c - POSIX file descriptors on Windows.
 *
 * An fd is a CRT fd. The table here adds what the CRT lacks: the kind of handle behind it, O_NONBLOCK, and for a
 * socket the SOCKET itself (its CRT slot holds a NUL handle, so socket fds never collide with file fds).
 *  - read/write go to ReadFile/WriteFile (recv/send for sockets); a non-blocking pipe reports EAGAIN when empty.
 *  - the console is read as key events (raw mode) or lines (cooked mode) and written as UTF-16, so UTF-8 survives
 *    whatever code page the console has.
 *  - poll() waits on sockets with select(), on the console with its input event, and checks pipes (PeekNamedPipe)
 *    in short sleeps: anonymous pipes have nothing to wait on. */
#include "plat/win/win.h"

#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

/* this file implements the socket calls: below, the names are Winsock's own */
#undef socket
#undef connect
#undef bind
#undef listen
#undef accept
#undef accept4
#undef send
#undef recv
#undef sendto
#undef recvfrom
#undef setsockopt
#undef getsockopt
#undef shutdown
#undef getsockname
#undef getpeername
#undef getaddrinfo

#define FD_MAX 8192

typedef struct {
    unsigned char kind;
    bool nonblock;
    bool used;
    SOCKET sock;
    /* console input state, allocated on first use (con_in_t below) */
    struct con_in *con;
    /* console output: the start of a UTF-8 sequence split across writes */
    char out_part[4];
    unsigned char out_part_n;
} fdinfo_t;

static fdinfo_t g_fd[FD_MAX];
static SRWLOCK g_fd_lock = SRWLOCK_INIT;

void omc_win_fd_init(void) {}

static fdinfo_t *info(int fd) { return fd >= 0 && fd < FD_MAX ? &g_fd[fd] : NULL; }

static void fd_reset(fdinfo_t *i) {
    free(i->con);
    memset(i, 0, sizeof *i);
}

static int classify(HANDLE h) {
    if (h == INVALID_HANDLE_VALUE || !h) return FDK_UNKNOWN;
    switch (GetFileType(h)) {
    case FILE_TYPE_DISK: return FDK_FILE;
    case FILE_TYPE_PIPE: return FDK_PIPE;
    case FILE_TYPE_CHAR: {
        DWORD m;
        return GetConsoleMode(h, &m) ? FDK_CONSOLE : FDK_CHAR;
    }
    default: return FDK_UNKNOWN;
    }
}

HANDLE win_fd_get_handle(int fd) {
    fdinfo_t *i = info(fd);
    if (i && i->kind == FDK_SOCKET) return (HANDLE)i->sock;
    return (HANDLE)_get_osfhandle(fd);
}

int win_fd_kind(int fd) {
    fdinfo_t *i = info(fd);
    if (!i) return FDK_UNKNOWN;
    if (i->kind == FDK_SOCKET) return FDK_SOCKET;
    /* the handle behind a CRT fd can change (dup2 onto it), so non-sockets are classified on each use */
    return classify((HANDLE)_get_osfhandle(fd));
}

SOCKET win_fd_socket(int fd) {
    fdinfo_t *i = info(fd);
    return i && i->kind == FDK_SOCKET ? i->sock : INVALID_SOCKET;
}

bool omc_win_fd_is_socket(int fd) { return win_fd_kind(fd) == FDK_SOCKET; }

intptr_t omc_win_fd_handle(int fd) { return (intptr_t)win_fd_get_handle(fd); }

int omc_win_fd_from_handle(intptr_t h, int flags) {
    int fd = _open_osfhandle(h, _O_BINARY | (flags & O_APPEND ? _O_APPEND : 0));
    if (fd < 0) {
        errno = EMFILE;
        return -1;
    }
    fdinfo_t *i = info(fd);
    if (i) {
        AcquireSRWLockExclusive(&g_fd_lock);
        fd_reset(i);
        i->used = true;
        i->nonblock = (flags & O_NONBLOCK) != 0;
        ReleaseSRWLockExclusive(&g_fd_lock);
    }
    return fd;
}

int win_fd_new_socket(SOCKET s, bool nonblock) {
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (nul == INVALID_HANDLE_VALUE) {
        closesocket(s);
        return win_fail(GetLastError());
    }
    int fd = _open_osfhandle((intptr_t)nul, _O_BINARY);
    fdinfo_t *i = info(fd);
    if (fd < 0 || !i) {
        if (fd >= 0) _close(fd);
        else CloseHandle(nul);
        closesocket(s);
        errno = EMFILE;
        return -1;
    }
    AcquireSRWLockExclusive(&g_fd_lock);
    fd_reset(i);
    i->used = true;
    i->kind = FDK_SOCKET;
    i->sock = s;
    i->nonblock = nonblock;
    ReleaseSRWLockExclusive(&g_fd_lock);
    u_long nb = nonblock;
    ioctlsocket(s, FIONBIO, &nb);
    return fd;
}

void win_fd_set_nonblock(int fd, bool on) {
    fdinfo_t *i = info(fd);
    if (!i) return;
    i->nonblock = on;
    if (i->kind == FDK_SOCKET) {
        u_long nb = on;
        ioctlsocket(i->sock, FIONBIO, &nb);
    }
}

bool win_fd_nonblock(int fd) {
    fdinfo_t *i = info(fd);
    return i && i->nonblock;
}

void win_fd_forget(int fd) {
    fdinfo_t *i = info(fd);
    if (!i) return;
    AcquireSRWLockExclusive(&g_fd_lock);
    fd_reset(i);
    ReleaseSRWLockExclusive(&g_fd_lock);
}

/* ---------- console input ----------
 * Key-down records carry the characters (VT sequences included, with ENABLE_VIRTUAL_TERMINAL_INPUT). Once the TUI asks
 * for win32-input-mode (CSI ? 9001 h, see tui/term.c), the console host reports every key as the text
 * `CSI Vk ; Sc ; Uc ; Kd ; Cs ; Rc _`, which carries the modifiers legacy VT loses (Shift+Enter is a bare CR there).
 * Those records are decoded here, as omp's windows-input-mode.ts Win32InputModeDecoder does: plain keys become the
 * bytes a legacy terminal sends, chords legacy encoding cannot carry become kitty CSI-u, and relayed text records
 * (Vk 0) become their text, so the TUI's key parser reads one byte stream either way. */

typedef struct con_in {
    char pend[8192]; /* decoded bytes not yet read */
    size_t pend_n, pend_off;
    wchar_t hi_sur;  /* a high surrogate waiting for its pair (record text) */
    wchar_t raw_hi;  /* the same for plain characters */
    char hold[64];   /* an ESC-started prefix that may still become a win32-input-mode record */
    size_t hold_n;
} con_in_t;

static con_in_t *con_state(fdinfo_t *fi) {
    if (!fi->con) fi->con = calloc(1, sizeof *fi->con);
    return fi->con;
}

static void pend_bytes(con_in_t *c, const char *s, size_t n) {
    if (c->pend_off == c->pend_n) c->pend_off = c->pend_n = 0;
    if (c->pend_n + n > sizeof c->pend) {
        memmove(c->pend, c->pend + c->pend_off, c->pend_n - c->pend_off);
        c->pend_n -= c->pend_off;
        c->pend_off = 0;
    }
    if (c->pend_n + n > sizeof c->pend) n = sizeof c->pend - c->pend_n; /* a flood: the rest is dropped */
    memcpy(c->pend + c->pend_n, s, n);
    c->pend_n += n;
}

static void pend_str(con_in_t *c, const char *s) { pend_bytes(c, s, strlen(s)); }

/* one UTF-16 unit as UTF-8 into the decoded stream (surrogate halves paired through *hi) */
static void pend_wchar(con_in_t *c, wchar_t w, wchar_t *hi, int repeat) {
    wchar_t pair[2];
    int wn = 0;
    if (w >= 0xD800 && w <= 0xDBFF) {
        *hi = w;
        return;
    }
    if (w >= 0xDC00 && w <= 0xDFFF) {
        if (!*hi) return;
        pair[wn++] = *hi;
    }
    *hi = 0;
    pair[wn++] = w;
    char buf[8];
    int n = WideCharToMultiByte(CP_UTF8, 0, pair, wn, buf, sizeof buf, NULL, NULL);
    for (int r = 0; n > 0 && r < (repeat > 0 ? repeat : 1); r++) pend_bytes(c, buf, (size_t)n);
}

/* windows-input-mode.ts encodeKey: the bytes for one key-down record, "" for none */
static void w32_encode(int vk, int uc, int state, char *out, size_t cap) {
    enum { RALT = 1, LALT = 2, RCTRL = 4, LCTRL = 8, SHIFT = 16 };
    bool shift = state & SHIFT, alt = state & (LALT | RALT), ctrl = state & (LCTRL | RCTRL);
    int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    char text[8] = "";
    if (uc) {
        wchar_t w = (wchar_t)uc;
        int n = WideCharToMultiByte(CP_UTF8, 0, &w, 1, text, sizeof text - 1, NULL, NULL);
        text[n > 0 ? n : 0] = 0;
    }
    out[0] = 0;
    /* a pending dead key composes onto Space/Enter/Tab/Backspace/Escape: the carried text is the input */
    if (!ctrl && !alt && uc > 0x20 && uc != 0x7f && (vk == 0x08 || vk == 0x09 || vk == 0x0d || vk == 0x1b || vk == 0x20)) {
        snprintf(out, cap, "%s", text);
        return;
    }
    switch (vk) {
    case 0x0d: snprintf(out, cap, mod == 1 ? "\r" : "\x1b[13;%du", mod); return;
    case 0x09:
        if (mod == 1) snprintf(out, cap, "\t");
        else if (mod == 2) snprintf(out, cap, "\x1b[Z");
        else snprintf(out, cap, "\x1b[9;%du", mod);
        return;
    case 0x08:
        if (mod == 1) snprintf(out, cap, "\x7f");
        else if (mod == 3) snprintf(out, cap, "\x1b\x7f");
        else snprintf(out, cap, "\x1b[127;%du", mod);
        return;
    case 0x1b: snprintf(out, cap, mod == 1 ? "\x1b" : "\x1b[27;%du", mod); return;
    case 0x20:
        if (!ctrl && !alt) snprintf(out, cap, " ");
        else snprintf(out, cap, "\x1b[32;%du", mod);
        return;
    }
    static const struct {
        int vk;
        char fin;
        bool ss3;
    } CURSOR[] = {{0x23, 'F', 0}, {0x24, 'H', 0}, {0x25, 'D', 0}, {0x26, 'A', 0}, {0x27, 'C', 0},
                  {0x28, 'B', 0}, {0x70, 'P', 1}, {0x71, 'Q', 1}, {0x72, 'R', 1}, {0x73, 'S', 1}};
    for (size_t i = 0; i < ARRAYSIZE(CURSOR); i++)
        if (CURSOR[i].vk == vk) {
            if (mod != 1) snprintf(out, cap, "\x1b[1;%d%c", mod, CURSOR[i].fin);
            else snprintf(out, cap, CURSOR[i].ss3 ? "\x1bO%c" : "\x1b[%c", CURSOR[i].fin);
            return;
        }
    static const int TILDE[][2] = {{0x21, 5},  {0x22, 6},  {0x2d, 2},  {0x2e, 3},  {0x74, 15}, {0x75, 17},
                                   {0x76, 18}, {0x77, 19}, {0x78, 20}, {0x79, 21}, {0x7a, 23}, {0x7b, 24}};
    for (size_t i = 0; i < ARRAYSIZE(TILDE); i++)
        if (TILDE[i][0] == vk) {
            if (mod == 1) snprintf(out, cap, "\x1b[%d~", TILDE[i][1]);
            else snprintf(out, cap, "\x1b[%d;%d~", TILDE[i][1], mod);
            return;
        }
    /* AltGr (Right Alt + Left Ctrl) that composed printable text: the text */
    if ((state & RALT) && (state & LCTRL) && uc >= 0x20 && uc != 0x7f) {
        snprintf(out, cap, "%s", text);
        return;
    }
    int base = vk >= 0x41 && vk <= 0x5a ? vk + 0x20 : vk >= 0x30 && vk <= 0x39 ? vk : 0;
    if (ctrl) {
        /* Ctrl+J keeps its identity (Uc is LF, which would read as Enter); Ctrl+Shift+letter stays distinct */
        bool ctrl_j = !alt && vk == 0x4a && uc == 0x0a;
        if ((shift || uc == 0 || ctrl_j) && base) snprintf(out, cap, "\x1b[%d;%du", base, mod);
        else if (uc) snprintf(out, cap, alt ? "\x1b%s" : "%s", text);
        return;
    }
    if (alt) {
        if (uc) snprintf(out, cap, "\x1b%s", text);
        else if (base) snprintf(out, cap, "\x1b%c", base);
        return;
    }
    if (uc) snprintf(out, cap, "%s", text); /* Uc 0: a dead key, composed with the next one */
}

static bool is_modifier_vk(int vk) {
    return vk == 0x10 || vk == 0x11 || vk == 0x12 || vk == 0x14 || vk == 0x5b || vk == 0x5c || vk == 0x90 || vk == 0x91 ||
           (vk >= 0xa0 && vk <= 0xa5);
}

/* a complete `ESC [ params _` record in hold: decode it into pend */
static void w32_record(con_in_t *c) {
    /* fields accumulate from 0; an omitted or zero repeat count (Rc) means 1, as in omp's decoder */
    int p[6] = {0, 0, 0, 0, 0, 0}, np = 0;
    for (size_t i = 2; i + 1 < c->hold_n && np < 6; i++) {
        if (c->hold[i] == ';') np++;
        else if (p[np] < 1000000) p[np] = p[np] * 10 + (c->hold[i] - '0');
    }
    int vk = p[0], uc = p[2], down = p[3] == 1, state = p[4], repeat = p[5] < 1 ? 1 : p[5] > 0xffff ? 0xffff : p[5];
    if (!down) {
        /* Alt+Numpad composition delivers its character on the Alt release */
        if (vk == 0x12 && uc) pend_wchar(c, (wchar_t)uc, &c->hi_sur, 1);
        return;
    }
    if (is_modifier_vk(vk)) return;
    if (vk == 0 || vk == 0xe7 || (uc >= 0xd800 && uc <= 0xdfff)) {
        pend_wchar(c, (wchar_t)uc, &c->hi_sur, vk == 0 || vk == 0xe7 ? repeat : 1);
        return;
    }
    c->hi_sur = 0;
    char enc[32];
    w32_encode(vk, uc, state, enc, sizeof enc);
    for (int r = 0; r < repeat && enc[0]; r++) pend_str(c, enc);
}

/* feed one decoded console byte through the record detector */
static void con_feed(con_in_t *c, char b) {
    if (!c->hold_n && b != 0x1b) {
        pend_bytes(c, &b, 1);
        return;
    }
    if (c->hold_n && b == 0x1b) { /* a new ESC: what was held is not a record */
        pend_bytes(c, c->hold, c->hold_n);
        c->hold_n = 0;
    }
    c->hold[c->hold_n++] = b;
    if (c->hold_n == 1) return;
    if (c->hold[1] != '[') {
        pend_bytes(c, c->hold, c->hold_n);
        c->hold_n = 0;
        return;
    }
    if (c->hold_n == 2) return;
    if ((b >= '0' && b <= '9') || b == ';') {
        if (c->hold_n < sizeof c->hold) return;
    } else if (b == '_') {
        w32_record(c);
        c->hold_n = 0;
        return;
    }
    pend_bytes(c, c->hold, c->hold_n); /* any other CSI (a reply, a legacy key) passes through as it came */
    c->hold_n = 0;
}

/* release a held prefix once no more input followed: a lone ESC is the Escape key */
static void con_release(con_in_t *c) {
    if (!c->hold_n) return;
    pend_bytes(c, c->hold, c->hold_n);
    c->hold_n = 0;
}

static void con_feed_wchar(con_in_t *c, wchar_t w, int repeat) {
    wchar_t pair[2];
    int wn = 0;
    if (w >= 0xD800 && w <= 0xDBFF) {
        c->raw_hi = w;
        return;
    }
    if (w >= 0xDC00 && w <= 0xDFFF) {
        if (!c->raw_hi) return;
        pair[wn++] = c->raw_hi;
    }
    c->raw_hi = 0;
    pair[wn++] = w;
    char buf[8];
    int n = WideCharToMultiByte(CP_UTF8, 0, pair, wn, buf, sizeof buf, NULL, NULL);
    for (int r = 0; r < (repeat > 0 ? repeat : 1); r++)
        for (int i = 0; i < n; i++) con_feed(c, buf[i]);
}

/* move pending console input events into the decoded buffer; a buffer-size event is a resize. Returns true when
 * bytes are pending. */
static bool console_pump(HANDLE h, fdinfo_t *fi, bool block) {
    con_in_t *c = con_state(fi);
    if (!c) return false;
    for (;;) {
        if (c->pend_off < c->pend_n) return true;
        c->pend_off = c->pend_n = 0;
        DWORD avail = 0;
        if (!GetNumberOfConsoleInputEvents(h, &avail)) return false;
        if (!avail) {
            if (c->hold_n) {
                /* a record is written into the input queue whole: give the rest of one a moment, then let go */
                if (WaitForSingleObject(h, 30) == WAIT_OBJECT_0) continue;
                con_release(c);
                continue;
            }
            if (!block) return false;
            if (WaitForSingleObject(h, INFINITE) != WAIT_OBJECT_0) return false;
            continue;
        }
        INPUT_RECORD rec[64];
        DWORD got = 0;
        if (!ReadConsoleInputW(h, rec, ARRAYSIZE(rec), &got)) return false;
        for (DWORD k = 0; k < got; k++) {
            if (rec[k].EventType == WINDOW_BUFFER_SIZE_EVENT) {
                win_console_note_resize();
                continue;
            }
            if (rec[k].EventType != KEY_EVENT) continue;
            KEY_EVENT_RECORD *ke = &rec[k].Event.KeyEvent;
            wchar_t ch = ke->uChar.UnicodeChar;
            /* key-ups carry nothing, except Alt+Numpad composition which delivers on the Alt release */
            if (!ke->bKeyDown && !(ke->wVirtualKeyCode == VK_MENU && ch)) continue;
            if (!ch) continue;
            con_feed_wchar(c, ch, ke->wRepeatCount ? ke->wRepeatCount : 1);
        }
    }
}

static bool console_line_mode(HANDLE h) {
    DWORD m;
    return GetConsoleMode(h, &m) && (m & ENABLE_LINE_INPUT);
}

static ssize_t console_read(int fd, HANDLE h, void *buf, size_t n) {
    fdinfo_t *fi = info(fd);
    con_in_t *c = fi ? con_state(fi) : NULL;
    if (!c) return -1;
    if (console_line_mode(h)) {
        /* cooked: a line as the console edits it, through UTF-16 */
        if (c->pend_off >= c->pend_n) {
            wchar_t w[16];
            DWORD got = 0;
            if (!ReadConsoleW(h, w, ARRAYSIZE(w), &got, NULL)) return win_fail(GetLastError());
            if (!got) return 0;
            c->pend_off = c->pend_n = 0;
            for (DWORD k = 0; k < got; k++) {
                if (w[k] == L'\r') continue;
                if (w[k] == 0x1A) return 0; /* Ctrl+Z: end of input */
                pend_wchar(c, w[k], &c->raw_hi, 1);
            }
            if (c->pend_n == 0) return console_read(fd, h, buf, n);
        }
    } else if (!console_pump(h, fi, !fi->nonblock)) {
        errno = EAGAIN;
        return -1;
    }
    size_t k = c->pend_n - c->pend_off;
    if (k > n) k = n;
    memcpy(buf, c->pend + c->pend_off, k);
    c->pend_off += k;
    return (ssize_t)k;
}

static bool console_readable(int fd, HANDLE h) {
    fdinfo_t *fi = info(fd);
    if (!fi) return false;
    if (console_line_mode(h)) {
        DWORD avail = 0;
        return (fi->con && fi->con->pend_off < fi->con->pend_n) || (GetNumberOfConsoleInputEvents(h, &avail) && avail);
    }
    return console_pump(h, fi, false);
}

/* ---------- console output ---------- */

static size_t utf8_len(unsigned char c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }

static ssize_t console_write(int fd, HANDLE h, const void *buf, size_t n) {
    fdinfo_t *fi = info(fd);
    const unsigned char *p = buf;
    size_t total = n;
    char tmp[4 + 8192];
    while (n || (fi && fi->out_part_n)) {
        size_t tl = 0;
        if (fi && fi->out_part_n) {
            memcpy(tmp, fi->out_part, fi->out_part_n);
            tl = fi->out_part_n;
            fi->out_part_n = 0;
        }
        size_t take = n > 8192 ? 8192 : n;
        memcpy(tmp + tl, p, take);
        tl += take;
        p += take;
        n -= take;
        /* hold back an incomplete sequence at the end */
        size_t cut = tl, back = 0;
        while (back < 3 && back < tl && (((unsigned char)tmp[tl - 1 - back]) & 0xC0) == 0x80) back++;
        if (back < tl) {
            unsigned char lead = (unsigned char)tmp[tl - 1 - back];
            if (lead >= 0xC0 && utf8_len(lead) > back + 1) cut = tl - 1 - back;
        }
        if (fi && cut < tl) {
            fi->out_part_n = (unsigned char)(tl - cut);
            memcpy(fi->out_part, tmp + cut, tl - cut);
        }
        if (cut) {
            wchar_t *w = win_wide_n(tmp, cut);
            if (!w) return -1;
            DWORD wl = (DWORD)wcslen(w), done = 0;
            while (done < wl) {
                DWORD wrote = 0;
                if (!WriteConsoleW(h, w + done, wl - done, &wrote, NULL)) {
                    free(w);
                    return win_fail(GetLastError());
                }
                done += wrote;
            }
            free(w);
        }
        if (!n) break;
    }
    return (ssize_t)total;
}

/* ---------- read / write / close ---------- */

static ssize_t sock_err(void) {
    errno = win_errno_from_wsa(WSAGetLastError());
    return -1;
}

ssize_t omc_win_read(int fd, void *buf, size_t n) {
    fdinfo_t *fi = info(fd);
    if (fi && fi->kind == FDK_SOCKET) {
        int r = recv(fi->sock, buf, n > INT_MAX ? INT_MAX : (int)n, 0);
        return r == SOCKET_ERROR ? sock_err() : r;
    }
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    int kind = classify(h);
    if (kind == FDK_CONSOLE) return console_read(fd, h, buf, n);
    DWORD want = n > 0x7FFFFFFF ? 0x7FFFFFFF : (DWORD)n;
    if (kind == FDK_PIPE && fi && fi->nonblock) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
            DWORD e = GetLastError();
            if (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED) return 0;
            return win_fail(e);
        }
        if (!avail) {
            errno = EAGAIN;
            return -1;
        }
        if (want > avail) want = avail;
    }
    DWORD got = 0;
    if (!ReadFile(h, buf, want, &got, NULL)) {
        DWORD e = GetLastError();
        if (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF || e == ERROR_PIPE_NOT_CONNECTED) return 0;
        if (e == ERROR_ACCESS_DENIED && kind == FDK_FILE) {
            struct omc_win_stat st;
            if (omc_win_fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
                errno = EISDIR;
                return -1;
            }
        }
        return win_fail(e);
    }
    return (ssize_t)got;
}

ssize_t omc_win_write(int fd, const void *buf, size_t n) {
    fdinfo_t *fi = info(fd);
    if (fi && fi->kind == FDK_SOCKET) {
        int r = send(fi->sock, buf, n > INT_MAX ? INT_MAX : (int)n, 0);
        return r == SOCKET_ERROR ? sock_err() : r;
    }
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    int kind = classify(h);
    if (kind == FDK_CONSOLE) return console_write(fd, h, buf, n);
    /* O_APPEND: the handle has FILE_APPEND_DATA without FILE_WRITE_DATA, so every write lands at the end */
    DWORD want = n > 0x7FFFFFFF ? 0x7FFFFFFF : (DWORD)n, wrote = 0;
    if (!WriteFile(h, buf, want, &wrote, NULL)) {
        DWORD e = GetLastError();
        if (e == ERROR_NO_DATA || e == ERROR_BROKEN_PIPE) {
            errno = EPIPE;
            return -1;
        }
        return win_fail(e);
    }
    return (ssize_t)wrote;
}

int omc_win_close(int fd) {
    fdinfo_t *fi = info(fd);
    if (fi && fi->kind == FDK_SOCKET) {
        SOCKET s = fi->sock;
        win_fd_forget(fd);
        closesocket(s);
        return _close(fd);
    }
    if (fi) win_fd_forget(fd);
    return _close(fd);
}

ssize_t omc_win_pread(int fd, void *buf, size_t n, long long off) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)((unsigned long long)off >> 32);
    DWORD got = 0;
    if (!ReadFile(h, buf, n > 0x7FFFFFFF ? 0x7FFFFFFF : (DWORD)n, &got, &ov)) {
        DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF) return 0;
        return win_fail(e);
    }
    return (ssize_t)got;
}

ssize_t omc_win_pwrite(int fd, const void *buf, size_t n, long long off) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)((unsigned long long)off >> 32);
    DWORD wrote = 0;
    if (!WriteFile(h, buf, n > 0x7FFFFFFF ? 0x7FFFFFFF : (DWORD)n, &wrote, &ov)) return win_fail(GetLastError());
    return (ssize_t)wrote;
}

long long omc_win_lseek(int fd, long long off, int whence) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (GetFileType(h) != FILE_TYPE_DISK) {
        errno = ESPIPE;
        return -1;
    }
    LARGE_INTEGER d = {.QuadPart = off}, out;
    DWORD m = whence == SEEK_CUR ? FILE_CURRENT : whence == SEEK_END ? FILE_END : FILE_BEGIN;
    if (!SetFilePointerEx(h, d, &out, m)) return win_fail(GetLastError());
    return out.QuadPart;
}

int omc_win_dup(int fd) {
    fdinfo_t *fi = info(fd);
    if (fi && fi->kind == FDK_SOCKET) {
        WSAPROTOCOL_INFOW pi;
        if (WSADuplicateSocketW(fi->sock, GetCurrentProcessId(), &pi) != 0) return (int)sock_err();
        SOCKET s = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &pi, 0, WSA_FLAG_OVERLAPPED);
        if (s == INVALID_SOCKET) return (int)sock_err();
        return win_fd_new_socket(s, fi->nonblock);
    }
    int r = _dup(fd);
    if (r >= 0 && fi && info(r)) {
        fdinfo_t *ri = info(r);
        fd_reset(ri);
        ri->nonblock = fi->nonblock;
    }
    return r;
}

int omc_win_dup2(int fd, int fd2) {
    if (fd == fd2) return fd2;
    if (win_fd_kind(fd) == FDK_SOCKET) {
        errno = ENOTSUP;
        return -1;
    }
    if (win_fd_kind(fd2) == FDK_SOCKET) omc_win_close(fd2);
    if (_dup2(fd, fd2) != 0) return -1;
    fdinfo_t *a = info(fd), *b = info(fd2);
    if (b) {
        fd_reset(b);
        if (a) b->nonblock = a->nonblock;
    }
    /* the process's standard handles follow fds 0-2, so children started later inherit the new ones */
    if (fd2 <= 2) {
        DWORD which = fd2 == 0 ? STD_INPUT_HANDLE : fd2 == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE;
        SetStdHandle(which, (HANDLE)_get_osfhandle(fd2));
    }
    return fd2;
}

/* ---------- pipes ---------- */

int omc_win_pipe2(int fds[2], int flags) {
    HANDLE r, w;
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, FALSE};
    if (!CreatePipe(&r, &w, &sa, 1 << 16)) return win_fail(GetLastError());
    int a = _open_osfhandle((intptr_t)r, _O_BINARY | _O_RDONLY);
    if (a < 0) {
        CloseHandle(r);
        CloseHandle(w);
        errno = EMFILE;
        return -1;
    }
    int b = _open_osfhandle((intptr_t)w, _O_BINARY | _O_WRONLY);
    if (b < 0) {
        _close(a);
        CloseHandle(w);
        errno = EMFILE;
        return -1;
    }
    win_fd_forget(a);
    win_fd_forget(b);
    if (flags & O_NONBLOCK) {
        win_fd_set_nonblock(a, true);
        win_fd_set_nonblock(b, true);
    }
    fds[0] = a;
    fds[1] = b;
    return 0;
}

int omc_win_pipe(int fds[2]) { return omc_win_pipe2(fds, 0); }

/* ---------- fcntl / ioctl / isatty ---------- */

int omc_win_fcntl_lock(int fd, int cmd, struct omc_win_flock *fl);

int omc_win_fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    int ret = 0;
    switch (cmd) {
    case F_GETFD: {
        HANDLE h = win_fd_get_handle(fd);
        DWORD f = 0;
        ret = (h != INVALID_HANDLE_VALUE && GetHandleInformation(h, &f) && (f & HANDLE_FLAG_INHERIT)) ? 0 : FD_CLOEXEC;
        break;
    }
    case F_SETFD: {
        int v = va_arg(ap, int);
        HANDLE h = win_fd_get_handle(fd);
        if (h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, (v & FD_CLOEXEC) ? 0 : HANDLE_FLAG_INHERIT);
        break;
    }
    case F_GETFL: ret = (win_fd_nonblock(fd) ? O_NONBLOCK : 0) | O_RDWR; break;
    case F_SETFL: {
        int v = va_arg(ap, int);
        win_fd_set_nonblock(fd, (v & O_NONBLOCK) != 0);
        break;
    }
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: ret = omc_win_dup(fd); break;
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW: ret = omc_win_fcntl_lock(fd, cmd, va_arg(ap, struct omc_win_flock *)); break;
    default: errno = EINVAL; ret = -1;
    }
    va_end(ap);
    return ret;
}

int omc_win_isatty(int fd) {
    if (win_fd_kind(fd) == FDK_CONSOLE) return 1;
    errno = ENOTTY;
    return 0;
}

static bool console_size(HANDLE h, struct winsize *ws) {
    CONSOLE_SCREEN_BUFFER_INFO ci;
    if (!GetConsoleScreenBufferInfo(h, &ci)) {
        HANDLE o = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (o == INVALID_HANDLE_VALUE) return false;
        BOOL ok = GetConsoleScreenBufferInfo(o, &ci);
        CloseHandle(o);
        if (!ok) return false;
    }
    ws->ws_col = (unsigned short)(ci.srWindow.Right - ci.srWindow.Left + 1);
    ws->ws_row = (unsigned short)(ci.srWindow.Bottom - ci.srWindow.Top + 1);
    ws->ws_xpixel = ws->ws_ypixel = 0;
    return true;
}

int omc_win_ioctl(int fd, unsigned long req, ...) {
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (req == TIOCGWINSZ) {
        if (win_fd_kind(fd) != FDK_CONSOLE || !console_size(win_fd_get_handle(fd), arg)) {
            errno = ENOTTY;
            return -1;
        }
        return 0;
    }
    if (req == FIONREAD) {
        int *out = arg;
        int k = win_fd_kind(fd);
        if (k == FDK_SOCKET) {
            u_long n = 0;
            if (ioctlsocket(win_fd_socket(fd), FIONREAD, &n) != 0) return (int)sock_err();
            *out = (int)n;
            return 0;
        }
        if (k == FDK_PIPE) {
            DWORD avail = 0;
            PeekNamedPipe(win_fd_get_handle(fd), NULL, 0, NULL, &avail, NULL);
            *out = (int)avail;
            return 0;
        }
        *out = 0;
        return 0;
    }
    if (req == FIONBIO) {
        win_fd_set_nonblock(fd, *(int *)arg != 0);
        return 0;
    }
    if (req == TIOCSCTTY || req == TIOCNOTTY || req == TIOCSWINSZ) return 0;
    errno = ENOTTY;
    return -1;
}

/* ---------- poll ---------- */

int omc_win_poll(struct pollfd *fds, nfds_t n, int timeout_ms) {
    ULONGLONG start = GetTickCount64();
    unsigned spins = 0;
    for (;;) {
        int ready = 0;
        bool only_sockets = n > 0;
        HANDLE con = NULL;
        fd_set rs, ws, es;
        FD_ZERO(&rs);
        FD_ZERO(&ws);
        FD_ZERO(&es);
        int nsock = 0;
        for (nfds_t i = 0; i < n; i++) {
            fds[i].revents = 0;
            int fd = (int)fds[i].fd;
            if (fd < 0) continue;
            int kind = win_fd_kind(fd);
            if (kind == FDK_SOCKET) {
                SOCKET s = win_fd_socket(fd);
                if (nsock < FD_SETSIZE) {
                    if (fds[i].events & (POLLIN | POLLRDNORM)) FD_SET(s, &rs);
                    if (fds[i].events & (POLLOUT | POLLWRNORM)) FD_SET(s, &ws);
                    FD_SET(s, &es);
                    nsock++;
                }
                continue;
            }
            only_sockets = false;
            HANDLE h = (HANDLE)_get_osfhandle(fd);
            if (h == INVALID_HANDLE_VALUE) {
                fds[i].revents = POLLNVAL;
                ready++;
                continue;
            }
            short ev = 0;
            switch (kind) {
            case FDK_PIPE: {
                DWORD avail = 0;
                if (fds[i].events & POLLIN) {
                    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
                        DWORD e = GetLastError();
                        /* the write end of a pipe can't be peeked: report it writable */
                        if (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED) ev |= POLLHUP;
                        else if (e == ERROR_ACCESS_DENIED) ev |= (fds[i].events & POLLOUT);
                        else ev |= POLLERR;
                    } else if (avail) {
                        ev |= POLLIN;
                    }
                }
                if (fds[i].events & POLLOUT) ev |= POLLOUT;
                break;
            }
            case FDK_CONSOLE:
                if ((fds[i].events & POLLIN) && console_readable(fd, h)) ev |= POLLIN;
                if (fds[i].events & POLLOUT) ev |= POLLOUT;
                if (fds[i].events & POLLIN) con = h;
                break;
            default: /* files and devices never block */
                ev = fds[i].events & (POLLIN | POLLOUT);
                break;
            }
            fds[i].revents = ev;
            if (ev) ready++;
        }
        if (nsock) {
            ULONGLONG el = GetTickCount64() - start;
            long wait_ms = 0;
            if (only_sockets && !ready) wait_ms = timeout_ms < 0 ? -1 : (long)(timeout_ms > (int)el ? timeout_ms - (int)el : 0);
            struct timeval tv = {wait_ms / 1000, (wait_ms % 1000) * 1000};
            int r = select(0, &rs, &ws, &es, wait_ms < 0 ? NULL : &tv);
            if (r == SOCKET_ERROR) return (int)sock_err();
            if (r > 0) {
                for (nfds_t i = 0; i < n; i++) {
                    int fd = (int)fds[i].fd;
                    if (fd < 0 || win_fd_kind(fd) != FDK_SOCKET) continue;
                    SOCKET s = win_fd_socket(fd);
                    short ev = 0;
                    if (FD_ISSET(s, &rs)) ev |= POLLIN;
                    if (FD_ISSET(s, &ws)) ev |= POLLOUT;
                    if (FD_ISSET(s, &es)) ev |= POLLERR;
                    fds[i].revents = ev;
                    if (ev) ready++;
                }
            }
            if (only_sockets) return ready;
        }
        if (ready) return ready;
        ULONGLONG el = GetTickCount64() - start;
        if (timeout_ms >= 0 && el >= (ULONGLONG)timeout_ms) return 0;
        DWORD slice = spins < 20 ? 1 : spins < 100 ? 5 : 10;
        if (timeout_ms >= 0 && el + slice > (ULONGLONG)timeout_ms) slice = (DWORD)((ULONGLONG)timeout_ms - el);
        spins++;
        if (con) WaitForSingleObject(con, slice);
        else Sleep(slice);
    }
}

/* ---------- sockets ---------- */

/* Winsock needs WSAStartup before any call: done here for code that reaches sockets before omc_main/omc_session_new
 * (the unit tests' own mains, library hosts) */
static void wsa_ready(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    InitOnceBeginInitialize(&once, 0, &pending, NULL);
    if (pending) {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
        InitOnceComplete(&once, 0, NULL);
    }
}

int omc_win_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    wsa_ready();
    return getaddrinfo(node, service, hints, res);
}

int omc_win_socket(int domain, int type, int proto) {
    wsa_ready();
    int base = type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK);
    SOCKET s = WSASocketW(domain, base, proto, NULL, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) return (int)sock_err();
    return win_fd_new_socket(s, (type & SOCK_NONBLOCK) != 0);
}

#define SOCK_OR_FAIL(fd)                                                                                                 \
    SOCKET s = win_fd_socket(fd);                                                                                      \
    if (s == INVALID_SOCKET) {                                                                                         \
        errno = ENOTSOCK;                                                                                              \
        return -1;                                                                                                     \
    }

int omc_win_connect(int fd, const struct sockaddr *sa, socklen_t len) {
    SOCK_OR_FAIL(fd);
    if (connect(s, sa, len) == 0) return 0;
    int e = WSAGetLastError();
    errno = e == WSAEWOULDBLOCK ? EINPROGRESS : win_errno_from_wsa(e);
    return -1;
}

int omc_win_bind(int fd, const struct sockaddr *sa, socklen_t len) {
    SOCK_OR_FAIL(fd);
    return bind(s, sa, len) == 0 ? 0 : (int)sock_err();
}

int omc_win_listen(int fd, int backlog) {
    SOCK_OR_FAIL(fd);
    return listen(s, backlog) == 0 ? 0 : (int)sock_err();
}

int omc_win_accept4(int fd, struct sockaddr *sa, socklen_t *len, int flags) {
    SOCK_OR_FAIL(fd);
    SOCKET c = accept(s, sa, len);
    if (c == INVALID_SOCKET) return (int)sock_err();
    SetHandleInformation((HANDLE)c, HANDLE_FLAG_INHERIT, 0);
    return win_fd_new_socket(c, (flags & SOCK_NONBLOCK) != 0);
}

int omc_win_accept(int fd, struct sockaddr *sa, socklen_t *len) { return omc_win_accept4(fd, sa, len, 0); }

ssize_t omc_win_send(int fd, const void *buf, size_t n, int flags) {
    SOCK_OR_FAIL(fd);
    int r = send(s, buf, n > INT_MAX ? INT_MAX : (int)n, flags & ~MSG_NOSIGNAL);
    return r == SOCKET_ERROR ? sock_err() : r;
}

ssize_t omc_win_recv(int fd, void *buf, size_t n, int flags) {
    SOCK_OR_FAIL(fd);
    int r = recv(s, buf, n > INT_MAX ? INT_MAX : (int)n, flags);
    return r == SOCKET_ERROR ? sock_err() : r;
}

ssize_t omc_win_sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *sa, socklen_t len) {
    SOCK_OR_FAIL(fd);
    int r = sendto(s, buf, n > INT_MAX ? INT_MAX : (int)n, flags, sa, len);
    return r == SOCKET_ERROR ? sock_err() : r;
}

ssize_t omc_win_recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *sa, socklen_t *len) {
    SOCK_OR_FAIL(fd);
    int r = recvfrom(s, buf, n > INT_MAX ? INT_MAX : (int)n, flags, sa, len);
    return r == SOCKET_ERROR ? sock_err() : r;
}

int omc_win_setsockopt(int fd, int level, int opt, const void *val, socklen_t len) {
    SOCK_OR_FAIL(fd);
    if (level == SOL_SOCKET && opt == SO_NOSIGPIPE) return 0;
    return setsockopt(s, level, opt, val, len) == 0 ? 0 : (int)sock_err();
}

int omc_win_getsockopt(int fd, int level, int opt, void *val, socklen_t *len) {
    SOCK_OR_FAIL(fd);
    if (getsockopt(s, level, opt, val, len) != 0) return (int)sock_err();
    /* SO_ERROR carries a Winsock code: callers hand it to strerror() */
    if (level == SOL_SOCKET && opt == SO_ERROR && *len >= (socklen_t)sizeof(int) && *(int *)val)
        *(int *)val = win_errno_from_wsa(*(int *)val);
    return 0;
}

int omc_win_shutdown(int fd, int how) {
    SOCK_OR_FAIL(fd);
    return shutdown(s, how) == 0 ? 0 : (int)sock_err();
}

int omc_win_getsockname(int fd, struct sockaddr *sa, socklen_t *len) {
    SOCK_OR_FAIL(fd);
    return getsockname(s, sa, len) == 0 ? 0 : (int)sock_err();
}

int omc_win_getpeername(int fd, struct sockaddr *sa, socklen_t *len) {
    SOCK_OR_FAIL(fd);
    return getpeername(s, sa, len) == 0 ? 0 : (int)sock_err();
}
