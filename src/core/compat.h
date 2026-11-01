/* compat.h - the few Linux/glibc interfaces omc uses, provided on macOS (and other BSD-likes).
 * Included by core.h, so every translation unit sees the same names on every platform. */
#ifndef OMC_COMPAT_H
#define OMC_COMPAT_H

#if defined(__APPLE__)

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* struct stat timestamps: Linux st_mtim / st_atim / st_ctim are st_*timespec on Darwin */
#ifndef st_mtim
#define st_mtim st_mtimespec
#define st_atim st_atimespec
#define st_ctim st_ctimespec
#endif

/* send(..., MSG_NOSIGNAL): omc ignores SIGPIPE process-wide (proc_ignore_sigpipe), so a no-op flag is safe */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* pipe2(fds, O_CLOEXEC | O_NONBLOCK) */
static inline int omc_compat_pipe2(int fds[2], int flags) {
    if (pipe(fds) != 0) return -1;
    for (int i = 0; i < 2; i++) {
        if ((flags & O_CLOEXEC) && fcntl(fds[i], F_SETFD, FD_CLOEXEC) != 0) goto fail;
        if ((flags & O_NONBLOCK) && fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK) != 0) goto fail;
    }
    return 0;
fail:;
    int e = errno;
    close(fds[0]);
    close(fds[1]);
    errno = e;
    return -1;
}
#define pipe2(fds, flags) omc_compat_pipe2((fds), (flags))

/* socket(domain, type | SOCK_CLOEXEC | SOCK_NONBLOCK, proto) */
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0x10000000
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0x20000000
#endif
static inline int omc_compat_socket(int domain, int type, int proto) {
    int fd = socket(domain, type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), proto);
    if (fd < 0) return fd;
    if (type & SOCK_CLOEXEC) fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (type & SOCK_NONBLOCK) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); /* stands in for MSG_NOSIGNAL */
    return fd;
}
#define socket(d, t, p) omc_compat_socket((d), (t), (p))

/* getrandom(buf, len, flags) */
static inline ssize_t omc_compat_getrandom(void *buf, size_t len, unsigned flags) {
    (void)flags;
    arc4random_buf(buf, len);
    return (ssize_t)len;
}
#define getrandom(b, n, f) omc_compat_getrandom((b), (n), (f))

/* execvpe(file, argv, envp): search PATH with a replacement environment */
extern char **environ;
static inline int omc_compat_execvpe(const char *file, char *const argv[], char *const envp[]) {
    environ = (char **)envp;
    return execvp(file, argv);
}
#define execvpe(f, a, e) omc_compat_execvpe((f), (a), (e))

#endif /* __APPLE__ */

#endif
