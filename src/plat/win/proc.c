/* plat/win/proc.c - child processes on Windows.
 *
 * win_spawn starts a program with CreateProcessW: argv quoted the way the MSVC runtime splits it, a UTF-16
 * environment block, exactly the three stdio handles inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST, so no other pipe
 * end leaks into a child and every EOF arrives), and the child in a job object of its own so kill() ends the whole
 * tree (a POSIX process group). Children are registered by pid for waitpid/waitid/kill. A pseudo console (ConPTY)
 * stands in for a pty. */
#include "plat/win/win.h"

#include <pty.h>
#include <spawn.h>
#include <sys/wait.h>

/* ---------- ConPTY (kernel32 since Windows 10 1809; looked up so older systems still start omc) ---------- */

typedef HRESULT(WINAPI *create_pc_t)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
typedef void(WINAPI *close_pc_t)(HPCON);
typedef HRESULT(WINAPI *resize_pc_t)(HPCON, COORD);
static create_pc_t p_create_pc;
static close_pc_t p_close_pc;
static resize_pc_t p_resize_pc;

static void pc_load(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    InitOnceBeginInitialize(&once, 0, &pending, NULL);
    if (!pending) return;
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    p_create_pc = (create_pc_t)(void *)GetProcAddress(k, "CreatePseudoConsole");
    p_close_pc = (close_pc_t)(void *)GetProcAddress(k, "ClosePseudoConsole");
    p_resize_pc = (resize_pc_t)(void *)GetProcAddress(k, "ResizePseudoConsole");
    InitOnceComplete(&once, 0, NULL);
}

/* ---------- the child table ---------- */

typedef struct {
    int pid;
    HANDLE proc, job;
    HPCON pcon;
    int killsig;
} child_ent_t;

static child_ent_t *g_kids;
static size_t g_nkids, g_capkids;
static SRWLOCK g_kids_lock = SRWLOCK_INIT;

static void kids_add(int pid, HANDLE proc, HANDLE job, HPCON pcon) {
    AcquireSRWLockExclusive(&g_kids_lock);
    if (g_nkids == g_capkids) {
        g_capkids = g_capkids ? g_capkids * 2 : 16;
        g_kids = realloc(g_kids, g_capkids * sizeof *g_kids);
    }
    g_kids[g_nkids++] = (child_ent_t){pid, proc, job, pcon, 0};
    ReleaseSRWLockExclusive(&g_kids_lock);
}

/* copy of the entry for pid (lock held by the caller) */
static child_ent_t *kids_find(int pid) {
    for (size_t i = 0; i < g_nkids; i++)
        if (g_kids[i].pid == pid) return &g_kids[i];
    return NULL;
}

static void kids_remove(int pid) {
    for (size_t i = 0; i < g_nkids; i++)
        if (g_kids[i].pid == pid) {
            g_kids[i] = g_kids[--g_nkids];
            return;
        }
}

/* ---------- command lines and the environment ---------- */

/* one argument as CommandLineToArgvW / the MSVC runtime read it back */
static void quote_arg(wchar_t **buf, size_t *len, size_t *cap, const wchar_t *a) {
    size_t need = wcslen(a) * 2 + 3;
    if (*len + need + 2 > *cap) {
        *cap = (*len + need + 2) * 2;
        *buf = realloc(*buf, *cap * sizeof(wchar_t));
    }
    wchar_t *o = *buf + *len;
    bool q = !*a || wcspbrk(a, L" \t\n\v\"");
    if (!q) {
        size_t n = wcslen(a);
        memcpy(o, a, n * sizeof(wchar_t));
        *len += n;
        return;
    }
    *o++ = L'"';
    for (const wchar_t *p = a;; p++) {
        size_t bs = 0;
        while (*p == L'\\') p++, bs++;
        if (!*p) {
            for (size_t k = 0; k < bs * 2; k++) *o++ = L'\\';
            break;
        }
        if (*p == L'"') {
            for (size_t k = 0; k < bs * 2 + 1; k++) *o++ = L'\\';
            *o++ = L'"';
        } else {
            for (size_t k = 0; k < bs; k++) *o++ = L'\\';
            *o++ = *p;
        }
    }
    *o++ = L'"';
    *len = (size_t)(o - *buf);
}

/* argv as one command line; argv0 (UTF-16) replaces argv[0] when given. verbatim: argv[1..] are joined unquoted */
static wchar_t *build_cmdline(const wchar_t *argv0, char *const argv[], bool verbatim) {
    size_t len = 0, cap = 256;
    wchar_t *b = malloc(cap * sizeof(wchar_t));
    for (int i = 0; argv[i]; i++) {
        wchar_t *w = i == 0 && argv0 ? _wcsdup(argv0) : win_wide(argv[i]);
        if (i) {
            if (len + 2 > cap) cap *= 2, b = realloc(b, cap * sizeof(wchar_t));
            b[len++] = L' ';
        }
        if (verbatim && i > 0) {
            size_t n = wcslen(w);
            if (len + n + 2 > cap) cap = (len + n + 2) * 2, b = realloc(b, cap * sizeof(wchar_t));
            memcpy(b + len, w, n * sizeof(wchar_t));
            len += n;
        } else {
            quote_arg(&b, &len, &cap, w);
        }
        free(w);
    }
    b[len] = 0;
    return b;
}

static int cmp_env(const void *a, const void *b) { return _wcsicmp(*(wchar_t *const *)a, *(wchar_t *const *)b); }

static wchar_t *build_env(char *const envp[]) {
    size_t n = 0;
    while (envp[n]) n++;
    wchar_t **v = calloc(n + 1, sizeof *v);
    size_t total = 1;
    for (size_t i = 0; i < n; i++) {
        v[i] = win_wide(envp[i]);
        total += wcslen(v[i]) + 1;
    }
    qsort(v, n, sizeof *v, cmp_env);
    wchar_t *blk = malloc(total * sizeof(wchar_t)), *o = blk;
    for (size_t i = 0; i < n; i++) {
        size_t l = wcslen(v[i]);
        memcpy(o, v[i], (l + 1) * sizeof(wchar_t));
        o += l + 1;
        free(v[i]);
    }
    *o = 0;
    if (!n) o[1] = 0;
    free(v);
    return blk;
}

/* ---------- finding programs ---------- */

static bool is_file(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static wchar_t *try_exts(const wchar_t *base) {
    if (is_file(base)) {
        const wchar_t *dot = wcsrchr(base, L'.'), *sl = wcsrchr(base, L'\\');
        if (dot && (!sl || dot > sl)) return _wcsdup(base);
    }
    const char *pe = getenv("PATHEXT");
    wchar_t *exts = win_wide(pe && *pe ? pe : ".COM;.EXE;.BAT;.CMD");
    size_t bl = wcslen(base);
    wchar_t *found = NULL;
    for (wchar_t *ctx = NULL, *e = wcstok_s(exts, L";", &ctx); e && !found; e = wcstok_s(NULL, L";", &ctx)) {
        size_t el = wcslen(e);
        wchar_t *c = malloc((bl + el + 1) * sizeof(wchar_t));
        memcpy(c, base, bl * sizeof(wchar_t));
        memcpy(c + bl, e, (el + 1) * sizeof(wchar_t));
        if (is_file(c)) found = c;
        else free(c);
    }
    free(exts);
    /* a file with no extension (a script) only when nothing executable matched */
    if (!found && is_file(base)) found = _wcsdup(base);
    return found;
}

static wchar_t *find_exe_w(const char *argv0) {
    if (!argv0 || !*argv0) return NULL;
    if (strchr(argv0, '/') || strchr(argv0, '\\') || (isalpha((unsigned char)argv0[0]) && argv0[1] == ':')) {
        wchar_t *w = win_path(argv0);
        wchar_t *r = try_exts(w);
        free(w);
        return r;
    }
    const char *path = getenv("PATH");
    if (!path) return NULL;
    wchar_t *wpath = win_wide(path), *name = win_wide(argv0), *found = NULL;
    for (wchar_t *ctx = NULL, *d = wcstok_s(wpath, L";", &ctx); d && !found; d = wcstok_s(NULL, L";", &ctx)) {
        if (!*d) continue;
        size_t dl = wcslen(d), nl = wcslen(name);
        if (d[0] == L'"' && dl > 1 && d[dl - 1] == L'"') d[dl - 1] = 0, d++, dl -= 2;
        wchar_t *c = malloc((dl + nl + 2) * sizeof(wchar_t));
        memcpy(c, d, dl * sizeof(wchar_t));
        size_t k = dl;
        if (k && c[k - 1] != L'\\' && c[k - 1] != L'/') c[k++] = L'\\';
        memcpy(c + k, name, (nl + 1) * sizeof(wchar_t));
        found = try_exts(c);
        free(c);
    }
    free(wpath);
    free(name);
    return found;
}

char *win_find_exe(const char *argv0) {
    wchar_t *w = find_exe_w(argv0);
    if (!w) return NULL;
    char *u = win_utf8(w);
    free(w);
    win_slashes(u);
    return u;
}

/* ---------- spawn ---------- */

static bool ends_with_ci(const wchar_t *s, const wchar_t *suf) {
    size_t a = wcslen(s), b = wcslen(suf);
    return a >= b && !_wcsicmp(s + a - b, suf);
}

static HANDLE inheritable(HANDLE h) {
    HANDLE d = NULL;
    if (!h || h == INVALID_HANDLE_VALUE) return NULL;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, TRUE, DUPLICATE_SAME_ACCESS)) return NULL;
    return d;
}

int win_spawn(char *const argv[], const win_spawn_t *o) {
    if (!argv || !argv[0]) {
        errno = EINVAL;
        return -1;
    }
    wchar_t *exe = find_exe_w(o->exe ? o->exe : argv[0]);
    if (!exe) {
        errno = ENOENT;
        return -1;
    }
    /* batch files run through cmd.exe, which CreateProcess would pick anyway but with its own quoting */
    wchar_t *cmdline;
    if (ends_with_ci(exe, L".bat") || ends_with_ci(exe, L".cmd")) {
        wchar_t *inner = build_cmdline(exe, argv, o->verbatim);
        size_t il = wcslen(inner);
        const wchar_t *pre = L"cmd.exe /d /s /c \"";
        cmdline = malloc((wcslen(pre) + il + 2) * sizeof(wchar_t));
        wcscpy(cmdline, pre);
        wcscat(cmdline, inner);
        wcscat(cmdline, L"\"");
        free(inner);
        wchar_t sys[MAX_PATH];
        UINT sl = GetSystemDirectoryW(sys, MAX_PATH);
        free(exe);
        exe = malloc((sl + 10) * sizeof(wchar_t));
        wcscpy(exe, sys);
        wcscat(exe, L"\\cmd.exe");
    } else {
        cmdline = build_cmdline(NULL, argv, o->verbatim);
    }
    wchar_t *env = o->envp ? build_env(o->envp) : NULL;
    wchar_t *cwd = o->cwd ? win_path(o->cwd) : NULL;

    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    HANDLE nul = NULL, hs[3] = {NULL, NULL, NULL};
    HANDLE list[3];
    DWORD nlist = 0;
    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(NULL, 2, 0, &asz);
    si.lpAttributeList = malloc(asz);
    InitializeProcThreadAttributeList(si.lpAttributeList, 2, 0, &asz);
    if (o->pcon) {
        UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, o->pcon, sizeof o->pcon, NULL, NULL);
    } else {
        HANDLE src[3] = {o->h_in, o->h_out, o->h_err};
        for (int i = 0; i < 3; i++) {
            HANDLE h = src[i];
            if (!h) {
                if (!nul) {
                    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
                    nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
                }
                hs[i] = nul;
            } else {
                hs[i] = inheritable(h);
            }
        }
        for (int i = 0; i < 3; i++) {
            bool dup = false;
            for (DWORD k = 0; k < nlist; k++)
                if (list[k] == hs[i]) dup = true;
            if (hs[i] && hs[i] != INVALID_HANDLE_VALUE && !dup) list[nlist++] = hs[i];
        }
        si.StartupInfo.hStdInput = hs[0];
        si.StartupInfo.hStdOutput = hs[1];
        si.StartupInfo.hStdError = hs[2];
        if (nlist)
            UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, nlist * sizeof(HANDLE), NULL, NULL);
    }
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED;
    if (o->new_group) flags |= CREATE_NEW_PROCESS_GROUP;
    if (o->detached) flags |= DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB;
    else if (o->new_console_hidden || !GetConsoleWindow()) flags |= CREATE_NO_WINDOW;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(exe, cmdline, NULL, NULL, !o->pcon && nlist > 0, flags, env, cwd, &si.StartupInfo, &pi);
    DWORD err = ok ? 0 : GetLastError();
    if (!ok && err == ERROR_ACCESS_DENIED && (flags & CREATE_BREAKAWAY_FROM_JOB)) {
        /* omc's own job forbids breakaway: the child stays in it */
        flags &= ~(DWORD)CREATE_BREAKAWAY_FROM_JOB;
        ok = CreateProcessW(exe, cmdline, NULL, NULL, nlist > 0, flags, env, cwd, &si.StartupInfo, &pi);
        err = ok ? 0 : GetLastError();
    }
    for (int i = 0; i < 3; i++)
        if (hs[i] && hs[i] != nul) CloseHandle(hs[i]);
    if (nul) CloseHandle(nul);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    free(exe);
    free(cmdline);
    free(env);
    free(cwd);
    if (!ok) {
        errno = err == ERROR_DIRECTORY ? ENOENT : win_errno_from(err);
        return -1;
    }
    if (o->detached) {
        /* never waited for: not tracked, its handles go now */
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return (int)pi.dwProcessId;
    }
    HANDLE job = NULL;
    {
        job = CreateJobObjectW(NULL, NULL);
        if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
            CloseHandle(job);
            job = NULL;
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    kids_add((int)pi.dwProcessId, pi.hProcess, job, o->pcon);
    return (int)pi.dwProcessId;
}

static HANDLE spawn_fd_handle(int fd, int which) {
    if (fd == OMC_WIN_INHERIT) return GetStdHandle(which);
    if (fd < 0) return NULL;
    return win_fd_get_handle(fd);
}

int omc_win_spawn(char *const argv[], const omc_win_spawn_opts_t *o) {
    win_spawn_t w = {.exe = o->exe, .cwd = o->cwd, .envp = o->envp, .detached = o->detached, .new_console_hidden = o->hidden,
                     .verbatim = o->verbatim};
    w.h_in = spawn_fd_handle(o->fd_in, STD_INPUT_HANDLE);
    w.h_out = spawn_fd_handle(o->fd_out, STD_OUTPUT_HANDLE);
    w.h_err = spawn_fd_handle(o->fd_err, STD_ERROR_HANDLE);
    return win_spawn(argv, &w);
}

char *omc_win_find_exe(const char *name) { return win_find_exe(name); }

/* ---------- wait / kill ---------- */

static int sig_of_status(DWORD code) {
    switch (code) {
    case 0xC0000005: /* access violation */
    case 0xC00000FD: /* stack overflow */
        return SIGSEGV;
    case 0xC000001D: return SIGILL;
    case 0xC0000094:
    case 0xC000008E: return SIGFPE;
    case 0xC0000409: /* fail-fast / abort() */
        return SIGABRT;
    case 0xC000013A: /* STATUS_CONTROL_C_EXIT */
        return SIGINT;
    default: return 0;
    }
}

static int make_status(const child_ent_t *c, DWORD code) {
    if (c->killsig) return c->killsig & 0x7f;
    int s = sig_of_status(code);
    if (s) return s;
    return (int)((code & 0xff) << 8);
}

static void reap(child_ent_t *c) {
    if (c->pcon) {
        pc_load();
        if (p_close_pc) p_close_pc(c->pcon);
    }
    CloseHandle(c->proc);
    if (c->job) CloseHandle(c->job);
    kids_remove(c->pid);
}

int omc_win_waitpid(int pid, int *status, int options) {
    for (;;) {
        AcquireSRWLockExclusive(&g_kids_lock);
        if (pid > 0) {
            child_ent_t *c = kids_find(pid);
            if (!c) {
                ReleaseSRWLockExclusive(&g_kids_lock);
                errno = ECHILD;
                return -1;
            }
            HANDLE h = c->proc;
            ReleaseSRWLockExclusive(&g_kids_lock);
            DWORD w = WaitForSingleObject(h, (options & WNOHANG) ? 0 : INFINITE);
            if (w == WAIT_TIMEOUT) return 0;
            AcquireSRWLockExclusive(&g_kids_lock);
            c = kids_find(pid);
            if (!c) {
                ReleaseSRWLockExclusive(&g_kids_lock);
                errno = ECHILD;
                return -1;
            }
            DWORD code = 0;
            GetExitCodeProcess(c->proc, &code);
            if (status) *status = make_status(c, code);
            reap(c);
            ReleaseSRWLockExclusive(&g_kids_lock);
            return pid;
        }
        /* any child */
        if (!g_nkids) {
            ReleaseSRWLockExclusive(&g_kids_lock);
            errno = ECHILD;
            return -1;
        }
        for (size_t i = 0; i < g_nkids; i++) {
            if (WaitForSingleObject(g_kids[i].proc, 0) == WAIT_OBJECT_0) {
                DWORD code = 0;
                GetExitCodeProcess(g_kids[i].proc, &code);
                int p = g_kids[i].pid;
                if (status) *status = make_status(&g_kids[i], code);
                reap(&g_kids[i]);
                ReleaseSRWLockExclusive(&g_kids_lock);
                return p;
            }
        }
        ReleaseSRWLockExclusive(&g_kids_lock);
        if (options & WNOHANG) return 0;
        Sleep(10);
    }
}

int omc_win_waitid(int idtype, id_t id, siginfo_t *si, int options) {
    memset(si, 0, sizeof *si);
    if (idtype != P_PID) {
        errno = EINVAL;
        return -1;
    }
    for (;;) {
        AcquireSRWLockExclusive(&g_kids_lock);
        child_ent_t *c = kids_find((int)id);
        if (!c) {
            ReleaseSRWLockExclusive(&g_kids_lock);
            errno = ECHILD;
            return -1;
        }
        if (WaitForSingleObject(c->proc, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(c->proc, &code);
            int st = make_status(c, code);
            si->si_pid = (int)id;
            si->si_signo = SIGCHLD;
            if (WIFSIGNALED(st)) {
                si->si_code = CLD_KILLED;
                si->si_status = WTERMSIG(st);
            } else {
                si->si_code = CLD_EXITED;
                si->si_status = WEXITSTATUS(st);
            }
            if (!(options & WNOWAIT)) reap(c);
            ReleaseSRWLockExclusive(&g_kids_lock);
            return 0;
        }
        ReleaseSRWLockExclusive(&g_kids_lock);
        if (options & WNOHANG) return 0;
        Sleep(10);
    }
}

int omc_win_kill(int pid, int sig) {
    if (pid == 0 || pid == (int)GetCurrentProcessId() || pid == -(int)GetCurrentProcessId()) return omc_win_raise(sig);
    int target = pid < 0 ? -pid : pid;
    AcquireSRWLockExclusive(&g_kids_lock);
    child_ent_t *c = kids_find(target);
    if (c) {
        bool alive = WaitForSingleObject(c->proc, 0) == WAIT_TIMEOUT;
        if (sig != 0 && alive) {
            c->killsig = sig;
            UINT code = 128u + (UINT)sig;
            if (!c->job || !TerminateJobObject(c->job, code)) TerminateProcess(c->proc, code);
        } else if (sig != 0 && c->job) {
            /* the leader is gone; its group may live on */
            TerminateJobObject(c->job, 128u + (UINT)sig);
        }
        ReleaseSRWLockExclusive(&g_kids_lock);
        return 0;
    }
    ReleaseSRWLockExclusive(&g_kids_lock);
    HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, (DWORD)target);
    if (!h) {
        DWORD e = GetLastError();
        errno = e == ERROR_INVALID_PARAMETER ? ESRCH : e == ERROR_ACCESS_DENIED ? EPERM : ESRCH;
        return -1;
    }
    int r = 0;
    if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
        errno = ESRCH;
        r = -1;
    } else if (sig != 0 && !TerminateProcess(h, 128u + (UINT)sig)) {
        errno = EPERM;
        r = -1;
    }
    CloseHandle(h);
    return r;
}

int omc_win_job_kill_on_close(int pid) {
    int r = -1;
    AcquireSRWLockExclusive(&g_kids_lock);
    child_ent_t *c = kids_find(pid);
    if (c && c->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        r = SetInformationJobObject(c->job, JobObjectExtendedLimitInformation, &li, sizeof li) ? 0 : -1;
    }
    ReleaseSRWLockExclusive(&g_kids_lock);
    return r;
}

/* ---------- pseudo consoles ---------- */

int omc_win_openpty(int *master, int *slave, char *name, const struct termios *t, const struct winsize *ws) {
    (void)master;
    (void)slave;
    (void)name;
    (void)t;
    (void)ws;
    errno = ENOSYS;
    return -1;
}

/* start argv on a new pseudo console of cols x rows: *in_fd writes its input, *out_fd reads its screen output (VT).
 * The pseudo console closes when the child is reaped, which ends *out_fd. */
int omc_win_pty_spawn(char *const argv[], const char *cwd, char *const envp[], int cols, int rows, int *in_fd, int *out_fd) {
    pc_load();
    if (!p_create_pc) {
        errno = ENOSYS;
        return -1;
    }
    HANDLE in_r, in_w, out_r, out_w;
    if (!CreatePipe(&in_r, &in_w, NULL, 0)) return win_fail(GetLastError());
    if (!CreatePipe(&out_r, &out_w, NULL, 1 << 16)) {
        DWORD e = GetLastError();
        CloseHandle(in_r);
        CloseHandle(in_w);
        return win_fail(e);
    }
    HPCON pc;
    COORD size = {(SHORT)(cols > 0 ? cols : 80), (SHORT)(rows > 0 ? rows : 24)};
    HRESULT hr = p_create_pc(size, in_r, out_w, 0, &pc);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (FAILED(hr)) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        errno = EIO;
        return -1;
    }
    win_spawn_t o = {.cwd = cwd, .envp = envp, .pcon = pc};
    int pid = win_spawn(argv, &o);
    if (pid < 0) {
        int e = errno;
        p_close_pc(pc);
        CloseHandle(in_w);
        CloseHandle(out_r);
        errno = e;
        return -1;
    }
    *in_fd = omc_win_fd_from_handle((intptr_t)in_w, 0);
    *out_fd = omc_win_fd_from_handle((intptr_t)out_r, 0);
    return pid;
}

int omc_win_pty_resize(int pid, int cols, int rows) {
    pc_load();
    int r = -1;
    AcquireSRWLockExclusive(&g_kids_lock);
    child_ent_t *c = kids_find(pid);
    if (c && c->pcon && p_resize_pc) r = SUCCEEDED(p_resize_pc(c->pcon, (COORD){(SHORT)cols, (SHORT)rows})) ? 0 : -1;
    ReleaseSRWLockExclusive(&g_kids_lock);
    return r;
}

/* the child's pseudo console goes away now (its output pipe then ends once drained) */
void omc_win_pty_close(int pid) {
    pc_load();
    AcquireSRWLockExclusive(&g_kids_lock);
    child_ent_t *c = kids_find(pid);
    if (c && c->pcon && p_close_pc) {
        p_close_pc(c->pcon);
        c->pcon = NULL;
    }
    ReleaseSRWLockExclusive(&g_kids_lock);
}

/* ---------- posix_spawn ---------- */

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa) {
    memset(fa, 0, sizeof *fa);
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa) {
    for (int i = 0; i < fa->n; i++) free(fa->a[i].path);
    fa->n = 0;
    return 0;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd, const char *path, int oflag, int mode) {
    if (fa->n >= 8) return ENOMEM;
    fa->a[fa->n++] = (typeof(fa->a[0])){0, fd, -1, oflag, mode, strdup(path)};
    return 0;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd) {
    if (fa->n >= 8) return ENOMEM;
    fa->a[fa->n++] = (typeof(fa->a[0])){1, newfd, fd, 0, 0, NULL};
    return 0;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd) {
    if (fa->n >= 8) return ENOMEM;
    fa->a[fa->n++] = (typeof(fa->a[0])){2, fd, -1, 0, 0, NULL};
    return 0;
}

int posix_spawnattr_init(posix_spawnattr_t *at) {
    at->flags = 0;
    return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t *at) {
    (void)at;
    return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t *at, short flags) {
    at->flags = flags;
    return 0;
}

static int do_spawn(int *pid, const char *file, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                    char *const argv[], char *const envp[]) {
    HANDLE std[3] = {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)};
    int opened[8], nopened = 0;
    for (int i = 0; fa && i < fa->n; i++) {
        int fd = fa->a[i].fd;
        if (fd < 0 || fd > 2) continue;
        if (fa->a[i].op == 0) {
            int f = omc_win_open(fa->a[i].path, fa->a[i].oflag, fa->a[i].mode);
            if (f < 0) {
                int e = errno;
                for (int k = 0; k < nopened; k++) omc_win_close(opened[k]);
                return e;
            }
            opened[nopened++] = f;
            std[fd] = win_fd_get_handle(f);
        } else if (fa->a[i].op == 1) {
            int src = fa->a[i].src;
            std[fd] = src >= 0 && src <= 2 ? std[src] : win_fd_get_handle(src);
        } else {
            std[fd] = NULL;
        }
    }
    win_spawn_t o = {.envp = envp, .h_in = std[0], .h_out = std[1], .h_err = std[2],
                     .detached = at && (at->flags & POSIX_SPAWN_SETSID)};
    int p = win_spawn(argv, &o);
    int e = errno;
    (void)file;
    for (int k = 0; k < nopened; k++) omc_win_close(opened[k]);
    if (p < 0) return e;
    if (pid) *pid = p;
    return 0;
}

int posix_spawn(int *pid, const char *path, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                char *const argv[], char *const envp[]) {
    /* argv[0] is only a name: the program is `path` */
    size_t n = 0;
    while (argv[n]) n++;
    char **v = calloc(n + 1, sizeof *v);
    for (size_t i = 0; i < n; i++) v[i] = argv[i];
    v[0] = (char *)path;
    int r = do_spawn(pid, path, fa, at, v, envp);
    free(v);
    return r;
}

int posix_spawnp(int *pid, const char *file, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                 char *const argv[], char *const envp[]) {
    return do_spawn(pid, file, fa, at, argv, envp);
}
