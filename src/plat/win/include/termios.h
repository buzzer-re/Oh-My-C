/* termios.h for Windows: the console's modes behind the termios calls omc makes. tcsetattr maps ICANON/ECHO/ISIG onto
 * ENABLE_LINE_INPUT/ECHO_INPUT/PROCESSED_INPUT and keeps VT input/output on (plat/win/term.c). */
#ifndef OMC_WIN_TERMIOS_H
#define OMC_WIN_TERMIOS_H
typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;
#define NCCS 32
struct termios {
    tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
    cc_t c_line;
    cc_t c_cc[NCCS];
    speed_t c_ispeed, c_ospeed;
    unsigned int win_in_mode, win_out_mode; /* the console modes this snapshot was taken from */
    int win_valid;
};
struct winsize {
    unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
};
/* c_iflag */
#define IGNBRK 0x1
#define BRKINT 0x2
#define IGNPAR 0x4
#define PARMRK 0x8
#define INPCK 0x10
#define ISTRIP 0x20
#define INLCR 0x40
#define IGNCR 0x80
#define ICRNL 0x100
#define IXON 0x400
#define IXANY 0x800
#define IXOFF 0x1000
#define IMAXBEL 0x2000
#define IUTF8 0x4000
/* c_oflag */
#define OPOST 0x1
#define ONLCR 0x4
/* c_cflag */
#define CSIZE 0x30
#define CS8 0x30
#define CREAD 0x80
#define PARENB 0x100
#define HUPCL 0x400
#define CLOCAL 0x800
/* c_lflag */
#define ISIG 0x1
#define ICANON 0x2
#define ECHO 0x8
#define ECHOE 0x10
#define ECHOK 0x20
#define ECHONL 0x40
#define NOFLSH 0x80
#define TOSTOP 0x100
#define ECHOCTL 0x200
#define IEXTEN 0x8000
/* c_cc */
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSTART 8
#define VSTOP 9
#define VSUSP 10
/* tcsetattr */
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2
#define TCIFLUSH 0
#define TCOFLUSH 1
#define TCIOFLUSH 2
#define B38400 15
#define tcgetattr omc_win_tcgetattr
#define tcsetattr omc_win_tcsetattr
#define cfmakeraw omc_win_cfmakeraw
#define tcflush omc_win_tcflush
#define tcdrain(fd) 0
#define cfgetospeed(t) B38400
#define cfsetospeed(t, s) 0
#define cfsetispeed(t, s) 0
#define tcgetpgrp(fd) getpid()
#define tcsetpgrp(fd, p) 0
int omc_win_tcgetattr(int fd, struct termios *t);
int omc_win_tcsetattr(int fd, int when, const struct termios *t);
void omc_win_cfmakeraw(struct termios *t);
int omc_win_tcflush(int fd, int queue);
#endif
