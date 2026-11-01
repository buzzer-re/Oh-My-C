/* compat.h - Linux/glibc names omc uses, provided on macOS. Included by core.h, so it must stay free of
 * system includes (a global <sys/socket.h> would clash with local helpers named bind/listen/...).
 * Function shims (pipe2, SOCK_CLOEXEC sockets, getrandom, execvpe) live in core/compat_sys.h, which
 * only the files that call them include. */
#ifndef OMC_COMPAT_H
#define OMC_COMPAT_H

#if defined(__APPLE__)
/* struct stat timestamps: Linux st_mtim / st_atim / st_ctim are st_*timespec on Darwin */
#define st_mtim st_mtimespec
#define st_atim st_atimespec
#define st_ctim st_ctimespec
/* send(..., MSG_NOSIGNAL): omc ignores SIGPIPE process-wide (proc_ignore_sigpipe); sockets also get
 * SO_NOSIGPIPE in compat_sys.h */
#define MSG_NOSIGNAL 0
#endif

#endif
