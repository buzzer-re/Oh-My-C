/* pty.h for Windows: no POSIX ptys; a pseudo console (ConPTY) is reached through plat/win/proc.c's spawn instead.
 * openpty always fails with ENOSYS, so a caller that still reaches it reports why. */
#ifndef OMC_WIN_PTY_H
#define OMC_WIN_PTY_H
#include <termios.h>
#define openpty omc_win_openpty
int omc_win_openpty(int *master, int *slave, char *name, const struct termios *t, const struct winsize *ws);
#endif
