/* plat/win/util.c - UTF-8/UTF-16 conversion, error mapping, and the libc pieces the Windows CRT lacks (strings,
 * environment, identity, time, system information, start-up) */
#include "plat/win/win.h"

#include <bcrypt.h>
#include <dbghelp.h>
#include <lmcons.h>
#include <psapi.h>
#include <pwd.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <execinfo.h>

/* ---------- UTF-8 <-> UTF-16 ---------- */

wchar_t *win_wide_n(const char *s, size_t n) {
    if (!s) return NULL;
    int wn = n ? MultiByteToWideChar(CP_UTF8, 0, s, (int)n, NULL, 0) : 0;
    wchar_t *w = malloc(((size_t)wn + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    if (wn) MultiByteToWideChar(CP_UTF8, 0, s, (int)n, w, wn);
    w[wn] = 0;
    return w;
}

wchar_t *win_wide(const char *s) { return s ? win_wide_n(s, strlen(s)) : NULL; }

char *win_utf8_n(const wchar_t *w, size_t n) {
    if (!w) return NULL;
    int bn = n ? WideCharToMultiByte(CP_UTF8, 0, w, (int)n, NULL, 0, NULL, NULL) : 0;
    char *s = malloc((size_t)bn + 1);
    if (!s) return NULL;
    if (bn) WideCharToMultiByte(CP_UTF8, 0, w, (int)n, s, bn, NULL, NULL);
    s[bn] = 0;
    return s;
}

char *win_utf8(const wchar_t *w) { return w ? win_utf8_n(w, wcslen(w)) : NULL; }

/* %TEMP% without its trailing separator (UTF-16, static) */
static const wchar_t *temp_dir_w(void) {
    static wchar_t dir[MAX_PATH + 1];
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    InitOnceBeginInitialize(&once, 0, &pending, NULL);
    if (pending) {
        DWORD n = GetTempPathW(ARRAYSIZE(dir), dir);
        if (!n || n >= ARRAYSIZE(dir)) n = 0, dir[0] = 0;
        while (n > 3 && (dir[n - 1] == L'\\' || dir[n - 1] == L'/')) dir[--n] = 0;
        InitOnceComplete(&once, 0, NULL);
    }
    return dir;
}

wchar_t *win_path(const char *path) {
    if (!path) return NULL;
    if (!strcmp(path, "/dev/null")) return win_wide("NUL");
    if (!strcmp(path, "/dev/tty")) return win_wide("CONIN$");
    /* /tmp is %TEMP%, as Git for Windows mounts it: omc and the bash tool's shell agree on it */
    if (!strncmp(path, "/tmp", 4) && (path[4] == 0 || path[4] == '/' || path[4] == '\\') && temp_dir_w()[0]) {
        wchar_t *rest = win_wide(path + 4);
        const wchar_t *t = temp_dir_w();
        size_t tl = wcslen(t), rl = wcslen(rest);
        wchar_t *w = malloc((tl + rl + 1) * sizeof(wchar_t));
        if (w) {
            memcpy(w, t, tl * sizeof(wchar_t));
            memcpy(w + tl, rest, (rl + 1) * sizeof(wchar_t));
            for (wchar_t *p = w; *p; p++)
                if (*p == L'/') *p = L'\\';
        }
        free(rest);
        return w;
    }
    wchar_t *w = win_wide(path);
    if (w)
        for (wchar_t *p = w; *p; p++)
            if (*p == L'/') *p = L'\\';
    return w;
}

void win_slashes(char *s) {
    for (; s && *s; s++)
        if (*s == '\\') *s = '/';
}

/* ---------- errors ---------- */

int win_errno_from(DWORD err) {
    switch (err) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_DIRECTORY: return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_PRIVILEGE_NOT_HELD: return EACCES;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return EEXIST;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: return ENOMEM;
    case ERROR_INVALID_HANDLE: return EBADF;
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
    case ERROR_PIPE_NOT_CONNECTED: return EPIPE;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case ERROR_NOT_SAME_DEVICE: return EXDEV;
    case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
    case ERROR_BUFFER_OVERFLOW:
    case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED: return ENOSYS;
    case ERROR_BAD_EXE_FORMAT:
    case ERROR_EXE_MACHINE_TYPE_MISMATCH: return ENOEXEC;
    case ERROR_WRITE_PROTECT: return EROFS;
    case ERROR_CANT_RESOLVE_FILENAME: return ELOOP;
    case ERROR_NEGATIVE_SEEK: return EINVAL;
    case ERROR_IO_PENDING:
    case ERROR_NO_SYSTEM_RESOURCES: return EAGAIN;
    case ERROR_OPERATION_ABORTED: return EINTR;
    case ERROR_INVALID_PARAMETER:
    default: return EINVAL;
    }
}

int win_errno_from_wsa(int err) {
    switch (err) {
    case WSAEWOULDBLOCK: return EWOULDBLOCK;
    case WSAEINPROGRESS: return EINPROGRESS;
    case WSAEALREADY: return EALREADY;
    case WSAENOTSOCK: return ENOTSOCK;
    case WSAEDESTADDRREQ: return EDESTADDRREQ;
    case WSAEMSGSIZE: return EMSGSIZE;
    case WSAEPROTOTYPE: return EPROTOTYPE;
    case WSAENOPROTOOPT: return ENOPROTOOPT;
    case WSAEPROTONOSUPPORT: return EPROTONOSUPPORT;
    case WSAEOPNOTSUPP: return EOPNOTSUPP;
    case WSAEAFNOSUPPORT: return EAFNOSUPPORT;
    case WSAEADDRINUSE: return EADDRINUSE;
    case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
    case WSAENETDOWN: return ENETDOWN;
    case WSAENETUNREACH: return ENETUNREACH;
    case WSAENETRESET: return ENETRESET;
    case WSAECONNABORTED: return ECONNABORTED;
    case WSAECONNRESET: return ECONNRESET;
    case WSAENOBUFS: return ENOBUFS;
    case WSAEISCONN: return EISCONN;
    case WSAENOTCONN: return ENOTCONN;
    case WSAESHUTDOWN: return EPIPE;
    case WSAETIMEDOUT: return ETIMEDOUT;
    case WSAECONNREFUSED: return ECONNREFUSED;
    case WSAEHOSTUNREACH: return EHOSTUNREACH;
    case WSAEINTR: return EINTR;
    case WSAEBADF: return EBADF;
    case WSAEACCES: return EACCES;
    case WSAEFAULT: return EFAULT;
    case WSAEINVAL: return EINVAL;
    case WSAEMFILE: return EMFILE;
    default: return EIO;
    }
}

/* ---------- strings ---------- */

void *omc_win_memmem(const void *h, size_t hn, const void *n, size_t nn) {
    if (!nn) return (void *)h;
    if (nn > hn) return NULL;
    const unsigned char *p = h, *end = p + hn - nn, *q = n;
    for (; p <= end; p++) {
        p = memchr(p, q[0], (size_t)(end - p) + 1);
        if (!p) return NULL;
        if (!memcmp(p, q, nn)) return (void *)p;
    }
    return NULL;
}

void *omc_win_memrchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s + n;
    while (n--)
        if (*--p == (unsigned char)c) return (void *)p;
    return NULL;
}

char *omc_win_strcasestr(const char *h, const char *n) {
    size_t nn = strlen(n);
    if (!nn) return (char *)h;
    for (; *h; h++)
        if (!strncasecmp(h, n, nn)) return (char *)h;
    return NULL;
}

char *omc_win_strsep(char **s, const char *delim) {
    char *start = *s;
    if (!start) return NULL;
    char *p = start + strcspn(start, delim);
    if (*p) {
        *p = 0;
        *s = p + 1;
    } else {
        *s = NULL;
    }
    return start;
}

char *omc_win_strchrnul(const char *s, int c) {
    while (*s && *s != (char)c) s++;
    return (char *)s;
}

char *omc_win_strndup(const char *s, size_t n) {
    size_t l = strnlen(s, n);
    char *r = malloc(l + 1);
    if (!r) return NULL;
    memcpy(r, s, l);
    r[l] = 0;
    return r;
}

/* glibc's descriptions, which omc's output (and bash's job notices) use */
char *omc_win_strsignal(int sig) {
    switch (sig) {
    case SIGHUP: return "Hangup";
    case SIGINT: return "Interrupt";
    case SIGQUIT: return "Quit";
    case SIGILL: return "Illegal instruction";
    case SIGTRAP: return "Trace/breakpoint trap";
    case SIGABRT: return "Aborted";
    case SIGBUS: return "Bus error";
    case SIGFPE: return "Floating point exception";
    case SIGKILL: return "Killed";
    case SIGUSR1: return "User defined signal 1";
    case SIGSEGV: return "Segmentation fault";
    case SIGUSR2: return "User defined signal 2";
    case SIGPIPE: return "Broken pipe";
    case SIGALRM: return "Alarm clock";
    case SIGTERM: return "Terminated";
    case SIGCHLD: return "Child exited";
    case SIGCONT: return "Continued";
    case SIGSTOP: return "Stopped (signal)";
    case SIGTSTP: return "Stopped";
    case SIGTTIN: return "Stopped (tty input)";
    case SIGTTOU: return "Stopped (tty output)";
    case SIGWINCH: return "Window changed";
    case SIGSYS: return "Bad system call";
    default: return "Unknown signal";
    }
}

ssize_t omc_win_getdelim(char **line, size_t *cap, int delim, FILE *f) {
    if (!line || !cap || !f) {
        errno = EINVAL;
        return -1;
    }
    size_t n = 0;
    if (!*line || !*cap) {
        *cap = 128;
        *line = malloc(*cap);
        if (!*line) return -1;
    }
    int c;
    while ((c = getc(f)) != EOF) {
        if (n + 2 > *cap) {
            size_t nc = *cap * 2;
            char *nl = realloc(*line, nc);
            if (!nl) return -1;
            *line = nl;
            *cap = nc;
        }
        (*line)[n++] = (char)c;
        if (c == delim) break;
    }
    if (!n) return -1;
    (*line)[n] = 0;
    return (ssize_t)n;
}

ssize_t omc_win_getline(char **line, size_t *cap, FILE *f) { return omc_win_getdelim(line, cap, '\n', f); }

int omc_win_vasprintf(char **out, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = __mingw_vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) return -1;
    *out = malloc((size_t)n + 1);
    if (!*out) return -1;
    return __mingw_vsnprintf(*out, (size_t)n + 1, fmt, ap);
}

int omc_win_asprintf(char **out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = omc_win_vasprintf(out, fmt, ap);
    va_end(ap);
    return n;
}

/* ---------- time ---------- */

static int match_name(const char **s, const char *const *names, int count) {
    for (int i = 0; i < count; i++) {
        size_t l = strlen(names[i]);
        if (!strncasecmp(*s, names[i], l)) {
            *s += l;
            return i;
        }
        if (!strncasecmp(*s, names[i], 3)) {
            *s += 3;
            return i;
        }
    }
    return -1;
}

static bool num(const char **s, int min_digits, int max_digits, int *out) {
    int v = 0, d = 0;
    while (d < max_digits && isdigit((unsigned char)**s)) v = v * 10 + (*(*s)++ - '0'), d++;
    if (d < min_digits) return false;
    *out = v;
    return true;
}

/* the conversions omc's callers use: %Y %m %d %H %M %S %y %b %B %a %A %e %j %T %D %z %Z %n %t %% */
char *omc_win_strptime(const char *s, const char *fmt, struct tm *tm) {
    static const char *const MON[] = {"January", "February", "March", "April", "May", "June", "July",
                                      "August", "September", "October", "November", "December"};
    static const char *const DAY[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    for (; *fmt; fmt++) {
        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*s)) s++;
            continue;
        }
        if (*fmt != '%') {
            if (*s++ != *fmt) return NULL;
            continue;
        }
        int v;
        switch (*++fmt) {
        case 'Y':
            if (!num(&s, 1, 4, &v)) return NULL;
            tm->tm_year = v - 1900;
            break;
        case 'y':
            if (!num(&s, 1, 2, &v)) return NULL;
            tm->tm_year = v < 69 ? v + 100 : v;
            break;
        case 'm':
            if (!num(&s, 1, 2, &v)) return NULL;
            tm->tm_mon = v - 1;
            break;
        case 'd':
        case 'e':
            while (*s == ' ') s++;
            if (!num(&s, 1, 2, &v)) return NULL;
            tm->tm_mday = v;
            break;
        case 'H':
            if (!num(&s, 1, 2, &v)) return NULL;
            tm->tm_hour = v;
            break;
        case 'M':
            if (!num(&s, 1, 2, &v)) return NULL;
            tm->tm_min = v;
            break;
        case 'S':
            if (!num(&s, 1, 2, &v)) return NULL;
            tm->tm_sec = v;
            break;
        case 'j':
            if (!num(&s, 1, 3, &v)) return NULL;
            tm->tm_yday = v - 1;
            break;
        case 'b':
        case 'B':
        case 'h':
            if ((v = match_name(&s, MON, 12)) < 0) return NULL;
            tm->tm_mon = v;
            break;
        case 'a':
        case 'A':
            if ((v = match_name(&s, DAY, 7)) < 0) return NULL;
            tm->tm_wday = v;
            break;
        case 'T':
            if (!(s = omc_win_strptime(s, "%H:%M:%S", tm))) return NULL;
            break;
        case 'D':
            if (!(s = omc_win_strptime(s, "%m/%d/%y", tm))) return NULL;
            break;
        case 'z':
            if (*s == 'Z') {
                s++;
                break;
            }
            if (*s != '+' && *s != '-') return NULL;
            s++;
            if (!num(&s, 2, 2, &v)) return NULL;
            if (*s == ':') s++;
            if (!num(&s, 2, 2, &v)) return NULL;
            break;
        case 'Z':
            while (isalpha((unsigned char)*s)) s++;
            break;
        case 'n':
        case 't':
            while (isspace((unsigned char)*s)) s++;
            break;
        case '%':
            if (*s++ != '%') return NULL;
            break;
        default: return NULL;
        }
    }
    return (char *)s;
}

unsigned omc_win_sleep(unsigned s) {
    Sleep(s * 1000u);
    return 0;
}

int omc_win_usleep(unsigned long long us) {
    Sleep((DWORD)((us + 999) / 1000));
    return 0;
}

int omc_win_pause(void) {
    for (;;) Sleep(INFINITE);
    return -1;
}

/* ---------- environment ---------- */

char ***omc_win_environ_ptr(void) { return __p__environ(); }

int omc_win_setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite && getenv(name)) return 0;
    /* _putenv_s with "" would unset it: POSIX keeps an empty value; the CRT can only hold it via SetEnvironmentVariable */
    if (!*value) {
        size_t n = strlen(name);
        char *kv = malloc(n + 2);
        if (!kv) return -1;
        memcpy(kv, name, n);
        kv[n] = '=';
        kv[n + 1] = 0;
        _putenv(kv);
        free(kv);
        wchar_t *wn = win_wide(name);
        SetEnvironmentVariableW(wn, L"");
        free(wn);
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}

int omc_win_unsetenv(const char *name) {
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    _putenv_s(name, "");
    return 0;
}

/* ---------- identity and system ---------- */

int omc_win_getppid(void) {
    /* the parent's pid as Windows recorded it at creation (it may have exited since) */
    typedef LONG(WINAPI * nqip_t)(HANDLE, int, PVOID, ULONG, PULONG);
    static nqip_t nqip;
    if (!nqip) nqip = (nqip_t)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    if (!nqip) return 0;
    struct {
        PVOID r1;
        PVOID peb;
        PVOID r2[2];
        ULONG_PTR pid;
        ULONG_PTR ppid;
    } pbi;
    if (nqip(GetCurrentProcess(), 0, &pbi, sizeof pbi, NULL) != 0) return 0;
    return (int)pbi.ppid;
}

int omc_win_gethostname(char *name, int len) {
    wchar_t w[256];
    DWORD n = ARRAYSIZE(w);
    if (!GetComputerNameExW(ComputerNameDnsHostname, w, &n)) return win_fail(GetLastError());
    char *u = win_utf8(w);
    if (!u) return -1;
    if ((int)strlen(u) >= len) {
        free(u);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(name, u);
    free(u);
    return 0;
}

int omc_win_uname(struct utsname *u) {
    memset(u, 0, sizeof *u);
    strcpy(u->sysname, "Windows_NT");
    char host[256];
    if (omc_win_gethostname(host, sizeof host) == 0) snprintf(u->nodename, sizeof u->nodename, "%s", host);
    /* RtlGetVersion: GetVersionEx lies to unmanifested callers */
    typedef LONG(WINAPI * rgv_t)(OSVERSIONINFOW *);
    rgv_t rgv = (rgv_t)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW vi = {.dwOSVersionInfoSize = sizeof vi};
    if (rgv && rgv(&vi) == 0)
        snprintf(u->release, sizeof u->release, "%lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    else
        strcpy(u->release, "10.0.0");
    snprintf(u->version, sizeof u->version, "Windows %s", u->release);
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    strcpy(u->machine, si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "aarch64" : "x86_64");
    return 0;
}

struct passwd *omc_win_getpwuid(unsigned uid) {
    (void)uid;
    static struct passwd pw;
    static char name[UNLEN * 3 + 1], dir[MAX_PATH * 3 + 1];
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    InitOnceBeginInitialize(&once, 0, &pending, NULL);
    if (pending) {
        wchar_t w[UNLEN + 1];
        DWORD n = ARRAYSIZE(w);
        if (GetUserNameW(w, &n)) {
            char *u = win_utf8(w);
            if (u) snprintf(name, sizeof name, "%s", u);
            free(u);
        }
        const wchar_t *home = _wgetenv(L"USERPROFILE");
        if (home) {
            char *u = win_utf8(home);
            if (u) {
                win_slashes(u);
                snprintf(dir, sizeof dir, "%s", u);
            }
            free(u);
        }
        pw.pw_name = name;
        pw.pw_passwd = "";
        pw.pw_gecos = name;
        pw.pw_dir = dir;
        pw.pw_shell = "";
        InitOnceComplete(&once, 0, NULL);
    }
    return &pw;
}

long omc_win_sysconf(int name) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    switch (name) {
    case _SC_NPROCESSORS_ONLN:
    case _SC_NPROCESSORS_CONF: {
        DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        return n ? (long)n : (long)si.dwNumberOfProcessors;
    }
    case _SC_PAGESIZE: return (long)si.dwPageSize;
    case _SC_CLK_TCK: return 100;
    case _SC_OPEN_MAX: return 8192;
    case _SC_PHYS_PAGES: {
        MEMORYSTATUSEX ms = {.dwLength = sizeof ms};
        if (!GlobalMemoryStatusEx(&ms)) return -1;
        return (long)(ms.ullTotalPhys / si.dwPageSize);
    }
    default: errno = EINVAL; return -1;
    }
}

ssize_t omc_win_getrandom(void *buf, size_t len, unsigned flags) {
    (void)flags;
    unsigned char *p = buf;
    size_t left = len;
    while (left) {
        ULONG n = left > 0x10000000 ? 0x10000000 : (ULONG)left;
        if (BCryptGenRandom(NULL, p, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            errno = EIO;
            return -1;
        }
        p += n;
        left -= n;
    }
    return (ssize_t)len;
}

int omc_win_getrusage(int who, struct rusage *ru) {
    memset(ru, 0, sizeof *ru);
    if (who != RUSAGE_SELF) return 0;
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        unsigned long long uk = ((unsigned long long)k.dwHighDateTime << 32 | k.dwLowDateTime) / 10;
        unsigned long long uu = ((unsigned long long)u.dwHighDateTime << 32 | u.dwLowDateTime) / 10;
        ru->ru_stime.tv_sec = (long)(uk / 1000000);
        ru->ru_stime.tv_usec = (long)(uk % 1000000);
        ru->ru_utime.tv_sec = (long)(uu / 1000000);
        ru->ru_utime.tv_usec = (long)(uu % 1000000);
    }
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) ru->ru_maxrss = (long)(pmc.PeakWorkingSetSize / 1024);
    return 0;
}

/* ---------- backtraces ---------- */

int omc_win_backtrace(void **buf, int n) { return (int)CaptureStackBackTrace(1, (DWORD)(n > 62 ? 62 : n), buf, NULL); }

void omc_win_backtrace_symbols_fd(void *const *buf, int n, int fd) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    for (int i = 0; i < n; i++) {
        HMODULE mod = NULL;
        char line[600], name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)buf[i], &mod) &&
            mod) {
            GetModuleFileNameA(mod, name, sizeof name);
            const char *b = strrchr(name, '\\');
            int len = snprintf(line, sizeof line, "%s(+0x%llx) [%p]\n", b ? b + 1 : name,
                               (unsigned long long)((uintptr_t)buf[i] - (uintptr_t)mod), buf[i]);
            DWORD w;
            WriteFile(h, line, (DWORD)len, &w, NULL);
        } else {
            int len = snprintf(line, sizeof line, "[%p]\n", buf[i]);
            DWORD w;
            WriteFile(h, line, (DWORD)len, &w, NULL);
        }
    }
}

/* ---------- start-up ---------- */

void omc_win_sig_init(bool exe);
void omc_win_fd_init(void);

/* what any process running omc needs, the library's host included: Winsock, the temp dir, the Ctrl+C hook */
void omc_win_init(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    InitOnceBeginInitialize(&once, 0, &pending, NULL);
    if (!pending) return;
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    omc_win_fd_init();
    /* os.tmpdir(): %TEMP%, without its trailing separator; omc's temp files go through TMPDIR, with '/' separators
     * like every path omc builds (a TMPDIR handed over with backslashes is rewritten) */
    const char *t = getenv("TMPDIR");
    if (t && *t && strchr(t, '\\')) {
        char *c = strdup(t);
        win_slashes(c);
        size_t cl = strlen(c);
        while (cl > 3 && c[cl - 1] == '/') c[--cl] = 0;
        _putenv_s("TMPDIR", c);
        free(c);
    } else if (!t || !*t) {
        wchar_t w[MAX_PATH + 1];
        DWORD n = GetTempPathW(ARRAYSIZE(w), w);
        if (n && n < ARRAYSIZE(w)) {
            while (n > 3 && (w[n - 1] == L'\\' || w[n - 1] == L'/')) w[--n] = 0;
            char *u = win_utf8(w);
            if (u) {
                win_slashes(u);
                _putenv_s("TMPDIR", u);
                free(u);
            }
        }
    }
    /* 1 ms timer resolution: the poll loops sleep in short slices */
    typedef UINT(WINAPI * tbp_t)(UINT);
    HMODULE wm = LoadLibraryW(L"winmm.dll");
    tbp_t tbp = wm ? (tbp_t)(void *)GetProcAddress(wm, "timeBeginPeriod") : NULL;
    if (tbp) tbp(1);
    omc_win_sig_init(false);
    InitOnceComplete(&once, 0, NULL);
}

void omc_win_console_vt(void);

/* the omc executable owns its process: console code pages, byte-exact stdio, VT output, crash reports */
void omc_win_init_exe(void) {
    omc_win_init();
    /* UTF-8 on the console both ways; the bytes omc writes are UTF-8 */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    /* stdio carries bytes, never CRLF-translated text */
    _setmode(0, _O_BINARY);
    _setmode(1, _O_BINARY);
    _setmode(2, _O_BINARY);
    omc_win_console_vt();
    omc_win_sig_init(true);
}
