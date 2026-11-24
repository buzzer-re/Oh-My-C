/* sys/ioctl.h for Windows: TIOCGWINSZ (console window size), FIONREAD (bytes waiting on a pipe/socket/console) */
#ifndef OMC_WIN_SYS_IOCTL_H
#define OMC_WIN_SYS_IOCTL_H
#include <termios.h>
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCSCTTY 0x540E
#define TIOCNOTTY 0x5422
#undef FIONREAD
#undef FIONBIO
#define FIONREAD 0x541B
#define FIONBIO 0x5421
#define ioctl omc_win_ioctl
int omc_win_ioctl(int fd, unsigned long req, ...);
#endif
