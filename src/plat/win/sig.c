/* plat/win/sig.c - POSIX signals on Windows, as far as omc uses them.
 *  - SIGINT, SIGTERM, SIGHUP come from the console control handler (Ctrl+C / Ctrl+Break, logoff/shutdown, closing
 *    the window) and run the installed handler on the control thread, as a signal handler would run asynchronously.
 *  - SIGSEGV, SIGILL, SIGFPE, SIGBUS come from structured exceptions (the crash report's handler gets si_addr);
 *    SIGABRT from abort().
 *  - SIGWINCH: a watcher thread compares the console window size while a handler is installed.
 *  - raise() and kill(self) run the handler, or the default action (terminate with 128+sig, or ignore). */
#include "plat/win/win.h"

#undef signal
#undef raise
#undef _exit

typedef struct {
    void (*handler)(int);
    void (*action)(int, siginfo_t *, void *);
    int flags;
} sigent_t;

static sigent_t g_sig[NSIG];
static SRWLOCK g_sig_lock = SRWLOCK_INIT;
static volatile LONG g_resize_pending;

static bool default_ignores(int sig) { return sig == SIGCHLD || sig == SIGWINCH || sig == SIGCONT || sig == SIGPIPE; }

static void default_action(int sig) {
    if (default_ignores(sig)) return;
    /* the exit code a POSIX shell shows for a signal death */
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(), sig == SIGINT ? 0xC000013A : 128u + (UINT)sig);
}

static void deliver(int sig, siginfo_t *si) {
    if (sig <= 0 || sig >= NSIG) return;
    AcquireSRWLockShared(&g_sig_lock);
    sigent_t e = g_sig[sig];
    ReleaseSRWLockShared(&g_sig_lock);
    if (e.flags & SA_RESETHAND) {
        AcquireSRWLockExclusive(&g_sig_lock);
        memset(&g_sig[sig], 0, sizeof g_sig[sig]);
        ReleaseSRWLockExclusive(&g_sig_lock);
    }
    if ((e.flags & SA_SIGINFO) && e.action) {
        siginfo_t local = {.si_signo = sig};
        e.action(sig, si ? si : &local, NULL);
    } else if (e.handler == SIG_IGN) {
        return;
    } else if (e.handler && e.handler != SIG_DFL) {
        e.handler(sig);
    } else {
        default_action(sig);
    }
}

void win_sig_deliver(int sig) { deliver(sig, NULL); }

static bool has_handler(int sig) {
    AcquireSRWLockShared(&g_sig_lock);
    sigent_t e = g_sig[sig];
    ReleaseSRWLockShared(&g_sig_lock);
    return (e.flags & SA_SIGINFO) ? e.action != NULL : (e.handler != SIG_DFL && e.handler != NULL);
}

static bool is_ignored(int sig) {
    AcquireSRWLockShared(&g_sig_lock);
    bool ign = !(g_sig[sig].flags & SA_SIGINFO) && g_sig[sig].handler == SIG_IGN;
    ReleaseSRWLockShared(&g_sig_lock);
    return ign;
}

/* ---------- console control events ---------- */

static BOOL WINAPI ctrl_handler(DWORD type) {
    int sig;
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT: sig = SIGINT; break;
    case CTRL_CLOSE_EVENT: sig = SIGHUP; break;
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT: sig = SIGTERM; break;
    default: return FALSE;
    }
    if (is_ignored(sig)) return TRUE;
    if (!has_handler(sig)) return FALSE; /* the default: Windows ends the process */
    deliver(sig, NULL);
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        /* Windows ends the process when this returns: give the main thread time to restore and save */
        Sleep(3000);
    }
    return TRUE;
}

/* ---------- exceptions ---------- */

static LONG WINAPI exc_filter(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    int sig;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: sig = SIGSEGV; break;
    case EXCEPTION_DATATYPE_MISALIGNMENT: sig = SIGBUS; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION: sig = SIGILL; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW: sig = SIGFPE; break;
    default: return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!has_handler(sig)) return EXCEPTION_CONTINUE_SEARCH;
    siginfo_t si = {.si_signo = sig, .si_code = (int)code};
    si.si_addr = code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2
                     ? (void *)ep->ExceptionRecord->ExceptionInformation[1]
                     : ep->ExceptionRecord->ExceptionAddress;
    deliver(sig, &si);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void crt_abort_handler(int sig) {
    (void)sig;
    if (has_handler(SIGABRT)) deliver(SIGABRT, NULL);
    TerminateProcess(GetCurrentProcess(), 128u + SIGABRT);
}

/* ---------- SIGWINCH ---------- */

static DWORD WINAPI resize_watch(void *p) {
    (void)p;
    HANDLE out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (out == INVALID_HANDLE_VALUE) return 0;
    SHORT w = 0, h = 0;
    for (;;) {
        CONSOLE_SCREEN_BUFFER_INFO ci;
        if (GetConsoleScreenBufferInfo(out, &ci)) {
            SHORT nw = (SHORT)(ci.srWindow.Right - ci.srWindow.Left + 1), nh = (SHORT)(ci.srWindow.Bottom - ci.srWindow.Top + 1);
            if ((w || h) && (nw != w || nh != h)) InterlockedExchange(&g_resize_pending, 1);
            w = nw;
            h = nh;
        }
        if (InterlockedExchange(&g_resize_pending, 0) && has_handler(SIGWINCH)) deliver(SIGWINCH, NULL);
        Sleep(100);
    }
    return 0;
}

static void start_resize_watch(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    InitOnceBeginInitialize(&once, 0, &pending, NULL);
    if (!pending) return;
    if (GetConsoleWindow()) {
        HANDLE th = CreateThread(NULL, 64 * 1024, resize_watch, NULL, 0, NULL);
        if (th) CloseHandle(th);
    }
    InitOnceComplete(&once, 0, NULL);
}

void win_console_note_resize(void) { InterlockedExchange(&g_resize_pending, 1); }

bool win_console_take_resize(void) { return InterlockedExchange(&g_resize_pending, 0) != 0; }

/* ---------- the API ---------- */

/* the control handler only acts for signals omc installed handlers for, so a library host keeps its Ctrl+C; the
 * executable also takes crashes and abort() and keeps Windows' error dialogs away from omc and its children */
void omc_win_sig_init(bool exe) {
    static bool ctrl_done, exe_done;
    if (!ctrl_done) {
        SetConsoleCtrlHandler(ctrl_handler, TRUE);
        ctrl_done = true;
    }
    if (exe && !exe_done) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        SetUnhandledExceptionFilter(exc_filter);
        signal(SIGABRT, crt_abort_handler);
        exe_done = true;
    }
}

static void install(int sig, const struct sigaction *act) {
    AcquireSRWLockExclusive(&g_sig_lock);
    g_sig[sig].flags = act->sa_flags;
    if (act->sa_flags & SA_SIGINFO) {
        g_sig[sig].action = act->sa_sigaction;
        g_sig[sig].handler = NULL;
    } else {
        g_sig[sig].handler = act->sa_handler;
        g_sig[sig].action = NULL;
    }
    ReleaseSRWLockExclusive(&g_sig_lock);
    if (sig == SIGWINCH) start_resize_watch();
}

int omc_win_sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    if (sig <= 0 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP) {
        errno = EINVAL;
        return -1;
    }
    omc_win_init();
    if (old) {
        AcquireSRWLockShared(&g_sig_lock);
        memset(old, 0, sizeof *old);
        old->sa_flags = g_sig[sig].flags;
        if (g_sig[sig].flags & SA_SIGINFO) old->sa_sigaction = g_sig[sig].action;
        else old->sa_handler = g_sig[sig].handler ? g_sig[sig].handler : SIG_DFL;
        ReleaseSRWLockShared(&g_sig_lock);
    }
    if (act) install(sig, act);
    return 0;
}

omc_win_sighandler_t omc_win_signal(int sig, omc_win_sighandler_t h) {
    struct sigaction sa = {0}, old;
    sa.sa_handler = h;
    if (omc_win_sigaction(sig, &sa, &old) != 0) return SIG_ERR;
    return (old.sa_flags & SA_SIGINFO) ? SIG_DFL : old.sa_handler;
}

int omc_win_raise(int sig) {
    if (sig == 0) return 0;
    if (sig < 0 || sig >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    if (sig == SIGKILL) TerminateProcess(GetCurrentProcess(), 128u + SIGKILL);
    deliver(sig, NULL);
    return 0;
}

int omc_win_sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    static __thread sigset_t mask;
    if (old) *old = mask;
    if (set) {
        if (how == SIG_BLOCK) mask |= *set;
        else if (how == SIG_UNBLOCK) mask &= ~*set;
        else mask = *set;
    }
    return 0;
}

void omc_win__exit(int code) { _exit(code); }
