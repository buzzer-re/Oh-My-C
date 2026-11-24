/* plat/win/win.h - internals shared by the Windows platform layer (the sources in src/plat/win); nothing outside it
 * includes this */
#ifndef OMC_PLAT_WIN_WIN_H
#define OMC_PLAT_WIN_WIN_H

/* Winsock declares gethostname (dllimport): it must keep its own name, posix.h maps omc's calls elsewhere */
#pragma push_macro("gethostname")
#undef gethostname
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma pop_macro("gethostname")

/* pseudo consoles (Windows 10 1809): declared here for SDK headers that predate them */
typedef void *omc_hpcon_t;
#define HPCON omc_hpcon_t
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

/* ---------- UTF-8 <-> UTF-16 ---------- */
/* malloc'd conversions; NULL in -> NULL out. win_path also turns '/' into '\' and maps /dev/null to NUL. */
wchar_t *win_wide(const char *s);
wchar_t *win_wide_n(const char *s, size_t n);
char *win_utf8(const wchar_t *w);
char *win_utf8_n(const wchar_t *w, size_t n);
wchar_t *win_path(const char *path);
/* backslashes to forward slashes, in place */
void win_slashes(char *s);

/* ---------- errors ---------- */
int win_errno_from(DWORD err);   /* GetLastError() value -> errno */
int win_errno_from_wsa(int err); /* WSAGetLastError() value -> errno */
static inline int win_fail(DWORD err) {
    errno = win_errno_from(err);
    return -1;
}

/* ---------- fd table (fd.c) ---------- */
enum { FDK_UNKNOWN, FDK_FILE, FDK_PIPE, FDK_CONSOLE, FDK_CHAR, FDK_SOCKET };
int win_fd_kind(int fd);
HANDLE win_fd_get_handle(int fd);
SOCKET win_fd_socket(int fd);
/* the CRT fd for a socket (a NUL handle stands in for it in the CRT table); -1 with errno on failure */
int win_fd_new_socket(SOCKET s, bool nonblock);
void win_fd_set_nonblock(int fd, bool on);
bool win_fd_nonblock(int fd);
void win_fd_forget(int fd);

/* ---------- processes (proc.c) ---------- */
typedef struct {
    const char *exe; /* the program, when argv[0] is only the name it should see (bash's `bash: line 1:`) */
    const char *cwd;
    char *const *envp; /* NULL: this process's environment */
    HANDLE h_in, h_out, h_err; /* NULL: the NUL device */
    bool detached;             /* no console, own process group, out of omc's job: outlives omc */
    bool new_group;            /* CREATE_NEW_PROCESS_GROUP */
    bool new_console_hidden;   /* a console of its own, never shown (windowsHide) */
    bool verbatim;             /* argv[1] is the whole command line tail, passed as is (cmd.exe /s /c "...") */
    HPCON pcon;                /* attach to this pseudo console instead of h_in/h_out/h_err */
} win_spawn_t;
/* starts argv[0] (searched on PATH with PATHEXT unless it has a directory part); returns the pid and registers the child
 * for waitpid/kill, or -1 with errno */
int win_spawn(char *const argv[], const win_spawn_t *o);
/* the executable argv0 resolves to (owned), NULL if none */
char *win_find_exe(const char *argv0);

/* ---------- console (term.c, sig.c) ---------- */
void win_console_note_resize(void);
bool win_console_take_resize(void);
void win_sig_deliver(int sig); /* runs the handler installed for sig (or its default action) */

#endif
