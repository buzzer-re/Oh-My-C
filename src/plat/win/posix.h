/* plat/win/posix.h - the POSIX surface omc uses, on Windows (MinGW-w64). Force-included into every translation unit
 * of the Windows build (-include plat/win/posix.h), before anything else; never seen by the Linux/macOS builds.
 *
 * The CRT headers are included first, then the names omc calls are mapped onto omc_win_* implementations in
 * the src/plat/win sources with object-like macros (gnulib style), so call sites stay as they are on POSIX:
 *  - file descriptors: CRT fds for files, pipes and the console; sockets get a CRT fd of their own (fd.c keeps the
 *    SOCKET behind it), so read/write/close/poll/fcntl work on both. O_NONBLOCK is emulated for pipes and sockets.
 *  - paths are UTF-8 with '/' or '\' separators; the wrappers convert to UTF-16 and call the wide Win32 APIs.
 *  - files are always opened in binary mode (no CRLF translation).
 * Missing headers (poll.h, sys/socket.h, termios.h, zlib.h, the openssl headers, ...) live in src/plat/win/include. */
#ifndef OMC_PLAT_WIN_POSIX_H
#define OMC_PLAT_WIN_POSIX_H

#ifdef _WIN32

#ifndef _POSIX_THREAD_SAFE_FUNCTIONS
#define _POSIX_THREAD_SAFE_FUNCTIONS 200112L /* <time.h>: localtime_r, gmtime_r */
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <sched.h>
#include <pthread.h>
#include <wchar.h>
#include <locale.h>
#include <ctype.h>
#include <math.h>
#include <malloc.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- stdio: the C99 formatters (%zu, %lld, %hhd...), which gcc then checks as gnu_printf ---------- */
#undef printf
#undef fprintf
#undef sprintf
#undef snprintf
#undef vprintf
#undef vfprintf
#undef vsprintf
#undef vsnprintf
#define printf __mingw_printf
#define fprintf __mingw_fprintf
#define sprintf __mingw_sprintf
#define snprintf __mingw_snprintf
#define vprintf __mingw_vprintf
#define vfprintf __mingw_vfprintf
#define vsprintf __mingw_vsprintf
#define vsnprintf __mingw_vsnprintf

/* ---------- types ---------- */
#define pid_t int
#define ino_t unsigned long long
#define sigset_t omc_win_sigset_t
typedef unsigned long long omc_win_sigset_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int id_t;
/* <io.h> declares eof(int); omc has its own helpers by that name */
#define eof omc_win_eof_
typedef unsigned long nfds_t;
typedef long long blkcnt_t;
typedef long blksize_t;
typedef unsigned int nlink_t;

/* ---------- errno values the CRT lacks ---------- */
#ifndef EDQUOT
#define EDQUOT 200
#endif
#ifndef ENOTSUP
#define ENOTSUP 129
#endif
#ifndef ELOOP
#define ELOOP 114
#endif
#ifndef ESTALE
#define ESTALE 201
#endif

/* ---------- open flags ---------- */
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif
#define O_NONBLOCK 0x01000000 /* stripped before the CRT sees it; set on pipes/sockets via fcntl */
#define O_NOFOLLOW 0x02000000
#define O_DIRECTORY 0x04000000
#define O_NOCTTY 0
#define O_SYNC 0
#define O_DSYNC 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD 0
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1
/* fcntl record locks (whole-file only, over LockFileEx) and flock(): `struct flock` and flock() share the name */
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
#define flock omc_win_flock
struct omc_win_flock {
    short l_type, l_whence;
    long long l_start, l_len;
    int l_pid;
};
int omc_win_flock(int fd, int op);
#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define UTIME_NOW ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)

/* ---------- stat (64-bit sizes, timespec times, symlinks) ---------- */
#undef stat
#undef fstat
#undef lstat
#define stat omc_win_stat
#define fstat omc_win_fstat
#define lstat omc_win_lstat
struct omc_win_stat {
    unsigned int st_dev;
    unsigned long long st_ino;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned int st_uid, st_gid;
    unsigned int st_rdev;
    long long st_size;
    struct timespec st_atim, st_mtim, st_ctim;
    long st_blksize;
    long long st_blocks;
};
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec
int omc_win_stat(const char *path, struct omc_win_stat *st);
int omc_win_lstat(const char *path, struct omc_win_stat *st);
int omc_win_fstat(int fd, struct omc_win_stat *st);
#define fstatat omc_win_fstatat
int omc_win_fstatat(int dirfd, const char *path, struct omc_win_stat *st, int flags);

#ifndef S_IFLNK
#define S_IFLNK 0xA000
#endif
#ifndef S_IFSOCK
#define S_IFSOCK 0xC000
#endif
#ifndef S_IFBLK
#define S_IFBLK 0x6000
#endif
#ifndef S_IFIFO
#define S_IFIFO _S_IFIFO
#endif
#undef S_ISLNK
#undef S_ISSOCK
#undef S_ISBLK
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#ifndef S_ISFIFO
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#endif
#ifndef S_ISCHR
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#endif
#ifndef S_ISUID
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#endif

/* ---------- directories (d_type, UTF-8 names) ---------- */
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DIR omc_win_DIR
#define dirent omc_win_dirent
#define opendir omc_win_opendir
#define fdopendir omc_win_fdopendir
#define readdir omc_win_readdir
#define closedir omc_win_closedir
#define rewinddir omc_win_rewinddir
#define dirfd omc_win_dirfd
struct omc_win_dirent {
    unsigned long long d_ino;
    unsigned char d_type;
    char d_name[1024];
};
typedef struct omc_win_DIR omc_win_DIR;
omc_win_DIR *omc_win_opendir(const char *path);
omc_win_DIR *omc_win_fdopendir(int fd);
struct omc_win_dirent *omc_win_readdir(omc_win_DIR *d);
int omc_win_closedir(omc_win_DIR *d);
void omc_win_rewinddir(omc_win_DIR *d);
int omc_win_dirfd(omc_win_DIR *d);

/* ---------- file descriptors ---------- */
#define open omc_win_open
#define openat omc_win_openat
#define creat(p, m) omc_win_open((p), O_WRONLY | O_CREAT | O_TRUNC, (m))
#define read omc_win_read
#define write omc_win_write
#define close omc_win_close
#define pread omc_win_pread
#define pwrite omc_win_pwrite
#define lseek omc_win_lseek
#define dup omc_win_dup
#define dup2 omc_win_dup2
#define pipe omc_win_pipe
#define pipe2 omc_win_pipe2
#define fcntl omc_win_fcntl
#define isatty omc_win_isatty
#define fsync omc_win_fsync
#define fdatasync omc_win_fsync
#define ftruncate omc_win_ftruncate
#define truncate omc_win_truncate
int omc_win_open(const char *path, int flags, ...);
int omc_win_openat(int dirfd, const char *path, int flags, ...);
ssize_t omc_win_read(int fd, void *buf, size_t n);
ssize_t omc_win_write(int fd, const void *buf, size_t n);
int omc_win_close(int fd);
ssize_t omc_win_pread(int fd, void *buf, size_t n, long long off);
ssize_t omc_win_pwrite(int fd, const void *buf, size_t n, long long off);
long long omc_win_lseek(int fd, long long off, int whence);
int omc_win_dup(int fd);
int omc_win_dup2(int fd, int fd2);
int omc_win_pipe(int fds[2]);
int omc_win_pipe2(int fds[2], int flags);
int omc_win_fcntl(int fd, int cmd, ...);
int omc_win_isatty(int fd);
int omc_win_fsync(int fd);
int omc_win_ftruncate(int fd, long long len);
int omc_win_truncate(const char *path, long long len);
/* the Win32 HANDLE behind an fd (a SOCKET for a socket fd); INVALID_HANDLE_VALUE as intptr_t -1 if none */
intptr_t omc_win_fd_handle(int fd);
/* an fd for a handle the caller owns from now on (closing the fd closes it) */
int omc_win_fd_from_handle(intptr_t h, int flags);
bool omc_win_fd_is_socket(int fd);

/* ---------- stdio ---------- */
#define fopen omc_win_fopen
#define freopen omc_win_freopen
#define fdopen omc_win_fdopen
#define fileno omc_win_fileno
FILE *omc_win_fopen(const char *path, const char *mode);
FILE *omc_win_freopen(const char *path, const char *mode, FILE *f);
FILE *omc_win_fdopen(int fd, const char *mode);
int omc_win_fileno(FILE *f);
#define getline omc_win_getline
#define getdelim omc_win_getdelim
ssize_t omc_win_getline(char **line, size_t *cap, FILE *f);
ssize_t omc_win_getdelim(char **line, size_t *cap, int delim, FILE *f);
#define vasprintf omc_win_vasprintf
#define asprintf omc_win_asprintf
int omc_win_vasprintf(char **out, const char *fmt, va_list ap);
int omc_win_asprintf(char **out, const char *fmt, ...) __attribute__((format(gnu_printf, 2, 3)));

/* ---------- paths and the file system ---------- */
#undef mkdir
#define mkdir(p, m) omc_win_mkdir((p), (m))
#define rmdir omc_win_rmdir
#define unlink omc_win_unlink
#define unlinkat omc_win_unlinkat
#define remove omc_win_remove
#define rename omc_win_rename
#define renameat(a, b, c, d) omc_win_rename((b), (d))
#define access omc_win_access
#define faccessat(d, p, m, f) omc_win_access((p), (m))
#define chdir omc_win_chdir
#define getcwd omc_win_getcwd
#define realpath omc_win_realpath
#define readlink omc_win_readlink
#define symlink omc_win_symlink
#define link omc_win_link
#define chmod omc_win_chmod
#define fchmod omc_win_fchmod
#define chown(p, u, g) 0
#define fchown(f, u, g) 0
#define lchown(p, u, g) 0
#define utimensat omc_win_utimensat
#define futimens omc_win_futimens
#define mkdtemp omc_win_mkdtemp
#define mkstemp omc_win_mkstemp
#define mkstemps omc_win_mkstemps
int omc_win_mkstemps(char *tmpl, int suffixlen);
#define mkfifo omc_win_mkfifo
#define umask omc_win_umask
int omc_win_mkdir(const char *path, int mode);
int omc_win_rmdir(const char *path);
int omc_win_unlink(const char *path);
int omc_win_unlinkat(int dirfd, const char *path, int flags);
int omc_win_remove(const char *path);
int omc_win_rename(const char *from, const char *to);
int omc_win_access(const char *path, int mode);
int omc_win_chdir(const char *path);
char *omc_win_getcwd(char *buf, size_t size);
char *omc_win_realpath(const char *path, char *resolved);
ssize_t omc_win_readlink(const char *path, char *buf, size_t size);
int omc_win_symlink(const char *target, const char *linkpath);
int omc_win_link(const char *from, const char *to);
int omc_win_chmod(const char *path, int mode);
int omc_win_fchmod(int fd, int mode);
int omc_win_utimensat(int dirfd, const char *path, const struct timespec ts[2], int flags);
int omc_win_futimens(int fd, const struct timespec ts[2]);
char *omc_win_mkdtemp(char *tmpl);
int omc_win_mkstemp(char *tmpl);
int omc_win_mkfifo(const char *path, int mode);
int omc_win_umask(int mask);
#ifndef X_OK
#define X_OK 1
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#undef MAX_PATH_UTF8
#define MAX_PATH_UTF8 4096

/* ---------- environment, identity, time ---------- */
#define setenv omc_win_setenv
#define unsetenv omc_win_unsetenv
int omc_win_setenv(const char *name, const char *value, int overwrite);
int omc_win_unsetenv(const char *name);
/* mingw-w64 defines environ as a macro over the CRT's dllimport __p__environ(), so a file's own `extern char **environ;`
 * would redeclare that import; route it through a plain function instead (that declaration then declares it again) */
#undef environ
#define environ (*omc_win_environ_ptr())
char ***omc_win_environ_ptr(void);
#define getuid() 1000u
#define geteuid() 1000u
#define getgid() 1000u
#define getegid() 1000u
#define getppid omc_win_getppid
int omc_win_getppid(void);
#define gethostname omc_win_gethostname
int omc_win_gethostname(char *name, int len); /* Winsock's signature: <winsock2.h> declares it under this name too */
#define timegm _mkgmtime
#define strptime omc_win_strptime
char *omc_win_strptime(const char *s, const char *fmt, struct tm *tm);
#define sysconf omc_win_sysconf
#define _SC_NPROCESSORS_ONLN 84
#define _SC_NPROCESSORS_CONF 83
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_CLK_TCK 2
#define _SC_OPEN_MAX 4
#define _SC_PHYS_PAGES 85
long omc_win_sysconf(int name);
#define getpagesize() 4096
static inline unsigned alarm(unsigned s) { (void)s; return 0; }
#define sleep omc_win_sleep
unsigned omc_win_sleep(unsigned s);

/* ---------- strings the CRT lacks ---------- */
#define memmem omc_win_memmem
#define memrchr omc_win_memrchr
#define strcasestr omc_win_strcasestr
#define strsep omc_win_strsep
#define strsignal omc_win_strsignal
#define strerror_r(e, b, n) (strerror_s((b), (n), (e)), 0)
#define strchrnul omc_win_strchrnul
void *omc_win_memmem(const void *h, size_t hn, const void *n, size_t nn);
void *omc_win_memrchr(const void *s, int c, size_t n);
char *omc_win_strcasestr(const char *h, const char *n);
char *omc_win_strsep(char **s, const char *delim);
char *omc_win_strsignal(int sig);
char *omc_win_strchrnul(const char *s, int c);
#define strndup omc_win_strndup
char *omc_win_strndup(const char *s, size_t n);

/* ---------- signals ---------- */
/* the numbers Linux uses, for the ones the CRT has no value for; only SIGINT/SIGTERM/SIGBREAK/SIGABRT/SIGSEGV/SIGFPE/
 * SIGILL are ever raised on Windows (sig.c), the rest exist so kill() and the handlers can name them */
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGQUIT
#define SIGQUIT 3
#endif
#ifndef SIGTRAP
#define SIGTRAP 5
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGBUS
#define SIGBUS 7
#endif
#ifndef SIGUSR1
#define SIGUSR1 30
#endif
#ifndef SIGUSR2
#define SIGUSR2 31
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGALRM
#define SIGALRM 14
#endif
#ifndef SIGCHLD
#define SIGCHLD 17
#endif
#ifndef SIGCONT
#define SIGCONT 18
#endif
#ifndef SIGSTOP
#define SIGSTOP 19
#endif
#ifndef SIGTSTP
#define SIGTSTP 20
#endif
#ifndef SIGTTIN
#define SIGTTIN 24
#endif
#ifndef SIGTTOU
#define SIGTTOU 25
#endif
#ifndef SIGWINCH
#define SIGWINCH 28
#endif
#ifndef SIGXCPU
#define SIGXCPU 32
#endif
#ifndef SIGXFSZ
#define SIGXFSZ 33
#endif
#ifndef SIGVTALRM
#define SIGVTALRM 34
#endif
#ifndef SIGPROF
#define SIGPROF 35
#endif
#ifndef SIGSYS
#define SIGSYS 12
#endif
#undef NSIG
#define NSIG 36
#define SA_RESTART 0x10000000
#define SA_SIGINFO 0x4
#define SA_RESETHAND 0x80000000
#define SA_NODEFER 0x40000000
#define SA_ONSTACK 0x08000000
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
typedef struct {
    int si_signo, si_code, si_errno;
    int si_pid, si_status;
    void *si_addr;
} siginfo_t;
typedef struct {
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
} stack_t;
#define SIGSTKSZ 16384
static inline int sigaltstack(const stack_t *n, stack_t *o) { (void)n; (void)o; return 0; }
struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;
    int sa_flags;
};
/* `struct sigaction` and sigaction() share a name: the struct keeps it, calls go through the function-like macro */
int omc_win_sigaction(int sig, const struct sigaction *act, struct sigaction *old);
#define sigaction(s, a, o) omc_win_sigaction((s), (a), (o))
#define signal omc_win_signal
typedef void (*omc_win_sighandler_t)(int);
omc_win_sighandler_t omc_win_signal(int sig, omc_win_sighandler_t h);
#define raise omc_win_raise
int omc_win_raise(int sig);
#define kill omc_win_kill
int omc_win_kill(int pid, int sig);
static inline int sigemptyset(sigset_t *s) { *s = 0; return 0; }
static inline int sigfillset(sigset_t *s) { *s = ~(sigset_t)0; return 0; }
static inline int sigaddset(sigset_t *s, int n) { *s |= (sigset_t)1 << n; return 0; }
static inline int sigdelset(sigset_t *s, int n) { *s &= ~((sigset_t)1 << n); return 0; }
static inline int sigismember(const sigset_t *s, int n) { return (int)((*s >> n) & 1); }
#define pause omc_win_pause
int omc_win_pause(void);
#define sigprocmask(h, s, o) omc_win_sigprocmask((h), (s), (o))
#undef pthread_sigmask
#define pthread_sigmask(h, s, o) omc_win_sigprocmask((h), (s), (o))
int omc_win_sigprocmask(int how, const sigset_t *set, sigset_t *old);
#define _exit omc_win__exit
__attribute__((noreturn)) void omc_win__exit(int code);

/* ---------- processes ---------- */
#define WNOHANG 1
#define WUNTRACED 2
#define WEXITED 4
#define WNOWAIT 0x01000000
#define P_PID 1
#define CLD_EXITED 1
#define CLD_KILLED 2
#define CLD_DUMPED 3
#define waitid omc_win_waitid
int omc_win_waitid(int idtype, id_t id, siginfo_t *si, int options);
#define getpgrp() getpid()
#define setsid() getpid()
#define setpgid(a, b) 0
#define getpgid(p) (p)

/* ---------- misc ---------- */
#define MSG_NOSIGNAL 0
#define usleep omc_win_usleep
int omc_win_usleep(unsigned long long us);

/* process start-up for the omc executable (console code pages, binary stdio, Winsock, Ctrl+C); idempotent */
void omc_win_init(void);
/* omc_win_init plus what only the omc executable does to its own process (console code pages, binary stdio, VT
 * output, crash reports) */
void omc_win_init_exe(void);
/* VT escape processing on the console omc writes to */
void omc_win_console_vt(void);

/* ---------- Windows-only process API for omc's own call sites (plat/win/proc.c) ---------- */
#define OMC_WIN_INHERIT (-2)
typedef struct {
    const char *exe;           /* the program, when argv[0] is only the name it should see */
    const char *cwd;           /* NULL: omc's */
    char *const *envp;         /* NULL: omc's environment */
    int fd_in, fd_out, fd_err; /* -1: the NUL device; OMC_WIN_INHERIT: omc's own */
    bool detached;             /* no console, own process group, outside omc's job: outlives omc */
    bool hidden;               /* windowsHide: a console of its own that is never shown */
    bool verbatim;             /* argv[1..] are the command line tail as is (cmd.exe /s /c "...") */
} omc_win_spawn_opts_t;
/* start argv (argv[0] searched on PATH with PATHEXT); the pid for waitpid/kill, or -1 with errno */
int omc_win_spawn(char *const argv[], const omc_win_spawn_opts_t *o);
/* start argv on a pseudo console of cols x rows: *in_fd feeds its keyboard, *out_fd reads its screen (VT) */
int omc_win_pty_spawn(char *const argv[], const char *cwd, char *const envp[], int cols, int rows, int *in_fd, int *out_fd);
int omc_win_pty_resize(int pid, int cols, int rows);
/* a child of ours: its whole tree ends when this process does (job kill-on-close) */
int omc_win_job_kill_on_close(int pid);
void omc_win_pty_close(int pid);
/* the executable a command name runs (PATH + PATHEXT), '/' separators; owned, NULL if none */
char *omc_win_find_exe(const char *name);

/* clipboard (clip.c): text as UTF-8, the registered "PNG" format, or the CF_DIB bitmap (decode with img_dib_to_png) */
enum { OMC_CLIP_TEXT = 0, OMC_CLIP_PNG = 1, OMC_CLIP_DIB = 2 };
bool omc_win_clipboard_has(int kind); /* OMC_CLIP_DIB: any bitmap format */
/* 1 with *out (malloc'd; text NUL-terminated) and *len, 0 when the clipboard holds no such data, -1 when the clipboard
 * could not be opened (another program holds it) */
int omc_win_clipboard_read(int kind, char **out, size_t *len);
int omc_win_clipboard_set_text(const char *utf8, size_t n); /* 0, or -1 */

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif
