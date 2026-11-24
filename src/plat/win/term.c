/* plat/win/term.c - termios over the Windows console.
 *
 * tcgetattr snapshots the console's input and output modes; tcsetattr maps the termios flags omc toggles onto them:
 * ICANON -> ENABLE_LINE_INPUT, ECHO -> ENABLE_ECHO_INPUT, ISIG -> ENABLE_PROCESSED_INPUT (Ctrl+C as a signal, not a
 * byte). Raw mode is libuv's UV_TTY_MODE_RAW_VT (what Bun's setRawMode uses): keys arrive as VT sequences
 * (ENABLE_VIRTUAL_TERMINAL_INPUT) and resizes as window events. Output always keeps VT processing on. A snapshot
 * that tcsetattr is handed back unchanged restores the exact modes it was taken from. */
#include "plat/win/win.h"

#include <termios.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

static HANDLE con_in(int fd) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    DWORD m;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &m) && GetFileType(h) == FILE_TYPE_CHAR) {
        /* an output handle answers GetConsoleMode too: tell them apart by what the input API accepts */
        DWORD n;
        if (GetNumberOfConsoleInputEvents(h, &n)) return h;
    }
    return GetStdHandle(STD_INPUT_HANDLE);
}

static HANDLE con_out(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD m;
    if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &m)) return h;
    h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &m)) return h;
    return NULL;
}

static void flags_from_modes(struct termios *t, DWORD in, DWORD out) {
    memset(t, 0, sizeof *t);
    t->c_iflag = ICRNL | IXON | IUTF8;
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = CS8 | CREAD;
    if (in & ENABLE_LINE_INPUT) t->c_lflag |= ICANON | IEXTEN;
    if (in & ENABLE_ECHO_INPUT) t->c_lflag |= ECHO | ECHOE | ECHOK;
    if (in & ENABLE_PROCESSED_INPUT) t->c_lflag |= ISIG;
    t->c_cc[VINTR] = 3;
    t->c_cc[VEOF] = 26;
    t->c_cc[VMIN] = 1;
    t->win_in_mode = in;
    t->win_out_mode = out;
    t->win_valid = 1;
}

int omc_win_tcgetattr(int fd, struct termios *t) {
    HANDLE in = con_in(fd), out = con_out();
    DWORD im = 0, om = 0;
    if (!in || !GetConsoleMode(in, &im)) {
        errno = ENOTTY;
        return -1;
    }
    if (out) GetConsoleMode(out, &om);
    flags_from_modes(t, im, om);
    return 0;
}

int omc_win_tcsetattr(int fd, int when, const struct termios *t) {
    HANDLE in = con_in(fd), out = con_out();
    DWORD cur;
    if (!in || !GetConsoleMode(in, &cur)) {
        errno = ENOTTY;
        return -1;
    }
    struct termios snap;
    DWORD im, om = 0;
    if (t->win_valid) {
        flags_from_modes(&snap, t->win_in_mode, t->win_out_mode);
    }
    if (t->win_valid && snap.c_lflag == t->c_lflag) {
        /* the snapshot itself: restore exactly */
        im = t->win_in_mode;
        om = t->win_out_mode;
    } else {
        im = cur & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT |
                            ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT);
        if (t->c_lflag & ICANON) im |= ENABLE_LINE_INPUT;
        if ((t->c_lflag & ECHO) && (t->c_lflag & ICANON)) im |= ENABLE_ECHO_INPUT; /* the console echoes only lines */
        if (t->c_lflag & ISIG) im |= ENABLE_PROCESSED_INPUT;
        if (!(t->c_lflag & ICANON)) im |= ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT;
        if (out) GetConsoleMode(out, &om);
    }
    if (when == TCSAFLUSH) FlushConsoleInputBuffer(in);
    if (!SetConsoleMode(in, im)) {
        /* consoles older than VT input: raw without it */
        im &= ~(DWORD)ENABLE_VIRTUAL_TERMINAL_INPUT;
        if (!SetConsoleMode(in, im)) return win_fail(GetLastError());
    }
    if (out && om) SetConsoleMode(out, om | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    return 0;
}

void omc_win_cfmakeraw(struct termios *t) {
    t->c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~(tcflag_t)OPOST;
    t->c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag = (t->c_cflag & ~(tcflag_t)(CSIZE | PARENB)) | CS8;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

int omc_win_tcflush(int fd, int queue) {
    if (queue == TCIFLUSH || queue == TCIOFLUSH) FlushConsoleInputBuffer(con_in(fd));
    return 0;
}

/* VT output on the process's console (stdout and stderr), so escape sequences render outside the TUI too */
void omc_win_console_vt(void) {
    HANDLE hs[2] = {GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)};
    for (int i = 0; i < 2; i++) {
        DWORD m;
        if (hs[i] && hs[i] != INVALID_HANDLE_VALUE && GetConsoleMode(hs[i], &m))
            SetConsoleMode(hs[i], m | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}
